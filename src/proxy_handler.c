/*
 * proxy_handler.c - 리버스 프록시
 *
 * proxy_handle() 흐름
 *   1) Round-Robin 으로 백엔드 선택
 *   2) non-blocking connect + poll 로 "타임아웃 있는 connect"   (실패 502 / 타임아웃 504)
 *   3) 요청 재조립: hop-by-hop 헤더 제거, X-Forwarded-For 추가, Connection: close
 *   4) 백엔드 응답 헤더 수신 (\r\n\r\n 까지)                    (끊김 502 / 타임아웃 504)
 *   5) 응답 헤더 재조립: Connection 계열 제거 후 클라이언트 쪽 Connection 값 부여
 *   6) 본문 중계: Content-Length 가 있으면 정확히 그만큼 (-> 클라이언트 keep-alive 유지),
 *                없으면 백엔드가 닫을 때까지 (-> 클라이언트 연결도 닫음)
 */
#include "proxy_handler.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PROXY_REQ_HEAD_MAX   (HTTP_MAX_HEADER_SIZE + 2048)
#define PROXY_RESP_HEAD_MAX  16384

/* ------------------------------------------------------------------ */
/* 라우팅 테이블                                                       */
/* ------------------------------------------------------------------ */

int proxy_table_init(proxy_table_t *table, const server_config_t *cfg)
{
    memset(table, 0, sizeof(*table));
    table->connect_timeout_ms = cfg->proxy_connect_timeout_sec * 1000;
    table->io_timeout_ms      = cfg->proxy_timeout_sec * 1000;

    for (size_t i = 0; i < cfg->route_count; i++) {
        proxy_route_t *r = &table->routes[i];
        memcpy(r->prefix, cfg->routes[i].prefix, sizeof(r->prefix));
        r->prefix_len    = strlen(r->prefix);
        memcpy(r->backends, cfg->routes[i].backends, sizeof(r->backends));
        r->backend_count = cfg->routes[i].backend_count;
        r->next          = 0;
        if (pthread_mutex_init(&r->lock, NULL) != 0) {
            proxy_table_destroy(table);
            return -1;
        }
        table->route_count++;
    }
    return 0;
}

void proxy_table_destroy(proxy_table_t *table)
{
    for (size_t i = 0; i < table->route_count; i++)
        pthread_mutex_destroy(&table->routes[i].lock);
    table->route_count = 0;
}

proxy_route_t *proxy_match(proxy_table_t *table, const char *path)
{
    proxy_route_t *best = NULL;

    for (size_t i = 0; i < table->route_count; i++) {
        proxy_route_t *r = &table->routes[i];
        if (strncmp(path, r->prefix, r->prefix_len) == 0 &&
            (best == NULL || r->prefix_len > best->prefix_len))
            best = r;
    }
    return best;
}

size_t proxy_next_backend(proxy_route_t *route)
{
    pthread_mutex_lock(&route->lock);
    size_t idx  = route->next;
    route->next = (route->next + 1) % route->backend_count;
    pthread_mutex_unlock(&route->lock);
    return idx;
}

/* ------------------------------------------------------------------ */
/* 작은 유틸                                                           */
/* ------------------------------------------------------------------ */

static int is_timeout_errno(int e)
{
    return e == EAGAIN || e == EWOULDBLOCK || e == ETIMEDOUT;
}

static struct timeval ms_to_timeval(int ms)
{
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return tv;
}

static long find_crlfcrlf(const char *buf, size_t len)
{
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && memcmp(buf + i, "\r\n\r\n", 4) == 0)
            return (long)i;
    }
    return -1;
}

/* [p, limit) 범위에서 "\r\n" 위치. 없으면 NULL.
 * 백엔드 응답 버퍼는 NUL 종료가 아니므로 strstr 대신 길이 제한 검색을 쓴다. */
static const char *find_crlf(const char *p, const char *limit)
{
    for (; p + 1 < limit; p++) {
        if (p[0] == '\r' && p[1] == '\n')
            return p;
    }
    return NULL;
}

/* 헤더 이름 name[0..nlen) 이 target 과 같은지 (대소문자 무시) */
static int name_is(const char *name, size_t nlen, const char *target)
{
    return strlen(target) == nlen && strncasecmp(name, target, nlen) == 0;
}

/* RFC 7230 6.1: 프록시가 다음 홉으로 넘기면 안 되는 연결 단위 헤더 */
static int is_hop_by_hop(const char *name, size_t nlen)
{
    static const char *const hop[] = {
        "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer",
        "Transfer-Encoding", "Upgrade", "Proxy-Authorization", "Proxy-Authenticate",
    };
    for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); i++) {
        if (name_is(name, nlen, hop[i]))
            return 1;
    }
    return 0;
}

static int reply_error(int client_fd, http_request_t *req, int status)
{
    if (http_send_error(client_fd, status, NULL, req->keep_alive) < 0)
        req->keep_alive = 0;
    return status;
}

/* ------------------------------------------------------------------ */
/* 1) 타임아웃 있는 connect                                             */
/* ------------------------------------------------------------------ */

static int connect_backend(const struct sockaddr_in *addr, int connect_ms, int io_ms,
                           int *status_out, int *err_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *err_out = errno;
        *status_out = 502;
        return -1;
    }

    /* 그냥 connect 하면 응답 없는 호스트에서 OS 기본값(수십 초~수 분)만큼 막힌다.
     * non-blocking 으로 시작하고 poll 로 원하는 시간만 기다린다. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail_502;

    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
        if (errno != EINPROGRESS)
            goto fail_errno;

        struct pollfd p = { .fd = fd, .events = POLLOUT, .revents = 0 };
        int pr;
        do {
            pr = poll(&p, 1, connect_ms);
        } while (pr < 0 && errno == EINTR);
        if (pr == 0) {
            *err_out = ETIMEDOUT;
            *status_out = 504;
            close(fd);
            return -1;
        }
        if (pr < 0)
            goto fail_errno;

        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0)
            goto fail_errno;
        if (soerr != 0) {
            errno = soerr;
            goto fail_errno;
        }
    }

    /* 연결 후에는 다시 blocking + 송수신 타임아웃 */
    if (fcntl(fd, F_SETFL, flags) < 0)
        goto fail_502;
    struct timeval tv = ms_to_timeval(io_ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;

fail_errno:
    *err_out = errno;
    *status_out = is_timeout_errno(errno) ? 504 : 502;   /* ECONNREFUSED 등은 502 */
    close(fd);
    return -1;
fail_502:
    *err_out = errno;
    *status_out = 502;
    close(fd);
    return -1;
}

/* ------------------------------------------------------------------ */
/* 3) 백엔드로 보낼 요청 헤더 재조립                                     */
/* ------------------------------------------------------------------ */

/* buf[*len..cap) 뒤에 printf 형식으로 덧붙인다. 넘치면 -1. */
__attribute__((format(printf, 4, 5)))
static int appendf(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len)
        return -1;
    *len += (size_t)n;
    return 0;
}

static int build_backend_request(const http_request_t *req, const char *client_ip,
                                 char *out, size_t cap, size_t *out_len)
{
    size_t len = 0;
    const char *conn_hdr = http_get_header(req, "Connection");
    const char *xff      = http_get_header(req, "X-Forwarded-For");

    /* 요청 줄: 경로는 바꾸지 않고 원본 target 그대로 전달 */
    if (appendf(out, cap, &len, "%s %s HTTP/1.1\r\n", req->method_str, req->target) < 0)
        return -1;

    for (size_t i = 0; i < req->header_count; i++) {
        const char *name = req->headers[i].name;
        size_t nlen = strlen(name);

        if (is_hop_by_hop(name, nlen) ||
            name_is(name, nlen, "X-Forwarded-For") ||
            name_is(name, nlen, "Expect") ||                  /* 100-continue 는 이미 처리함 */
            http_header_has_token(conn_hdr, name))            /* Connection: 에 나열된 헤더 */
            continue;
        if (appendf(out, cap, &len, "%s: %s\r\n", name, req->headers[i].value) < 0)
            return -1;
    }

    /* 원래 클라이언트 IP 를 백엔드가 알 수 있도록 */
    if (xff != NULL) {
        if (appendf(out, cap, &len, "X-Forwarded-For: %s, %s\r\n", xff, client_ip) < 0)
            return -1;
    } else if (appendf(out, cap, &len, "X-Forwarded-For: %s\r\n", client_ip) < 0) {
        return -1;
    }
    if (http_get_header(req, "X-Forwarded-Proto") == NULL &&
        appendf(out, cap, &len, "X-Forwarded-Proto: http\r\n") < 0)
        return -1;

    /* 백엔드와는 요청 1개 후 닫는다 (백엔드 연결 풀은 범위 밖) */
    if (appendf(out, cap, &len, "Connection: close\r\n\r\n") < 0)
        return -1;

    *out_len = len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 4~6) 응답 중계                                                       */
/* ------------------------------------------------------------------ */

typedef enum { BODY_NONE, BODY_LENGTH, BODY_UNTIL_CLOSE } body_mode_t;

/* 백엔드 응답 헤더 블록 resp[0..head_len) 을 클라이언트용으로 재조립.
 * 성공 시 상태 코드, 실패 시 -1 (형식 오류 -> 호출자가 502). */
static int rewrite_response_head(const char *resp, size_t head_len, char *out, size_t cap,
                                 size_t *out_len, body_mode_t *mode, unsigned long long *clen,
                                 int *keep_alive)
{
    /* 상태 줄: "HTTP/1.x SSS reason" */
    if (head_len < 12 || strncmp(resp, "HTTP/1.", 7) != 0 || resp[8] != ' ')
        return -1;
    for (int i = 9; i < 12; i++) {
        if (resp[i] < '0' || resp[i] > '9')
            return -1;
    }
    int status = (resp[9] - '0') * 100 + (resp[10] - '0') * 10 + (resp[11] - '0');

    const char *limit = resp + head_len;
    const char *end   = limit - 2;                  /* 마지막 빈 줄의 \r\n 위치 */
    const char *eol   = find_crlf(resp, limit);
    if (eol == NULL || eol > end)
        return -1;

    size_t len = 0;
    size_t status_len = (size_t)(eol - resp) + 2;
    if (status_len >= cap)
        return -1;
    memcpy(out, resp, status_len);
    len = status_len;

    int have_cl = 0, have_te = 0;
    unsigned long long cl = 0;

    const char *line = eol + 2;
    while (line < end) {
        const char *le = find_crlf(line, limit);
        if (le == NULL || le > end)
            return -1;
        const char *colon = memchr(line, ':', (size_t)(le - line));
        if (colon == NULL || colon == line)
            return -1;
        size_t nlen = (size_t)(colon - line);

        if (name_is(line, nlen, "Content-Length")) {
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            unsigned long long val = 0;
            int digits = 0;
            while (v < le && *v >= '0' && *v <= '9') {
                val = val * 10 + (unsigned long long)(*v - '0');
                if (val > (1ULL << 50))
                    return -1;
                v++;
                digits++;
            }
            while (v < le && (*v == ' ' || *v == '\t'))
                v++;
            if (digits == 0 || v != le || (have_cl && val != cl))
                return -1;
            have_cl = 1;
            cl = val;
        } else if (name_is(line, nlen, "Transfer-Encoding")) {
            have_te = 1;
        }

        /* 연결 관리 헤더는 우리가 클라이언트 쪽 값으로 다시 쓴다 */
        if (!name_is(line, nlen, "Connection") && !name_is(line, nlen, "Keep-Alive") &&
            !name_is(line, nlen, "Proxy-Connection")) {
            size_t l = (size_t)(le - line) + 2;
            if (len + l >= cap)
                return -1;
            memcpy(out + len, line, l);
            len += l;
        }
        line = le + 2;
    }

    if (status == 204 || status == 304) {
        *mode = BODY_NONE;
    } else if (status < 200 || have_te || !have_cl) {
        *mode = BODY_UNTIL_CLOSE;                    /* 끝을 알 수 없음 -> 닫아서 알린다 */
        *keep_alive = 0;
    } else {
        *mode = BODY_LENGTH;
        *clen = cl;
    }

    int n = snprintf(out + len, cap - len, "Connection: %s\r\n\r\n",
                     *keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= cap - len)
        return -1;
    len += (size_t)n;

    *out_len = len;
    return status;
}

static ssize_t recv_retry(int fd, void *buf, size_t cap)
{
    for (;;) {
        ssize_t n = recv(fd, buf, cap, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

int proxy_handle(int client_fd, http_request_t *req, proxy_table_t *table,
                 proxy_route_t *route, const char *client_ip)
{
    size_t idx = proxy_next_backend(route);
    const backend_addr_t *be = &route->backends[idx];
    int status = 502, err = 0;

    /* 1~2) 연결 */
    int bfd = connect_backend(&be->addr, table->connect_timeout_ms, table->io_timeout_ms,
                              &status, &err);
    if (bfd < 0) {
        fprintf(stderr, "[WARN] proxy %s: connect failed (errno=%d) -> %d\n", be->text, err, status);
        return reply_error(client_fd, req, status);
    }

    /* 3) 요청 전달 */
    char   reqhead[PROXY_REQ_HEAD_MAX];
    size_t reqlen = 0;
    if (build_backend_request(req, client_ip, reqhead, sizeof(reqhead), &reqlen) < 0) {
        close(bfd);
        return reply_error(client_fd, req, 500);
    }
    if (http_send_all(bfd, reqhead, reqlen) < 0 ||
        (req->body_len > 0 && http_send_all(bfd, req->body, req->body_len) < 0)) {
        status = is_timeout_errno(errno) ? 504 : 502;
        fprintf(stderr, "[WARN] proxy %s: send failed (errno=%d) -> %d\n", be->text, errno, status);
        close(bfd);
        return reply_error(client_fd, req, status);
    }

    /* 4) 응답 헤더 수신 */
    char   resp[PROXY_RESP_HEAD_MAX];
    size_t rlen = 0;
    long   end;
    while ((end = find_crlfcrlf(resp, rlen)) < 0) {
        if (rlen == sizeof(resp)) {
            fprintf(stderr, "[WARN] proxy %s: response head too large -> 502\n", be->text);
            close(bfd);
            return reply_error(client_fd, req, 502);
        }
        ssize_t n = recv_retry(bfd, resp + rlen, sizeof(resp) - rlen);
        if (n <= 0) {
            status = (n < 0 && is_timeout_errno(errno)) ? 504 : 502;
            fprintf(stderr, "[WARN] proxy %s: %s before response -> %d\n", be->text,
                    n == 0 ? "connection closed" : "recv failed/timed out", status);
            close(bfd);
            return reply_error(client_fd, req, status);
        }
        rlen += (size_t)n;
    }
    size_t head_len = (size_t)end + 4;

    /* 5) 응답 헤더 재조립 후 전송 */
    char   out[PROXY_RESP_HEAD_MAX + 64];
    size_t outlen = 0;
    body_mode_t mode = BODY_NONE;
    unsigned long long clen = 0;
    status = rewrite_response_head(resp, head_len, out, sizeof(out), &outlen, &mode, &clen,
                                   &req->keep_alive);
    if (status < 0) {
        fprintf(stderr, "[WARN] proxy %s: malformed response head -> 502\n", be->text);
        close(bfd);
        return reply_error(client_fd, req, 502);
    }
    if (http_send_all(client_fd, out, outlen) < 0) {
        req->keep_alive = 0;
        close(bfd);
        return status;
    }

    /* 6) 본문 중계 — 헤더와 함께 받아 둔 조각부터 */
    const char *extra = resp + head_len;
    size_t extra_len  = rlen - head_len;

    if (mode == BODY_LENGTH) {
        unsigned long long remaining = clen;
        size_t first = extra_len < remaining ? extra_len : (size_t)remaining;
        if (first > 0 && http_send_all(client_fd, extra, first) < 0)
            goto abort_stream;
        remaining -= first;

        char chunk[HTTP_IO_CHUNK_SIZE];
        while (remaining > 0) {
            size_t want = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
            ssize_t n = recv_retry(bfd, chunk, want);
            if (n <= 0)                                    /* 백엔드가 약속보다 일찍 끊김 */
                goto abort_stream;
            if (http_send_all(client_fd, chunk, (size_t)n) < 0)
                goto abort_stream;
            remaining -= (unsigned long long)n;
        }
    } else if (mode == BODY_UNTIL_CLOSE) {
        if (extra_len > 0 && http_send_all(client_fd, extra, extra_len) < 0)
            goto abort_stream;
        char chunk[HTTP_IO_CHUNK_SIZE];
        for (;;) {
            ssize_t n = recv_retry(bfd, chunk, sizeof(chunk));
            if (n == 0)
                break;
            if (n < 0 || http_send_all(client_fd, chunk, (size_t)n) < 0)
                goto abort_stream;
        }
    }

    close(bfd);
    return status;

abort_stream:
    /* 헤더는 이미 보냈으므로 에러 페이지로 바꿀 수 없다 -> 연결을 끊어 실패를 알린다 */
    fprintf(stderr, "[WARN] proxy %s: body relay aborted\n", be->text);
    req->keep_alive = 0;
    close(bfd);
    return status;
}
