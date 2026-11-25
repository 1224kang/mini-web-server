#include "conn.h"
#include "builtin.h"
#include "cgi.h"
#include "http.h"
#include "static_files.h"
#include "upstream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* PATH_MAX, LLONG_MAX */
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* 시간 제한: "이 연결이 서버 자원을 얼마나 오래 붙잡을 수 있나"의 상한 */
#define IDLE_TIMEOUT_MS   5000  /* keep-alive: 다음 요청의 첫 바이트를 기다리는 최대 시간 */
#define HEADER_TIMEOUT_MS 10000 /* 첫 바이트부터 "\r\n\r\n" 까지 (slowloris 방어) */
#define BODY_TIMEOUT_MS   30000 /* 본문 전체를 받는 최대 시간 */
#define SEND_TIMEOUT_MS   10000 /* 한 바이트도 못 보내고 버틸 수 있는 시간 (안 읽는 클라이언트) */
#define LINGER_TIMEOUT_MS 1000  /* 닫기 전 남은 입력을 비우는 최대 시간 */
#define LINGER_MAX_BYTES  (64 * 1024)
/* CGI 타임아웃은 설정 파일에서 (config.h) */
#define UPSTREAM_MAX_RESPONSE (16 * 1024 * 1024) /* 업스트림 응답을 통째로 모아 둘 최대 크기 */
#define MAX_REQUESTS_PER_CONN 100

#define FILE_CHUNK   (64 * 1024)  /* 파일을 이만큼씩 읽어 보낸다 */
#define WRITE_BUDGET (256 * 1024) /* 이벤트 한 번에 한 연결이 보낼 수 있는 최대 바이트.
                                   * 이게 없으면 빠른 클라이언트 하나가 20MB 를 다 받을 때까지
                                   * 루프를 독점해서 다른 연결이 전부 멈춘다. */

enum conn_state {
    ST_READ_HEAD, /* 요청 헤더를 모으는 중 (len == 0 이면 "다음 요청을 기다리며 노는 중") */
    ST_READ_BODY, /* Content-Length 만큼 본문을 모으는 중 */
    ST_UPSTREAM,  /* CGI 에 넘기고 답을 기다리는 중 (5단계) */
    ST_WRITE,     /* 응답(헤더 + 본문 또는 파일)을 내보내는 중 */
    ST_LINGER,    /* FIN 을 보냈고, 상대가 닫을 때까지 남은 입력을 버리는 중 */
    ST_DONE,      /* 루프가 정리하면 됨 */
};

/* 스레드 풀 버전에선 이 정보 대부분이 "워커 스택의 지역 변수 + 지금 실행 중인 줄"에
 * 자연스럽게 들어 있었다. 이벤트 루프에선 중간에 손을 놓고 루프로 돌아가야 하므로
 * "어디까지 했는지"를 전부 여기 적어 둬야 한다. 이게 상태 머신이다. */
struct conn {
    int fd;
    unsigned long id;
    struct server_config *cfg;
    char peer_ip[INET_ADDRSTRLEN];
    int peer_port;
    enum conn_state state;
    long long deadline; /* 지금 상태의 마감 시각 */
    int stopping;       /* 서버 종료 중이면 1 → 이번 응답이 마지막 */
    int served;         /* 이 연결에서 끝낸 요청 수 */

    /* ---- 받기 ---- */
    char buf[HTTP_MAX_HEAD]; /* 헤더 (+ 딸려 온 본문 앞부분, 파이프라이닝된 다음 요청) */
    size_t len;              /* buf 에 든 바이트 수 */
    size_t scanned;          /* buf 앞에서부터 여기까지는 "\r\n\r\n" 이 없다고 확인함 */
    struct http_request req; /* 파싱 결과. 문자열은 buf 안을 가리킨다 */
    int head_only;           /* HEAD 요청: 헤더는 GET 과 같게, 본문만 빼고 보낸다 */
    size_t consumed;         /* 이번 요청이 buf 에서 차지한 바이트 (응답 후 버림) */
    char *body;
    size_t body_len, body_have;

    /* ---- 보내기 ---- */
    int keep_alive;
    char *out;              /* 상태 라인 + 헤더 (+ 작은 본문) */
    size_t out_len, out_sent;
    int file_fd;            /* 파일 본문을 보내는 중이면 열린 fd, 아니면 -1 */
    off_t file_left;        /* 아직 read 하지 않은 파일 바이트 */
    char *chunk;            /* 파일에서 읽었지만 아직 다 못 보낸 조각 */
    size_t chunk_len, chunk_sent;
    char summary[PATH_MAX + 64]; /* 응답을 다 보낸 뒤 찍을 로그 */

    size_t drained; /* LINGER 에서 버린 바이트 */

    /* ---- CGI: stdin 파이프에 쓰고 stdout 파이프에서 읽는다 ---- */
    pid_t cgi_pid;          /* 아직 cgi_release 하지 않은 자식 (0 이면 없음) */
    int up_in_fd, up_out_fd;
    const char *up_send;    /* CGI 에 보낼 요청 본문 */
    size_t up_send_len, up_send_sent;
    char *up_buf;           /* 업스트림이 보낸 것 전부 */
    size_t up_len, up_cap;
    char up_name[PATH_MAX]; /* 로그용: 스크립트 경로 */
};

static unsigned long total_requests; /* 스레드가 하나라 lock 이 필요 없다 */

unsigned long conn_total_requests(void) { return total_requests; }

long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); /* 벽시계가 아니라 "흐른 시간" (시계 조정에 안 흔들림) */
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void conn_log(const struct conn *c, const char *fmt, ...)
{
    char line[PATH_MAX + 256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("[c%lu] %s\n", c->id, line);
}

static void conn_log_errno(const struct conn *c, const char *what)
{
    conn_log(c, "%s: %s", what, strerror(errno));
}

static int would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

/* ------------------------------------------------------------------ */
/* 상태 전환                                                            */
/* ------------------------------------------------------------------ */

/* 업스트림 쪽 자원을 전부 놓는다. 자식이 아직 돌고 있으면 죽인다(응답을 더는 기다리지 않음). */
static void release_upstream(struct conn *c)
{
    if (c->up_in_fd >= 0)
        close(c->up_in_fd);
    if (c->up_out_fd >= 0)
        close(c->up_out_fd);
    c->up_in_fd = c->up_out_fd = -1;
    if (c->cgi_pid > 0)
        cgi_release(c->cgi_pid, 1);
    c->cgi_pid = 0;
    free(c->up_buf);
    c->up_buf = NULL;
    c->up_send = NULL;
    c->up_send_len = c->up_send_sent = c->up_len = c->up_cap = 0;
}

static void release_response(struct conn *c)
{
    release_upstream(c);
    free(c->out);
    c->out = NULL;
    c->out_len = c->out_sent = 0;
    if (c->file_fd >= 0)
        close(c->file_fd);
    c->file_fd = -1;
    c->file_left = 0;
    free(c->chunk);
    c->chunk = NULL;
    c->chunk_len = c->chunk_sent = 0;
    free(c->body);
    c->body = NULL;
    c->body_len = c->body_have = 0;
}

static void close_now(struct conn *c)
{
    c->state = ST_DONE;
}

/* 안 읽은 데이터가 남은 채 close 하면 커널이 RST 를 보내 클라이언트가 응답을 못 읽을 수 있다.
 * 그래서 FIN 만 먼저 보내고(SHUT_WR), 상대가 닫을 때까지 입력을 조금 비운다.
 * 스레드 풀 버전의 close_gracefully 와 같은 일인데, 기다리는 대신 "상태"가 되었다. */
static void start_linger(struct conn *c, long long now)
{
    shutdown(c->fd, SHUT_WR);
    c->state = ST_LINGER;
    c->drained = 0;
    c->deadline = now + LINGER_TIMEOUT_MS;
}

/* 다음 요청을 받을 준비. 버퍼에 이미 다음 요청 일부가 있으면(파이프라이닝) 헤더 마감을,
 * 없으면 idle 마감을 건다. 파싱은 여기서 하지 않는다 → conn_handle 의 pump 가 한다. */
static void enter_read_head(struct conn *c, long long now)
{
    c->state = ST_READ_HEAD;
    c->scanned = 0;
    c->head_only = 0;
    c->deadline = now + (c->len > 0 ? HEADER_TIMEOUT_MS : IDLE_TIMEOUT_MS);
}

/* 응답을 다 보냈다. keep-alive 면 같은 연결로 다음 요청, 아니면 닫기. */
static void finish_response(struct conn *c, long long now)
{
    conn_log(c, "%s%s", c->summary, c->keep_alive && !c->stopping ? " [keep-alive]" : "");
    release_response(c);

    if (!c->keep_alive || c->stopping) {
        start_linger(c, now);
        return;
    }
    c->served++;
    /* 이번 요청(헤더 + buf 안의 본문 부분)을 버리고, 뒤에 딸려 온 다음 요청을 앞으로 당긴다. */
    c->len -= c->consumed;
    memmove(c->buf, c->buf + c->consumed, c->len);
    enter_read_head(c, now);
}

/* ------------------------------------------------------------------ */
/* 보내기                                                               */
/* ------------------------------------------------------------------ */

/* 보낼 수 있는 만큼만 보내고 돌아온다.
 * send 가 EAGAIN = "소켓 송신 버퍼가 꽉 참" → 상태는 WRITE 그대로, 루프가 POLLOUT 을 기다려 준다. */
static void do_write(struct conn *c, long long now)
{
    size_t budget = WRITE_BUDGET;
    while (budget > 0) {
        const char *p;
        size_t left;
        size_t *sent;

        if (c->out_sent < c->out_len) {            /* 1) 헤더(+작은 본문) */
            p = c->out + c->out_sent;
            left = c->out_len - c->out_sent;
            sent = &c->out_sent;
        } else if (c->chunk_sent < c->chunk_len) { /* 2) 읽어 둔 파일 조각의 나머지 */
            p = c->chunk + c->chunk_sent;
            left = c->chunk_len - c->chunk_sent;
            sent = &c->chunk_sent;
        } else if (c->file_left > 0) {             /* 3) 파일에서 다음 조각 읽기 */
            if (!c->chunk && !(c->chunk = malloc(FILE_CHUNK))) {
                conn_log_errno(c, "malloc");
                close_now(c);
                return;
            }
            size_t want = c->file_left < FILE_CHUNK ? (size_t)c->file_left : FILE_CHUNK;
            /* 일반 파일 read 는 디스크에서 금방 돌아오므로 블로킹이어도 루프를 거의 안 멈춘다.
             * (진짜로 막지 않으려면 sendfile 이나 별도 I/O 스레드가 필요하다) */
            ssize_t r = read(c->file_fd, c->chunk, want);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                conn_log_errno(c, "read");
                close_now(c);
                return;
            }
            if (r == 0) { /* 보내는 도중 파일이 짧아졌다. Content-Length 를 못 채우니 끊는다. */
                conn_log(c, "file shrank while sending");
                close_now(c);
                return;
            }
            c->chunk_len = (size_t)r;
            c->chunk_sent = 0;
            c->file_left -= r;
            continue;
        } else {                                   /* 4) 다 보냈다 */
            finish_response(c, now);
            return;
        }

        if (left > budget)
            left = budget;
        ssize_t n = send(c->fd, p, left, 0);
        if (n < 0) {
            if (would_block())
                return; /* 송신 버퍼 꽉 참 → POLLOUT 기다리기 */
            /* EPIPE/ECONNRESET: 클라이언트가 먼저 끊음. SIGPIPE 를 무시해 뒀으므로 여기로 온다. */
            conn_log_errno(c, "send");
            close_now(c);
            return;
        }
        *sent += (size_t)n;
        budget -= (size_t)n;
        c->deadline = now + SEND_TIMEOUT_MS; /* 조금이라도 보냈으면 마감 연장 */
    }
    /* 예산을 다 씀 → 다른 연결 차례. 아직 WRITE 상태라 다음 바퀴에 POLLOUT 으로 다시 온다. */
}

/* 헤더 + 본문을 한 덩어리로 만들어 보내기 시작한다. */
static void begin_response(struct conn *c, int status, const char *content_type,
                           const char *body, size_t body_len, const char *extra_headers,
                           long long now)
{
    char head[2048];
    int n = http_build_response_head(head, sizeof(head), status, content_type, body_len,
                                     extra_headers, c->keep_alive && !c->stopping);
    size_t send_body = c->head_only ? 0 : body_len; /* HEAD: Content-Length 는 그대로, 본문만 뺀다 */
    if (n < 0 || !(c->out = malloc((size_t)n + send_body + 1))) {
        conn_log(c, "cannot build response");
        close_now(c);
        return;
    }
    memcpy(c->out, head, (size_t)n);
    if (send_body > 0)
        memcpy(c->out + n, body, send_body);
    c->out_len = (size_t)n + send_body;
    c->out_sent = 0;
    snprintf(c->summary, sizeof(c->summary), "<- %d %s (%zu bytes body)", status,
             http_reason(status), body_len);

    c->state = ST_WRITE;
    c->deadline = now + SEND_TIMEOUT_MS;
    do_write(c, now); /* 대부분은 송신 버퍼에 바로 들어가므로 POLLOUT 을 기다리지 않고 시도 */
}

/* 에러 응답: 본문은 "404 Not Found\n" 처럼 상태를 그대로 적는다. */
static void send_error(struct conn *c, int status, const char *extra_headers, long long now)
{
    char body[128];
    int n = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
    begin_response(c, status, "text/plain; charset=utf-8", body, (size_t)n, extra_headers, now);
}

/* 이 에러 뒤에는 연결을 이어 쓸 수 없다(요청 경계를 모르게 됨). */
static void fail(struct conn *c, int status, long long now)
{
    c->keep_alive = 0;
    send_error(c, status, NULL, now);
}

/* ------------------------------------------------------------------ */
/* 정적 파일 (3단계 로직 그대로, 보내는 부분만 "상태"로 바뀜)                  */
/* ------------------------------------------------------------------ */

static int status_from_errno(int err)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:      return 404; /* 없는 파일, 또는 "파일/xxx" 처럼 중간이 파일 */
    case EACCES:
    case EPERM:        return 403; /* 권한 없음 */
    case ENAMETOOLONG: return 414;
    default:           return 500; /* EMFILE(fd 고갈), EIO 등 서버 쪽 문제 */
    }
}

static void serve_file(struct conn *c, int head_only, long long now)
{
    const struct http_request *req = &c->req;

    /* 1) 글자 수준 검사: URL → 파일 경로, ".." 탈출 차단 */
    char path[STATIC_MAX_PATH];
    int trailing_slash;
    switch (static_resolve_path(c->cfg->docroot, req->target, path, sizeof(path), &trailing_slash)) {
    case STATIC_OK:          break;
    case STATIC_BAD_REQUEST: send_error(c, 400, NULL, now); return;
    case STATIC_FORBIDDEN:   send_error(c, 403, NULL, now); return;
    case STATIC_TOO_LONG:    send_error(c, 414, NULL, now); return;
    }

    /* 2) 파일 시스템 수준 검사: 심볼릭 링크까지 다 풀어서 진짜 위치가 루트 안인지 */
    char real[PATH_MAX];
    if (!realpath(path, real)) {
        send_error(c, status_from_errno(errno), NULL, now);
        return;
    }
    if (!static_is_under(c->cfg->docroot, real)) {
        conn_log(c, "blocked: %s -> %s (outside docroot)", path, real);
        send_error(c, 403, NULL, now);
        return;
    }

    /* 3) 열기. O_NONBLOCK: FIFO 라면 open 이 상대를 영원히 기다리는 걸 막는다.
     *    O_CLOEXEC: 이 사이에 CGI 를 fork+exec 해도 자식에게 이 fd 가 새지 않게. */
    int file_fd = open(real, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file_fd < 0) {
        send_error(c, status_from_errno(errno), NULL, now);
        return;
    }
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        conn_log_errno(c, "fstat");
        close(file_fd);
        send_error(c, 500, NULL, now);
        return;
    }

    /* 4) 디렉토리면 index.html 로 */
    if (S_ISDIR(st.st_mode)) {
        close(file_fd);
        if (!trailing_slash) {
            /* "/docs" → "/docs/" 로 보내야 index.html 안의 상대 경로가 제대로 풀린다. */
            char loc[STATIC_MAX_PATH + 32];
            size_t plen = strcspn(req->target, "?#");
            int n = snprintf(loc, sizeof(loc), "Location: %.*s/\r\n", (int)plen, req->target);
            if (n < 0 || (size_t)n >= sizeof(loc)) {
                send_error(c, 414, NULL, now);
                return;
            }
            send_error(c, 301, loc, now);
            return;
        }
        if (strlen(real) + sizeof("/index.html") > sizeof(real)) {
            send_error(c, 414, NULL, now);
            return;
        }
        strcat(real, "/index.html");
        file_fd = open(real, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (file_fd < 0) {
            /* index.html 이 없는 디렉토리: 목록을 보여 주지 않는다(파일 이름 노출 방지). */
            send_error(c, errno == ENOENT ? 403 : status_from_errno(errno), NULL, now);
            return;
        }
        if (fstat(file_fd, &st) < 0) {
            conn_log_errno(c, "fstat");
            close(file_fd);
            send_error(c, 500, NULL, now);
            return;
        }
    }

    /* 일반 파일만 보낸다. 장치 파일, 소켓, FIFO 등은 거절. */
    if (!S_ISREG(st.st_mode)) {
        close(file_fd);
        send_error(c, 403, NULL, now);
        return;
    }

    /* 5) 헤더를 out 에, 파일은 file_fd 로 걸어 두고 WRITE 상태로.
     *    ※ 에러 판단은 반드시 여기 "전에". 헤더가 나간 뒤엔 상태 코드를 바꿀 수 없다. */
    char head[1024];
    int n = http_build_response_head(head, sizeof(head), 200, static_mime_type(real),
                                     (size_t)st.st_size, NULL, c->keep_alive && !c->stopping);
    if (n < 0 || !(c->out = malloc((size_t)n))) {
        close(file_fd);
        close_now(c);
        return;
    }
    memcpy(c->out, head, (size_t)n);
    c->out_len = (size_t)n;
    c->out_sent = 0;
    if (head_only) {
        close(file_fd);
    } else {
        c->file_fd = file_fd;
        c->file_left = st.st_size;
    }
    snprintf(c->summary, sizeof(c->summary), "<- 200 OK %s (%lld bytes%s)", real,
             (long long)st.st_size, head_only ? ", HEAD" : "");

    c->state = ST_WRITE;
    c->deadline = now + SEND_TIMEOUT_MS;
    do_write(c, now);
}

/* ------------------------------------------------------------------ */
/* CGI 입출력 (5단계): 보내기, 받기, 실패, 완료                          */
/* ------------------------------------------------------------------ */

/* 업스트림 쪽 문제는 클라이언트 잘못이 아니다. 요청은 끝까지 받았으니 연결은 계속 쓸 수 있다.
 *   502 Bad Gateway    : 업스트림이 "틀린" 답을 했다 (실행 실패, 망가진 응답)
 *   504 Gateway Timeout: 업스트림이 "제때" 답을 안 했다 */
static void upstream_fail(struct conn *c, int status, const char *why, long long now)
{
    conn_log(c, "cgi %s: %s -> %d", c->up_name, why, status);
    release_upstream(c); /* CGI 가 아직 돌고 있으면 여기서 SIGKILL */
    send_error(c, status, NULL, now);
}

/* 업스트림이 출력을 다 보냈다(EOF). 해석해서 클라이언트에게 보낼 응답으로 바꾼다.
 * 업스트림 응답을 통째로 모은 뒤 바꾸므로, 우리가 Content-Length 를 정확히 알고
 * 클라이언트와의 keep-alive 를 그대로 유지할 수 있다. (대신 응답 크기에 상한이 있다) */
static void upstream_complete(struct conn *c, long long now)
{
    if (c->cgi_pid > 0) {
        /* stdout 이 닫혔다 = 자식이 끝났거나 곧 끝난다. 이미 끝났으면 바로 거두고,
         * 아니면 고아 목록에 넣어 SIGCHLD 때 거둔다. (죽이지는 않는다) */
        cgi_release(c->cgi_pid, 0);
        c->cgi_pid = 0;
    }
    struct upstream_response r;
    if (upstream_parse_response(c->up_buf ? c->up_buf : "", c->up_len, &r) < 0) {
        upstream_fail(c, 502, c->up_len == 0 ? "empty response" : "malformed response", now);
        return;
    }
    size_t out_len;
    char *out = upstream_build_client_response(&r, c->head_only, c->keep_alive && !c->stopping,
                                               &out_len);
    if (!out) {
        upstream_fail(c, 500, "cannot build response", now);
        return;
    }
    /* r 은 up_buf 안을 가리키므로 release_upstream 전에 로그를 만들어 둔다. */
    snprintf(c->summary, sizeof(c->summary), "<- %d %s from cgi %s (%zu bytes body)", r.status,
             r.reason, c->up_name, r.body_len);
    release_upstream(c);

    c->out = out;
    c->out_len = out_len;
    c->out_sent = 0;
    c->state = ST_WRITE;
    c->deadline = now + SEND_TIMEOUT_MS;
    do_write(c, now);
}

/* 업스트림 출력을 "한 번" 읽는다. 더 있으면 poll 이 또 알려 준다. */
static void upstream_read(struct conn *c, long long now)
{
    if (c->up_len == c->up_cap) {
        if (c->up_cap >= UPSTREAM_MAX_RESPONSE) {
            upstream_fail(c, 502, "response too large", now);
            return;
        }
        size_t cap = c->up_cap ? c->up_cap * 2 : 16 * 1024;
        char *nb = realloc(c->up_buf, cap);
        if (!nb) {
            upstream_fail(c, 500, "out of memory", now);
            return;
        }
        c->up_buf = nb;
        c->up_cap = cap;
    }
    ssize_t n = read(c->up_out_fd, c->up_buf + c->up_len, c->up_cap - c->up_len);
    if (n < 0) {
        if (would_block())
            return;
        upstream_fail(c, 502, strerror(errno), now);
        return;
    }
    if (n == 0) { /* EOF: CGI 가 stdout 을 닫음 = 응답 끝 */
        upstream_complete(c, now);
        return;
    }
    c->up_len += (size_t)n;
}

/* up_send 를 보낼 수 있는 만큼 보낸다. CGI 는 다 보내면 stdin 을 닫아 "본문 끝(EOF)"을 알린다. */
static void upstream_write(struct conn *c, long long now)
{
    while (c->up_send_sent < c->up_send_len) {
        ssize_t n = write(c->up_in_fd, c->up_send + c->up_send_sent, c->up_send_len - c->up_send_sent);
        if (n < 0) {
            if (would_block())
                return; /* 파이프 버퍼가 꽉 참 → POLLOUT 을 기다린다 */
            if (errno == EPIPE) {
                /* 스크립트가 본문을 다 안 읽고 stdin 을 닫았다. 흔한 일이라 에러가 아니다.
                 * (SIGPIPE 를 무시해 뒀기 때문에 서버가 죽지 않고 여기로 온다) */
                c->up_send_sent = c->up_send_len;
                break;
            }
            upstream_fail(c, 502, strerror(errno), now);
            return;
        }
        c->up_send_sent += (size_t)n;
    }
    close(c->up_in_fd);
    c->up_in_fd = -1;
}

/* ------------------------------------------------------------------ */
/* 5단계: CGI                                                           */
/* ------------------------------------------------------------------ */

#define CGI_PREFIX "/cgi-bin/"

/*
 *   /cgi-bin/add/extra/path?a=1&b=2
 *   └──────┬───┘└────┬────┘ └──┬──┘
 *     SCRIPT_NAME  PATH_INFO  QUERY_STRING
 *
 * 1) 스크립트 이름 검사 → 2) 파일이 실행 가능한지 → 3) 환경변수 만들기 → 4) fork+exec
 * → 5) UPSTREAM 상태로: 본문은 stdin 파이프로, 응답은 stdout 파이프에서
 */
static void start_cgi(struct conn *c, long long now)
{
    const char *m = c->req.method;
    if (strcmp(m, "GET") != 0 && strcmp(m, "HEAD") != 0 && strcmp(m, "POST") != 0) {
        send_error(c, 405, "Allow: GET, HEAD, POST\r\n", now);
        return;
    }

    /* 1) 이름에는 영숫자와 . _ - 만, 첫 글자는 '.' 금지 → "../" 나 숨김 파일은 아예 못 만든다 */
    const char *name = c->req.target + strlen(CGI_PREFIX);
    size_t nlen = strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-");
    char tail = name[nlen];
    if (nlen == 0 || name[0] == '.' || (tail != '\0' && tail != '/' && tail != '?' && tail != '#')) {
        send_error(c, 404, NULL, now);
        return;
    }
    const char *rest = name + nlen;
    size_t pi_len = strcspn(rest, "?#");
    const char *q = rest[pi_len] == '?' ? rest + pi_len + 1 : "";
    size_t q_len = strcspn(q, "#");

    char script_name[STATIC_MAX_PATH], path_info[STATIC_MAX_PATH], query[STATIC_MAX_PATH];
    char path[PATH_MAX];
    if (snprintf(script_name, sizeof(script_name), CGI_PREFIX "%.*s", (int)nlen, name) >= (int)sizeof(script_name) ||
        snprintf(path_info, sizeof(path_info), "%.*s", (int)pi_len, rest) >= (int)sizeof(path_info) ||
        snprintf(query, sizeof(query), "%.*s", (int)q_len, q) >= (int)sizeof(query) ||
        snprintf(path, sizeof(path), "%s/%.*s", c->cfg->cgi_dir, (int)nlen, name) >= (int)sizeof(path)) {
        send_error(c, 414, NULL, now);
        return;
    }

    /* 2) 일반 파일이고 실행 권한이 있어야 한다 */
    struct stat st;
    if (stat(path, &st) < 0) {
        send_error(c, status_from_errno(errno), NULL, now);
        return;
    }
    if (!S_ISREG(st.st_mode) || access(path, X_OK) < 0) {
        conn_log(c, "cgi %s: not an executable file", path);
        send_error(c, 403, NULL, now);
        return;
    }

    /* 3) 환경변수: 요청 정보는 전부 여기로 간다(본문만 stdin) */
    const char *path_env = getenv("PATH");
    struct cgi_env_info info = {
        .script_name = script_name,
        .path_info = path_info,
        .query = query,
        .remote_addr = c->peer_ip,
        .remote_port = c->peer_port,
        .server_port = c->cfg->port,
        .path_env = path_env ? path_env : "/usr/bin:/bin",
    };
    char **env = cgi_build_env(&c->req, c->body_len, &info);
    if (!env) {
        send_error(c, 500, NULL, now);
        return;
    }

    /* 4) fork + exec */
    pid_t pid;
    int in_fd, out_fd;
    int rc = cgi_spawn(path, c->cfg->cgi_dir, env, &pid, &in_fd, &out_fd);
    cgi_free_env(env); /* 자식은 fork 순간 복사본을 가졌으므로 부모는 바로 풀어도 된다 */
    if (rc < 0) {
        conn_log_errno(c, "cgi spawn"); /* EAGAIN: 프로세스 수 한도, EMFILE: fd 고갈 */
        send_error(c, 503, "Retry-After: 1\r\n", now);
        return;
    }

    /* 5) 이제부터 이 연결은 자식의 두 파이프를 poll 한다 */
    c->cgi_pid = pid;
    c->up_in_fd = in_fd;
    c->up_out_fd = out_fd;
    c->up_send = c->body;
    c->up_send_len = c->body_len;
    c->up_send_sent = 0;
    snprintf(c->up_name, sizeof(c->up_name), "%s", script_name);
    c->state = ST_UPSTREAM;
    c->deadline = now + c->cfg->cgi_timeout_ms; /* 실행 전체에 대한 마감 */
    conn_log(c, "cgi %s started (pid %d, %zu bytes to stdin)", script_name, (int)pid, c->body_len);
    upstream_write(c, now); /* 본문이 없으면 바로 stdin 을 닫는다 */
}

/* ------------------------------------------------------------------ */
/* 어디로 보낼까                                                         */
/* ------------------------------------------------------------------ */

/* 우선순위: /cgi-bin/ → /builtin/ → 정적 파일 */
static void dispatch(struct conn *c, long long now)
{
    const char *t = c->req.target, *m = c->req.method;

    if (c->cfg->cgi_dir[0] && strncmp(t, CGI_PREFIX, strlen(CGI_PREFIX)) == 0) {
        start_cgi(c, now);
        return;
    }
    struct builtin_response br;
    if (builtin_handle(&c->req, c->body, c->body_len, &br)) {
        begin_response(c, br.status, br.content_type, br.body, br.body_len, br.extra_headers, now);
        return;
    }
    if (strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0) {
        serve_file(c, c->head_only, now); /* HEAD: GET 과 같은 헤더, 본문만 없음 */
    } else {
        /* 405 응답에는 허용 메서드를 알려 주는 Allow 헤더가 필수다. */
        send_error(c, 405, "Allow: GET, HEAD\r\n", now);
    }
}

/* ------------------------------------------------------------------ */
/* 받기                                                                 */
/* ------------------------------------------------------------------ */

/* 헤더가 완성됐다: 파싱하고, 본문이 더 필요하면 READ_BODY, 아니면 바로 응답. */
static void process_head(struct conn *c, size_t head_len, long long now)
{
    enum http_parse_result pr = http_parse_request(c->buf, head_len, &c->req);
    if (pr != HTTP_PARSE_OK) {
        fail(c, pr == HTTP_PARSE_TOO_MANY_HEADERS ? 431
              : pr == HTTP_PARSE_BAD_VERSION      ? 505
                                                  : 400, now);
        return;
    }
    total_requests++;
    c->head_only = strcmp(c->req.method, "HEAD") == 0;

    /* 응답 헤더에 Connection 을 써야 하므로 보내기 "전에" 정한다.
     * 스레드 풀 버전에 있던 "줄 선 손님이 있으면 양보" 조건은 없다.
     * 노는 연결이 워커를 붙잡지 않으니 양보할 이유가 없다. */
    c->keep_alive = http_wants_keep_alive(&c->req)
                 && c->served + 1 < MAX_REQUESTS_PER_CONN
                 && !c->stopping;

    conn_log(c, "-> %s %s %s (%zu headers)", c->req.method, c->req.target, c->req.version,
             c->req.header_count);

    size_t clen;
    if (http_content_length(&c->req, &clen) < 0) {
        fail(c, 400, now);
        return;
    }
    if (clen > HTTP_MAX_BODY) {
        fail(c, 413, now); /* 본문을 안 읽고 버리므로 다음 요청의 시작 위치를 알 수 없다 */
        return;
    }

    size_t extra = c->len - head_len; /* 헤더 뒤에 이미 딸려 온 바이트 */
    size_t in_buf = extra < clen ? extra : clen;
    c->consumed = head_len + in_buf;

    if (clen > 0) {
        if (!(c->body = malloc(clen))) {
            conn_log_errno(c, "malloc");
            fail(c, 500, now);
            return;
        }
        c->body_len = clen;
        c->body_have = in_buf;
        memcpy(c->body, c->buf + head_len, in_buf);
        if (c->body_have < clen) {
            c->state = ST_READ_BODY;
            c->deadline = now + BODY_TIMEOUT_MS;
            return; /* 나머지는 POLLIN 이 올 때마다 do_read 가 채운다 */
        }
    }
    dispatch(c, now);
}

/* buf 에서 "\r\n\r\n" 을 찾아 본다. 있으면 process_head, 없으면 더 받을 때까지 대기. */
static void try_head(struct conn *c, long long now)
{
    /* 새로 받은 조각과 이전 끝 3바이트에 걸쳐 "\r\n\r\n" 이 생길 수 있다. */
    size_t from = c->scanned >= 3 ? c->scanned - 3 : 0;
    long end = http_find_head_end(c->buf + from, c->len - from);
    if (end > 0) {
        process_head(c, from + (size_t)end, now);
        return;
    }
    c->scanned = c->len;
    if (c->len == sizeof(c->buf))
        fail(c, 431, now); /* 버퍼가 꽉 찼는데도 헤더 끝이 없음 */
}

/* 읽을 게 있다는 신호가 왔다. recv 를 "한 번만" 하고 돌아간다.
 * (더 남아 있으면 poll 이 다음 바퀴에 또 알려 준다: level-triggered) */
static void do_read(struct conn *c, long long now)
{
    char sink[4096];
    char *dst;
    size_t room;

    switch (c->state) {
    case ST_READ_HEAD: dst = c->buf + c->len;           room = sizeof(c->buf) - c->len;  break;
    case ST_READ_BODY: dst = c->body + c->body_have;    room = c->body_len - c->body_have; break;
    case ST_LINGER:    dst = sink;                      room = sizeof(sink);             break;
    default:           return;
    }

    ssize_t n = recv(c->fd, dst, room, 0);
    if (n < 0) {
        if (would_block())
            return;
        if (errno != ECONNRESET && c->state != ST_LINGER)
            conn_log_errno(c, "recv");
        close_now(c);
        return;
    }
    if (n == 0) { /* 상대가 닫음 */
        if (c->state == ST_READ_BODY)
            conn_log(c, "client closed before sending full body (%zu/%zu)", c->body_have,
                     c->body_len);
        close_now(c);
        return;
    }

    switch (c->state) {
    case ST_READ_HEAD:
        if (c->len == 0)
            c->deadline = now + HEADER_TIMEOUT_MS; /* 첫 바이트: 여기서부터 헤더 마감 시작 */
        c->len += (size_t)n; /* 파싱은 pump 가 */
        break;
    case ST_READ_BODY:
        c->body_have += (size_t)n;
        if (c->body_have == c->body_len)
            dispatch(c, now);
        break;
    case ST_LINGER:
        c->drained += (size_t)n;
        if (c->drained >= LINGER_MAX_BYTES)
            close_now(c);
        break;
    default:
        break;
    }
}

/* 소켓을 기다리지 않고도 진행할 수 있는 동안 계속 진행한다.
 * 예: 파이프라이닝으로 요청 3개가 한 번에 왔다면, 첫 응답을 다 보낸 직후 buf 에 이미
 * 두 번째 요청이 있다. 새 POLLIN 은 안 오므로 여기서 바로 파싱해야 한다.
 * (재귀 대신 반복문: finish_response 가 직접 try_head 를 부르면 요청 수만큼 스택이 깊어진다) */
static void pump(struct conn *c, long long now)
{
    while (c->state == ST_READ_HEAD && c->len > c->scanned)
        try_head(c, now);
}

/* ------------------------------------------------------------------ */
/* 루프가 부르는 인터페이스                                                */
/* ------------------------------------------------------------------ */

struct conn *conn_new(int fd, unsigned long id, struct server_config *cfg, const char *peer_ip,
                      int peer_port, long long now)
{
    struct conn *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = fd;
    c->id = id;
    c->cfg = cfg;
    snprintf(c->peer_ip, sizeof(c->peer_ip), "%s", peer_ip);
    c->peer_port = peer_port;
    c->file_fd = -1;
    c->up_in_fd = c->up_out_fd = -1;
    enter_read_head(c, now);
    return c;
}

static short conn_events(const struct conn *c)
{
    switch (c->state) {
    case ST_READ_HEAD:
    case ST_READ_BODY:
    case ST_LINGER: return POLLIN;
    case ST_WRITE:  return POLLOUT;
    default:        return 0; /* UPSTREAM: 클라이언트 쪽은 기다릴 게 없다 */
    }
}

int conn_pollfds(const struct conn *c, struct pollfd *out)
{
    int n = 0;
    out[n++] = (struct pollfd){ .fd = c->fd, .events = conn_events(c) };
    if (c->state == ST_UPSTREAM) {
        /* CGI 는 stdin 쓰기와 stdout 읽기를 "동시에" 기다린다. 본문을 다 쓴 뒤에야 읽기 시작하면,
         * 스크립트가 본문을 읽으면서 출력도 많이 내는 경우 양쪽 파이프 버퍼가 동시에 꽉 차서
         * 서로를 영원히 기다린다(교착). */
        if (c->up_in_fd >= 0)
            out[n++] = (struct pollfd){ .fd = c->up_in_fd, .events = POLLOUT };
        if (c->up_out_fd >= 0)
            out[n++] = (struct pollfd){ .fd = c->up_out_fd, .events = POLLIN };
    }
    return n;
}

long long conn_deadline(const struct conn *c)
{
    return c->state == ST_DONE ? LLONG_MAX : c->deadline;
}

int conn_done(const struct conn *c)
{
    return c->state == ST_DONE;
}

void conn_handle(struct conn *c, const struct pollfd *pfds, int n, long long now)
{
    short revents = pfds[0].revents;
    if (c->state == ST_UPSTREAM) {
        if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* events=0 이어도 이 셋은 보고된다. 무시하면 poll 이 매번 즉시 돌아와 헛돈다. */
            conn_log(c, "client went away while waiting for CGI");
            release_upstream(c);
            close_now(c);
            return;
        }
        for (int i = 1; i < n && c->state == ST_UPSTREAM; i++) {
            if (!pfds[i].revents)
                continue;
            if (pfds[i].fd == c->up_in_fd)
                upstream_write(c, now);
            else if (pfds[i].fd == c->up_out_fd)
                upstream_read(c, now);
        }
    } else if (c->state == ST_WRITE) {
        /* POLLHUP/POLLERR 도 같이 처리한다: 이어지는 recv/send 가 0 이나 -1 로 이유를 알려 준다. */
        if (revents & (POLLOUT | POLLHUP | POLLERR))
            do_write(c, now);
    } else if (revents & (POLLIN | POLLHUP | POLLERR)) {
        do_read(c, now);
    }
    pump(c, now);
}

void conn_timeout(struct conn *c, long long now)
{
    switch (c->state) {
    case ST_READ_HEAD:
        if (c->len == 0) {
            conn_log(c, c->served > 0 ? "idle %d ms, closing" : "no request within %d ms, closing",
                     IDLE_TIMEOUT_MS);
            close_now(c);
        } else {
            conn_log(c, "header timed out after %d ms (%zu bytes)", HEADER_TIMEOUT_MS, c->len);
            fail(c, 408, now);
        }
        break;
    case ST_READ_BODY:
        conn_log(c, "body timed out (%zu/%zu)", c->body_have, c->body_len);
        release_response(c);
        fail(c, 408, now);
        break;
    case ST_UPSTREAM:
        upstream_fail(c, 504, "timed out, killing", now);
        break;
    case ST_WRITE:
        conn_log(c, "send timed out (client not reading)");
        close_now(c);
        break;
    case ST_LINGER:
        close_now(c);
        break;
    default:
        break;
    }
    pump(c, now);
}

void conn_shutdown(struct conn *c, long long now)
{
    (void)now;
    c->stopping = 1;
    if (c->state == ST_READ_HEAD && c->len == 0) {
        conn_log(c, "shutting down, closing idle connection");
        close_now(c);
    }
    /* 그 외(요청을 받는 중, 응답을 보내는 중)는 하던 일을 마친 뒤 finish_response 가
     * stopping 을 보고 keep-alive 대신 닫는다. */
}

void conn_free(struct conn *c)
{
    release_response(c);
    close(c->fd);
    free(c);
}
