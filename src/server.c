#include "server.h"
#include "http.h"
#include "static_files.h"
#include "thread_pool.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* PATH_MAX, INT_MAX */
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BACKLOG 128

int server_listen(int port)
{
    /* 1) socket: IPv4(AF_INET) + TCP(SOCK_STREAM) 소켓을 만든다. 아직 주소는 없다. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* SO_REUSEADDR: 서버를 껐다 바로 켰을 때 이전 연결이 TIME_WAIT 상태로
     * 포트를 잡고 있어도 bind 가 "Address already in use" 로 실패하지 않게 한다. */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    /* 2) bind: 소켓에 "0.0.0.0:port" 주소를 붙인다. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; //주소 형식 
    // 어느 IP 주소로 들어오는 연결을 받을지
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 모든 네트워크 인터페이스 */
    // 포트번호
    addr.sin_port = htons((unsigned short)port); /* 네트워크 바이트 순서(빅엔디언)로 */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); /* 흔한 원인: 포트 사용 중(EADDRINUSE), 1024 미만 포트 권한(EACCES) */
        close(fd);
        return -1;
    }

    /* 3) listen: 이제 이 소켓은 "연결 받는 용도"가 된다.
     * 커널이 3-way handshake 를 대신 처리하고, 완료된 연결을 BACKLOG 크기 큐에 쌓아둔다. */
    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}



/* ------------------------------------------------------------------ */
/* 연결 상태 + 공유 통계                                                  */
/* ------------------------------------------------------------------ */

/* 시간 제한들. 모두 "연결 하나가 워커 하나를 얼마나 오래 붙잡을 수 있나"의 상한이다. */
#define IDLE_TIMEOUT_MS   5000  /* keep-alive: 다음 요청의 첫 바이트를 기다리는 최대 시간 */
#define HEADER_TIMEOUT_MS 10000 /* 첫 바이트부터 "\r\n\r\n" 까지 (slowloris 방어) */
#define BODY_TIMEOUT_MS   30000 /* 본문 전체를 받는 최대 시간 */
#define SEND_TIMEOUT_SEC  10    /* send 한 번이 막혀 있을 수 있는 최대 시간 (안 읽는 클라이언트) */
#define DRAIN_TIMEOUT_MS  1000  /* close_gracefully 에서 남은 입력을 비우는 최대 시간 */
#define MAX_REQUESTS_PER_CONN 100

/* 연결 하나의 상태. 워커 스레드의 스택 위에 있으므로 다른 스레드와 공유되지 않는다. */
struct conn {
    int fd;
    int worker;       /* 처리 중인 워커 번호 (로그용) */
    unsigned long id; /* 서버 시작 후 몇 번째 연결인지 (로그용) */
    const struct server_ctx *ctx;

    /* 받은 바이트를 모아 두는 곳. keep-alive 에서는 한 번 recv 에 "이번 요청 끝 + 다음
     * 요청 앞부분"이 같이 올 수 있어서(파이프라이닝), 남은 부분을 다음 요청으로 넘겨야 한다. */
    char buf[HTTP_MAX_HEAD];
    size_t len;

    int keep_alive; /* 이번 응답 뒤에 연결을 유지할지. 응답 헤더의 Connection 값이 된다. */
};

/* 모든 워커가 같이 쓰는 통계 → 공유 자원이므로 mutex 로 보호한다.
 * (보호 없이 stats.requests++ 를 하면 "읽기-더하기-쓰기" 사이에 다른 스레드가 끼어들어
 *  증가분이 사라질 수 있다. 4단계 설명의 "lost update" 참고) */
static pthread_mutex_t stats_mu = PTHREAD_MUTEX_INITIALIZER;
static struct server_stats stats;

void server_stats_get(struct server_stats *out)
{
    pthread_mutex_lock(&stats_mu);
    *out = stats;
    pthread_mutex_unlock(&stats_mu);
}

/* 로그 한 줄을 printf "한 번"으로 출력한다. 여러 번 나눠 찍으면 다른 스레드의 출력이
 * 중간에 끼어들어 줄이 섞일 수 있다. (printf 한 번은 stdout 을 잠그고 쓰므로 안전) */
static void conn_log(const struct conn *c, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("[w%d c%lu] %s\n", c->worker, c->id, line);
}

/* perror 대신. strerror 는 스레드 안전이 보장되지 않아 strerror_r 를 쓴다. */
static void conn_log_errno(const struct conn *c, const char *what)
{
    int err = errno;
    char msg[128];
    if (strerror_r(err, msg, sizeof(msg)) != 0)
        snprintf(msg, sizeof(msg), "errno %d", err);
    conn_log(c, "%s: %s", what, msg);
    errno = err;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); /* 벽시계가 아니라 "흐른 시간" (시계 조정에 안 흔들림) */
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

enum { WAIT_READY, WAIT_TIMEOUT, WAIT_STOP, WAIT_ERROR };

/* fd 에 읽을 게 생기거나, deadline(now_ms 기준)이 지나거나, stop_fd 가 울릴 때까지 기다린다.
 * stop_fd 가 -1 이면 poll 이 그 항목을 무시한다 → 종료 신호에 반응하지 않는다. */
static int wait_readable(int fd, int stop_fd, long long deadline)
{
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0)
            return WAIT_TIMEOUT;
        struct pollfd p[2] = {
            { .fd = fd, .events = POLLIN },
            { .fd = stop_fd, .events = POLLIN },
        };
        int r = poll(p, 2, left > INT_MAX ? INT_MAX : (int)left);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return WAIT_ERROR;
        }
        if (r == 0)
            return WAIT_TIMEOUT;
        if (p[0].revents) /* POLLIN 뿐 아니라 POLLHUP/POLLERR 도: 이어지는 recv 가 0/-1 로 알려 준다 */
            return WAIT_READY;
        if (p[1].revents)
            return WAIT_STOP;
    }
}

/* 종료 신호가 이미 왔는지 (기다리지 않고 확인만). */
static int stop_requested(const struct server_ctx *ctx)
{
    struct pollfd p = { .fd = ctx->stop_fd, .events = POLLIN };
    return poll(&p, 1, 0) > 0;
}

/* ------------------------------------------------------------------ */
/* 보내기 헬퍼                                                          */
/* ------------------------------------------------------------------ */

/* send 는 요청한 만큼 다 보내지 못할 수 있으므로 남은 만큼 반복해서 보낸다.
 * 실패하면 이 연결은 더 쓸 수 없으므로 keep_alive 를 끈다. */
static int send_all(struct conn *c, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = send(c->fd, buf + sent, len - sent, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            /* EPIPE: 클라이언트가 이미 끊음. SIGPIPE 를 무시해 뒀기 때문에 프로세스가 죽지
             *        않고 여기로 온다.
             * EAGAIN: SO_SNDTIMEO(10초) 동안 한 바이트도 못 보냄 = 클라이언트가 안 읽는다. */
            conn_log_errno(c, errno == EAGAIN || errno == EWOULDBLOCK ? "send timed out" : "send");
            c->keep_alive = 0;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

/* 응답 = [상태 라인 + 헤더] 를 만들어 보내고, 이어서 본문을 보낸다. */
static void send_response(struct conn *c, int status, const char *content_type,
                          const char *body, size_t body_len,
                          const char *extra_headers)
{
    char head[1024];
    int n = http_build_response_head(head, sizeof(head), status, content_type,
                                     body_len, extra_headers, c->keep_alive);
    if (n < 0) {
        conn_log(c, "response head too large");
        c->keep_alive = 0;
        return;
    }
    if (send_all(c, head, (size_t)n) < 0)
        return;
    if (body_len > 0 && send_all(c, body, body_len) < 0)
        return;
    conn_log(c, "<- %d %s (%zu bytes body)%s", status, http_reason(status), body_len,
             c->keep_alive ? " [keep-alive]" : "");
}

/* 에러 응답: 본문은 "404 Not Found\n" 처럼 상태를 그대로 적는다. */
static void send_error(struct conn *c, int status, const char *extra_headers)
{
    char body[128];
    int n = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
    send_response(c, status, "text/plain; charset=utf-8", body, (size_t)n, extra_headers);
}

/* ------------------------------------------------------------------ */
/* 받기 헬퍼                                                            */
/* ------------------------------------------------------------------ */

enum {
    READ_CLOSED = 0,     /* 클라이언트가 끊음 (keep-alive 끝에 흔히 있는 정상 종료) */
    READ_ERROR = -1,
    READ_TOO_LARGE = -2, /* → 431 */
    READ_TIMEOUT = -3,   /* 요청을 보내다 말고 멈춤 → 408 */
    READ_IDLE = -4,      /* 다음 요청이 안 옴 → 조용히 닫기 */
    READ_STOP = -5,      /* 서버 종료 중, 기다리던 연결 → 조용히 닫기 */
};

/* c->buf 에 "\r\n\r\n" 이 보일 때까지 받는다. 헤더 길이(>0) 또는 READ_* 를 반환.
 *
 * 두 단계의 기다림이 있다.
 *   (1) 아직 한 바이트도 없음: "다음 요청을 기다리는 중" (keep-alive idle)
 *       → IDLE_TIMEOUT, 그리고 종료 신호가 오면 바로 포기한다.
 *   (2) 일부를 받음: "요청을 받는 중"
 *       → 첫 바이트부터 HEADER_TIMEOUT 안에 끝나야 한다.
 *         recv 마다 타이머를 새로 거는(SO_RCVTIMEO) 방식은 1바이트씩 4초 간격으로
 *         보내는 slowloris 공격을 못 막기 때문에, "마감 시각" 하나를 정해 둔다.
 *
 * 이전 요청의 남은 바이트(c->len > 0)가 있으면 recv 전에 그것부터 검사한다. */
static long read_head(struct conn *c)
{
    long long idle_deadline = now_ms() + IDLE_TIMEOUT_MS;
    long long head_deadline = 0;
    size_t scanned = 0; /* 앞에서부터 여기까지는 "\r\n\r\n" 이 없다고 확인함 */

    for (;;) {
        if (c->len > 0) {
            if (head_deadline == 0)
                head_deadline = now_ms() + HEADER_TIMEOUT_MS;
            /* 새로 받은 조각과 이전 끝 3바이트에 걸쳐 "\r\n\r\n" 이 생길 수 있으므로
             * 이전 끝에서 3바이트 앞부터 다시 찾는다. */
            size_t from = scanned >= 3 ? scanned - 3 : 0;
            long end = http_find_head_end(c->buf + from, c->len - from);
            if (end > 0)
                return (long)from + end;
            scanned = c->len;
            if (c->len == sizeof(c->buf))
                return READ_TOO_LARGE; /* 버퍼가 꽉 찼는데도 헤더 끝이 없음 */
        }

        int w = c->len == 0 ? wait_readable(c->fd, c->ctx->stop_fd, idle_deadline)
                            : wait_readable(c->fd, -1, head_deadline);
        if (w == WAIT_TIMEOUT)
            return c->len == 0 ? READ_IDLE : READ_TIMEOUT;
        if (w == WAIT_STOP)
            return READ_STOP;
        if (w == WAIT_ERROR) {
            conn_log_errno(c, "poll");
            return READ_ERROR;
        }

        ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != ECONNRESET)
                conn_log_errno(c, "recv");
            return READ_ERROR;
        }
        if (n == 0)
            return READ_CLOSED;
        c->len += (size_t)n;
    }
}

/* 본문을 정확히 content_length 바이트 받는다.
 * already 는 헤더와 같이 이미 받아 둔 본문 앞부분이다. 실패하면 NULL,
 * 시간 초과였다면 *timed_out = 1. */
static char *read_body(struct conn *c, const char *already, size_t already_len,
                       size_t content_length, int *timed_out)
{
    *timed_out = 0;
    char *body = malloc(content_length);
    if (!body) {
        conn_log_errno(c, "malloc");
        return NULL;
    }

    size_t have = already_len < content_length ? already_len : content_length;
    memcpy(body, already, have);

    long long deadline = now_ms() + BODY_TIMEOUT_MS;
    while (have < content_length) {
        int w = wait_readable(c->fd, -1, deadline);
        if (w == WAIT_TIMEOUT) {
            conn_log(c, "body timed out (%zu/%zu)", have, content_length);
            *timed_out = 1;
            free(body);
            return NULL;
        }
        if (w != WAIT_READY) {
            free(body);
            return NULL;
        }
        ssize_t n = recv(c->fd, body + have, content_length - have, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            conn_log_errno(c, "recv");
            free(body);
            return NULL;
        }
        if (n == 0) { /* Content-Length 만큼 보내기 전에 끊김 */
            conn_log(c, "client closed before sending full body (%zu/%zu)", have, content_length);
            free(body);
            return NULL;
        }
        have += (size_t)n;
    }
    return body;
}

/* 안 읽은 데이터가 수신 버퍼에 남은 채 close 하면 커널이 RST 를 보내고,
 * 그러면 클라이언트가 우리가 보낸 응답을 읽기도 전에 연결이 깨질 수 있다.
 * 그래서 "더 안 보낸다(FIN)"를 먼저 알리고, 남은 입력을 조금 비운 뒤 닫는다.
 * 비우는 데도 시간 상한(DRAIN_TIMEOUT)을 둬서 워커가 여기 묶이지 않게 한다. */
static void close_gracefully(int fd)
{
    shutdown(fd, SHUT_WR);
    char sink[4096];
    size_t drained = 0;
    long long deadline = now_ms() + DRAIN_TIMEOUT_MS;
    while (drained < 64 * 1024 && wait_readable(fd, -1, deadline) == WAIT_READY) {
        ssize_t n = recv(fd, sink, sizeof(sink), 0);
        if (n <= 0)
            break; /* 상대도 닫았거나(0), 오류(-1) */
        drained += (size_t)n;
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 정적 파일 서빙                                                        */
/* ------------------------------------------------------------------ */

#define FILE_CHUNK (64 * 1024) /* 큰 파일도 이만큼씩 읽어 보낸다 → 메모리 사용량이 파일 크기와 무관 */

/* open/realpath 의 errno 를 상태 코드로 바꾼다. */
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

/* fd 의 내용을 처음부터 끝까지 FILE_CHUNK 씩 읽어서 보낸다.
 * 헤더는 이미 나갔으므로 여기서 실패하면 상태 코드를 바꿀 방법이 없다 → 연결을 끊을 뿐. */
static int send_file_body(struct conn *c, int file_fd, off_t size)
{
    /* 3단계에선 static 버퍼였다. static 은 "프로그램 전체에 하나"라서 워커 두 개가 동시에
     * 쓰면 서로의 파일 내용을 덮어쓴다(A 에게 B 의 파일 조각이 감). 그래서 지역 변수로 바꿔
     * 스레드마다 자기 스택에 하나씩 갖게 한다. (스레드 스택: macOS 기본 512KB → 64KB 충분) */
    char chunk[FILE_CHUNK];
    off_t left = size;
    while (left > 0) {
        size_t want = left < (off_t)sizeof(chunk) ? (size_t)left : sizeof(chunk);
        ssize_t r = read(file_fd, chunk, want);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            conn_log_errno(c, "read");
            return -1;
        }
        if (r == 0) { /* 보내는 도중 파일이 짧아졌다. Content-Length 를 못 채우니 끊는다. */
            conn_log(c, "file shrank while sending");
            return -1;
        }
        if (send_all(c, chunk, (size_t)r) < 0) /* send 가 일부만 보내도 send_all 이 마저 보낸다 */
            return -1;
        left -= r;
    }
    return 0;
}

/* GET/HEAD 요청을 docroot 아래의 파일로 응답한다. */
static void serve_file(struct conn *c, const struct http_request *req, int head_only)
{
    const char *docroot = c->ctx->docroot;

    /* 1) 글자 수준 검사: URL → 파일 경로, ".." 탈출 차단 */
    char path[STATIC_MAX_PATH];
    int trailing_slash;
    switch (static_resolve_path(docroot, req->target, path, sizeof(path), &trailing_slash)) {
    case STATIC_OK:          break;
    case STATIC_BAD_REQUEST: send_error(c, 400, NULL); return;
    case STATIC_FORBIDDEN:   send_error(c, 403, NULL); return;
    case STATIC_TOO_LONG:    send_error(c, 414, NULL); return;
    }

    /* 2) 파일 시스템 수준 검사: 심볼릭 링크까지 다 풀어서 진짜 위치가 루트 안인지 */
    char real[PATH_MAX];
    if (!realpath(path, real)) {
        send_error(c, status_from_errno(errno), NULL);
        return;
    }
    if (!static_is_under(docroot, real)) {
        conn_log(c, "blocked: %s -> %s (outside docroot)", path, real);
        send_error(c, 403, NULL);
        return;
    }

    /* 3) 열기. O_NONBLOCK: 혹시 FIFO(named pipe)라면 보통 open 은 상대가 나타날 때까지
     *    영원히 멈춘다. 일반 파일 read 에는 영향이 없다. */
    int file_fd = open(real, O_RDONLY | O_NONBLOCK);
    if (file_fd < 0) {
        send_error(c, status_from_errno(errno), NULL);
        return;
    }
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        conn_log_errno(c, "fstat");
        close(file_fd);
        send_error(c, 500, NULL);
        return;
    }

    /* 4) 디렉토리면 index.html 로 */
    if (S_ISDIR(st.st_mode)) {
        close(file_fd);
        if (!trailing_slash) {
            /* "/docs" → "/docs/" 로 보내야 index.html 안의 상대 경로(style.css)가
             * "/docs/style.css" 로 제대로 풀린다. */
            char loc[STATIC_MAX_PATH + 32];
            size_t plen = strcspn(req->target, "?#");
            int n = snprintf(loc, sizeof(loc), "Location: %.*s/\r\n", (int)plen, req->target);
            if (n < 0 || (size_t)n >= sizeof(loc)) {
                send_error(c, 414, NULL);
                return;
            }
            send_error(c, 301, loc);
            return;
        }
        if (strlen(real) + sizeof("/index.html") > sizeof(real)) {
            send_error(c, 414, NULL);
            return;
        }
        strcat(real, "/index.html");
        file_fd = open(real, O_RDONLY | O_NONBLOCK);
        if (file_fd < 0) {
            /* index.html 이 없는 디렉토리: 목록을 보여 주지 않는다(파일 이름 노출 방지). */
            send_error(c, errno == ENOENT ? 403 : status_from_errno(errno), NULL);
            return;
        }
        if (fstat(file_fd, &st) < 0) {
            conn_log_errno(c, "fstat");
            close(file_fd);
            send_error(c, 500, NULL);
            return;
        }
    }

    /* 일반 파일만 보낸다. 장치 파일, 소켓, FIFO 등은 거절. */
    if (!S_ISREG(st.st_mode)) {
        close(file_fd);
        send_error(c, 403, NULL);
        return;
    }

    /* 5) 헤더 먼저, 그다음 본문을 조각조각.
     *    ※ 500 같은 에러는 반드시 이 줄 "전에" 판단해야 한다. 헤더가 나간 뒤엔 되돌릴 수 없다. */
    const char *ctype = static_mime_type(real);
    char head[1024];
    int n = http_build_response_head(head, sizeof(head), 200, ctype,
                                     (size_t)st.st_size, NULL, c->keep_alive);
    if (n < 0 || send_all(c, head, (size_t)n) < 0) {
        c->keep_alive = 0;
        close(file_fd);
        return;
    }
    if (!head_only && send_file_body(c, file_fd, st.st_size) < 0) {
        c->keep_alive = 0; /* Content-Length 만큼 못 보냈으니 이 연결은 망가졌다 */
        close(file_fd);
        return;
    }
    close(file_fd);
    conn_log(c, "<- 200 OK %s (%lld bytes%s)%s", real, (long long)st.st_size,
             head_only ? ", HEAD" : "", c->keep_alive ? " [keep-alive]" : "");
}

/* ------------------------------------------------------------------ */
/* 연결 하나 처리                                                        */
/* ------------------------------------------------------------------ */

static void dispatch(struct conn *c, const struct http_request *req,
                     const char *body, size_t body_len)
{
    if (strcmp(req->method, "GET") == 0) {
        serve_file(c, req, 0);
        return;
    }
    if (strcmp(req->method, "HEAD") == 0) { /* GET 과 같은 헤더, 본문만 없음 */
        serve_file(c, req, 1);
        return;
    }

    if (strcmp(req->method, "POST") == 0) {
        /* echo: 받은 본문을 받은 Content-Type 그대로 돌려준다. (2단계 확인용, 5단계에서 교체) */
        const char *ctype = http_get_header(req, "Content-Type");
        send_response(c, 200, ctype ? ctype : "application/octet-stream",
                      body, body_len, NULL);
        return;
    }

    /* 405 응답에는 허용 메서드를 알려 주는 Allow 헤더가 필수다. */
    send_error(c, 405, "Allow: GET, HEAD, POST\r\n");
}

/* 요청 하나를 처리한다. 다음 요청을 이어서 받아도 되면 1, 연결을 닫아야 하면 0. */
static int handle_one_request(struct conn *c, int served)
{
    /* 1) 헤더 받기: "\r\n\r\n" 이 나올 때까지 */
    long head_len = read_head(c);
    if (head_len <= 0) {
        c->keep_alive = 0;
        switch (head_len) {
        case READ_TOO_LARGE: send_error(c, 431, NULL); break;
        case READ_TIMEOUT:
            conn_log(c, "header timed out after %d ms (%zu bytes)", HEADER_TIMEOUT_MS, c->len);
            send_error(c, 408, NULL);
            break;
        case READ_IDLE:
            if (served > 0)
                conn_log(c, "idle %d ms, closing", IDLE_TIMEOUT_MS);
            else
                conn_log(c, "no request within %d ms, closing", IDLE_TIMEOUT_MS);
            break;
        case READ_STOP:   conn_log(c, "shutting down, closing idle connection"); break;
        default:          break; /* READ_CLOSED, READ_ERROR: 보낼 상대가 없음 */
        }
        return 0;
    }

    /* 2) 파싱: 요청 라인 + 헤더 */
    struct http_request req;
    enum http_parse_result pr = http_parse_request(c->buf, (size_t)head_len, &req);
    if (pr != HTTP_PARSE_OK) {
        int status = pr == HTTP_PARSE_TOO_MANY_HEADERS ? 431
                   : pr == HTTP_PARSE_BAD_VERSION      ? 505
                                                       : 400;
        c->keep_alive = 0; /* 형식이 깨진 요청 뒤에 오는 바이트는 믿을 수 없다 */
        send_error(c, status, NULL);
        return 0;
    }

    pthread_mutex_lock(&stats_mu);
    stats.requests++;
    pthread_mutex_unlock(&stats_mu);

    /* 3) 이 응답 뒤에 연결을 유지할지 결정 (응답 헤더에 써야 하므로 보내기 "전에")
     *    - 클라이언트가 원하고 (HTTP 버전 + Connection 헤더)
     *    - 한 연결이 워커를 독점하지 않게 요청 수 한도 안이고
     *    - 서버가 종료 중이 아니고
     *    - 큐에서 기다리는 다른 연결이 없을 때만.
     *      스레드 풀에선 idle keep-alive 연결도 워커 하나를 통째로 붙잡는다.
     *      손님이 줄 서 있으면 지금 손님을 보내고 자리를 비워 주는 게 공평하다. */
    c->keep_alive = http_wants_keep_alive(&req)
                 && served + 1 < MAX_REQUESTS_PER_CONN
                 && !stop_requested(c->ctx)
                 && (!c->ctx->pool || pool_pending(c->ctx->pool) == 0);

    conn_log(c, "-> %s %s %s (%zu headers)", req.method, req.target, req.version,
             req.header_count);

    /* 4) 본문 받기: Content-Length 만큼 */
    size_t clen;
    if (http_content_length(&req, &clen) < 0) {
        c->keep_alive = 0;
        send_error(c, 400, NULL);
        return 0;
    }
    if (clen > HTTP_MAX_BODY) {
        c->keep_alive = 0; /* 본문을 안 읽고 버리므로 다음 요청의 시작 위치를 알 수 없다 */
        send_error(c, 413, NULL);
        return 0;
    }

    size_t extra = c->len - (size_t)head_len; /* 헤더 뒤에 이미 딸려 온 바이트 */
    char *body = NULL;
    if (clen > 0) {
        int timed_out;
        body = read_body(c, c->buf + head_len, extra, clen, &timed_out);
        if (!body) {
            c->keep_alive = 0;
            if (timed_out)
                send_error(c, 408, NULL);
            return 0;
        }
    }

    /* 5) 처리 + 응답 */
    dispatch(c, &req, body, clen);
    free(body);
    if (!c->keep_alive)
        return 0;

    /* 6) 다음 요청 준비: 이번 요청(헤더 + buf 안에 있던 본문 부분)을 버리고
     *    그 뒤에 딸려 온 바이트(파이프라이닝된 다음 요청)를 버퍼 앞으로 당긴다. */
    size_t consumed = (size_t)head_len + (extra < clen ? extra : clen);
    c->len -= consumed;
    memmove(c->buf, c->buf + consumed, c->len);
    return 1;
}

void server_handle_client(int client_fd, int worker_id, void *arg)
{
    struct conn c = { .fd = client_fd, .worker = worker_id, .ctx = arg };

    pthread_mutex_lock(&stats_mu);
    c.id = ++stats.connections;
    stats.active++;
    pthread_mutex_unlock(&stats_mu);

    /* 응답을 안 읽어 가는 클라이언트 때문에 send 가 영원히 멈추지 않게 한다. */
    struct timeval tv = { .tv_sec = SEND_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int served = 0;
    while (handle_one_request(&c, served)) /* keep-alive: 같은 연결에서 요청을 계속 받는다 */
        served++;

    close_gracefully(client_fd);

    pthread_mutex_lock(&stats_mu);
    stats.active--;
    pthread_mutex_unlock(&stats_mu);
}

void server_reject_busy(int client_fd)
{
    /* accept 스레드에서 호출되므로 절대 막히면 안 된다 → 이 소켓만 non-blocking 으로.
     * 갓 만든 연결의 송신 버퍼는 비어 있어서 이 정도 크기는 한 번에 들어간다. */
    static const char resp[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 24\r\n"
        "Connection: close\r\n"
        "Retry-After: 1\r\n"
        "\r\n"
        "503 Service Unavailable\n";
    fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) | O_NONBLOCK);
    (void)!send(client_fd, resp, sizeof(resp) - 1, 0);
    shutdown(client_fd, SHUT_WR);
    close(client_fd);
}
