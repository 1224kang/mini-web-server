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
#define SEND_TIMEOUT_MS   10000 /* 한 바이트도 못 보내고 버틸 수 있는 시간 (안 읽는 클라이언트) */
#define LINGER_TIMEOUT_MS 1000  /* 닫기 전 남은 입력을 비우는 최대 시간 */
#define LINGER_MAX_BYTES  (64 * 1024)
/* CGI/백엔드 타임아웃은 설정 파일에서 (config.h) */
#define UPSTREAM_MAX_RESPONSE (16 * 1024 * 1024) /* CGI 출력 최대 크기 */
#define PROXY_HEADER_LIMIT (16 * 1024)
#define PROXY_BUFFER_SIZE  (64 * 1024)
#define PROXY_CHUNK_OVERHEAD 32 /* 16진수 길이, CRLF, 마지막 0 청크 */
#define PROXY_MAX_INFORMATIONAL 16
#define MAX_REQUESTS_PER_CONN 100

#define FILE_CHUNK   (64 * 1024)  /* 파일을 이만큼씩 읽어 보낸다 */
#define WRITE_BUDGET (256 * 1024) /* 이벤트 한 번에 한 연결이 보낼 수 있는 최대 바이트.
                                   * 이게 없으면 빠른 클라이언트 하나가 20MB 를 다 받을 때까지
                                   * 루프를 독점해서 다른 연결이 전부 멈춘다. */

enum conn_state {
    ST_READ_HEAD, /* 요청 헤더를 모으는 중 (len == 0 이면 "다음 요청을 기다리며 노는 중") */
    ST_READ_BODY, /* Content-Length 만큼, 또는 chunked 의 마지막 청크까지 본문을 모으는 중 */
    ST_UPSTREAM,  /* CGI/백엔드에게 넘기고 답을 기다리는 중 (5·6단계) */
    ST_WRITE,     /* 응답(헤더 + 본문 또는 파일)을 내보내는 중 */
    ST_LINGER,    /* FIN 을 보냈고, 상대가 닫을 때까지 남은 입력을 버리는 중 */
    ST_DONE,      /* 루프가 정리하면 됨 */
};

/* 스레드 풀 버전에선 이 정보 대부분이 "워커 스택의 지역 변수 + 지금 실행 중인 줄"에
 * 자연스럽게 들어 있었다. 이벤트 루프에선 중간에 손을 놓고 루프로 돌아가야 하므로
 * "어디까지 했는지"를 전부 여기 적어 둬야 한다. 이게 상태 머신이다. */
enum up_kind { UP_NONE, UP_CGI, UP_PROXY };

/* 프록시 연결, 요청 전송, 응답 헤더 수신, 본문 중계 순서. */
enum proxy_step { PX_NONE, PX_CONNECTING, PX_SENDING, PX_READING_HEAD, PX_RELAYING };

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
    /* Transfer-Encoding: chunked 요청: body 에 디코딩한 본문을 모은다(body_len == body_have) */
    int body_chunked;
    size_t body_cap;
    struct proxy_chunks body_chunks;

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

    /* ---- 업스트림 (CGI / 프록시) ----
     *   CGI  : up_in_fd = 자식 stdin 파이프(본문을 쓴다), up_out_fd = 자식 stdout 파이프(읽는다)
     *   프록시: up_in_fd = -1,                         up_out_fd = 백엔드 소켓(쓰고 읽는다) */
    enum up_kind up_kind;
    pid_t cgi_pid;          /* 아직 cgi_release 하지 않은 자식 (0 이면 없음) */
    int up_in_fd, up_out_fd;
    enum proxy_step px_step; /* 프록시: 지금 몇 번째 단계인지 (CGI 는 PX_NONE) */
    long proxy_remaining;   /* 남은 백엔드 본문 바이트. -1이면 EOF까지 받는다 */
    int proxy_chunked_in, proxy_chunked_out;
    unsigned proxy_informational;
    struct proxy_chunks proxy_chunks;
    const char *up_send;    /* 업스트림에 보낼 것 (CGI: 요청 본문, 프록시: 다시 쓴 요청) */
    size_t up_send_len, up_send_sent;
    char *up_req;           /* 프록시: 다시 쓴 요청 (up_send 가 가리킴, 우리가 해제) */
    char *up_buf;           /* CGI 출력 */
    size_t up_len, up_cap;
    char up_name[PATH_MAX]; /* 로그용: 스크립트 경로 또는 백엔드 주소 */
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
    free(c->up_req);
    free(c->up_buf);
    c->up_req = c->up_buf = NULL;
    c->up_send = NULL;
    c->up_send_len = c->up_send_sent = c->up_len = c->up_cap = 0;
    c->px_step = PX_NONE;
    c->up_kind = UP_NONE;
    c->proxy_remaining = 0;
    c->proxy_chunked_in = c->proxy_chunked_out = 0;
    c->proxy_informational = 0;
    c->proxy_chunks = (struct proxy_chunks){ 0 };
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
    c->body_len = c->body_have = c->body_cap = 0;
    c->body_chunked = 0;
    c->body_chunks = (struct proxy_chunks){ 0 };
}

/* 상태를 바꾸는 유일한 곳. 상태가 바뀌면 그 상태의 마감 시각도 반드시 같이 정한다.
 * (`grep set_state` 로 모든 상태 전이를 한눈에 볼 수 있다.)
 * 같은 상태 안에서 마감만 늦추는 경우(조금이라도 보냈다, 첫 바이트가 왔다)는 c->deadline 을 직접 고친다. */
static void set_state(struct conn *c, enum conn_state st, long long deadline)
{
    c->state = st;
    c->deadline = deadline;
}

static void close_now(struct conn *c)
{
    set_state(c, ST_DONE, LLONG_MAX);
}

/* 안 읽은 데이터가 남은 채 close 하면 커널이 RST 를 보내 클라이언트가 응답을 못 읽을 수 있다.
 * 그래서 FIN 만 먼저 보내고(SHUT_WR), 상대가 닫을 때까지 입력을 조금 비운다.
 * 스레드 풀 버전의 close_gracefully 와 같은 일인데, 기다리는 대신 "상태"가 되었다. */
static void start_linger(struct conn *c, long long now)
{
    shutdown(c->fd, SHUT_WR);
    c->drained = 0;
    set_state(c, ST_LINGER, now + LINGER_TIMEOUT_MS);
}

/* 다음 요청을 받을 준비. 버퍼에 이미 다음 요청 일부가 있으면(파이프라이닝) 헤더 마감을,
 * 없으면 idle 마감을 건다. 파싱은 여기서 하지 않는다 → conn_handle 의 pump 가 한다. */
static void enter_read_head(struct conn *c, long long now)
{
    c->scanned = 0;
    c->head_only = 0;
    set_state(c, ST_READ_HEAD, now + (c->len > 0 ? c->cfg->header_timeout_ms : c->cfg->keepalive_timeout_ms));
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

/* out(+file_fd) 에 보낼 것을 다 채운 뒤 부른다: WRITE 상태로 바꾸고 바로 보내기 시작.
 * 대부분은 송신 버퍼에 바로 들어가므로 POLLOUT 을 기다리지 않고 먼저 시도한다. */
static void start_write(struct conn *c, long long now)
{
    c->out_sent = 0;
    set_state(c, ST_WRITE, now + SEND_TIMEOUT_MS);
    do_write(c, now);
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
    snprintf(c->summary, sizeof(c->summary), "<- %d %s (%zu bytes body)", status,
             http_reason(status), body_len);
    start_write(c, now);
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
    if (head_only) {
        close(file_fd);
    } else {
        c->file_fd = file_fd;
        c->file_left = st.st_size;
    }
    snprintf(c->summary, sizeof(c->summary), "<- 200 OK %s (%lld bytes%s)", real,
             (long long)st.st_size, head_only ? ", HEAD" : "");
    start_write(c, now);
}

/* ------------------------------------------------------------------ */
/* 업스트림 공통 (5·6단계): 보내기, 받기, 실패, 완료                          */
/* ------------------------------------------------------------------ */

static const char *up_label(const struct conn *c)
{
    return c->up_kind == UP_CGI ? "cgi" : "proxy";
}

/* 업스트림 쪽 문제는 클라이언트 잘못이 아니다. 요청은 끝까지 받았으니 연결은 계속 쓸 수 있다.
 *   502 Bad Gateway    : 업스트림이 "틀린" 답을 했다 (연결 거부, 실행 실패, 망가진 응답)
 *   504 Gateway Timeout: 업스트림이 "제때" 답을 안 했다 */
static void upstream_fail(struct conn *c, int status, const char *why, long long now)
{
    if (c->up_kind == UP_PROXY && c->out_sent > 0) {
        conn_log(c, "proxy %s: %s after response started", c->up_name, why);
        release_response(c);
        start_linger(c, now);
        return;
    }
    conn_log(c, "%s %s: %s -> %d", up_label(c), c->up_name, why, status);
    release_response(c); /* CGI 가 아직 돌고 있으면 여기서 SIGKILL */
    send_error(c, status, NULL, now);
}

/* CGI 출력을 모아 Content-Length를 계산한 뒤 응답을 만든다. */
static void cgi_complete(struct conn *c, long long now)
{
    if (c->cgi_pid > 0) {
        /* stdout 이 닫혔다 = 자식이 끝났거나 곧 끝난다. 이미 끝났으면 바로 거두고,
         * 아니면 고아 목록에 넣어 SIGCHLD 때 거둔다. (죽이지는 않는다) */
        cgi_release(c->cgi_pid, 0);
        c->cgi_pid = 0;
    }
    struct upstream_response r;
    if (upstream_parse_response(c->up_buf ? c->up_buf : "", c->up_len, UPSTREAM_CGI, &r) < 0) {
        upstream_fail(c, 502, c->up_len == 0 ? "empty response" : "malformed response", now);
        return;
    }
    size_t out_len;
    char *out = upstream_build_client_response(&r, c->head_only, c->keep_alive && !c->stopping,
                                               NULL, &out_len);
    if (!out) {
        upstream_fail(c, 500, "cannot build response", now);
        return;
    }
    /* r 은 up_buf 안을 가리키므로 release_upstream 전에 로그를 만들어 둔다. */
    snprintf(c->summary, sizeof(c->summary), "<- %d %s from %s %s (%zu bytes body)", r.status,
             r.reason, up_label(c), c->up_name, r.body_len);
    release_upstream(c);

    c->out = out;
    c->out_len = out_len;
    start_write(c, now);
}

/* 업스트림 출력을 "한 번" 읽는다. 더 있으면 poll 이 또 알려 준다. */
static void cgi_read(struct conn *c, long long now)
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
    if (n == 0) {
        cgi_complete(c, now);
        return;
    }
    c->up_len += (size_t)n;
}

/* up_send 를 보낼 수 있는 만큼 보낸다. CGI 는 다 보내면 stdin 을 닫아 "본문 끝(EOF)"을 알린다. */
static void upstream_write(struct conn *c, long long now)
{
    int fd = c->up_kind == UP_CGI ? c->up_in_fd : c->up_out_fd;
    while (c->up_send_sent < c->up_send_len) {
        ssize_t n = write(fd, c->up_send + c->up_send_sent, c->up_send_len - c->up_send_sent);
        if (n < 0) {
            if (would_block())
                return; /* 파이프/소켓 버퍼가 꽉 참 → POLLOUT 을 기다린다 */
            if (c->up_kind == UP_CGI && errno == EPIPE) {
                /* 스크립트가 본문을 다 안 읽고 stdin 을 닫았다. 흔한 일이라 에러가 아니다.
                 * (SIGPIPE 를 무시해 뒀기 때문에 서버가 죽지 않고 여기로 온다) */
                c->up_send_sent = c->up_send_len;
                break;
            }
            upstream_fail(c, 502, strerror(errno), now);
            return;
        }
        c->up_send_sent += (size_t)n;
        if (c->up_kind == UP_PROXY)
            c->deadline = now + c->cfg->proxy_timeout_ms;
    }
    if (c->up_kind == UP_CGI) {
        close(c->up_in_fd);
        c->up_in_fd = -1;
    } else {
        free(c->up_req);
        c->up_req = NULL;
        c->up_send = NULL;
        c->px_step = PX_READING_HEAD;
        c->deadline = now + c->cfg->proxy_timeout_ms;
    }
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
    c->up_kind = UP_CGI;
    c->cgi_pid = pid;
    c->up_in_fd = in_fd;
    c->up_out_fd = out_fd;
    c->up_send = c->body;
    c->up_send_len = c->body_len;
    c->up_send_sent = 0;
    snprintf(c->up_name, sizeof(c->up_name), "%s", script_name);
    set_state(c, ST_UPSTREAM, now + c->cfg->cgi_timeout_ms); /* 실행 전체에 대한 마감 */
    conn_log(c, "cgi %s started (pid %d, %zu bytes to stdin)", script_name, (int)pid, c->body_len);
    upstream_write(c, now); /* 본문이 없으면 바로 stdin 을 닫는다 */
}

/* ------------------------------------------------------------------ */
/* 6단계: 리버스 프록시                                                   */
/* ------------------------------------------------------------------ */

/* 클라이언트는 우리 서버에게 물었다고 생각한다. 실제 답은 백엔드가 하고, 우리는
 * 요청을 다시 써서 넘기고(Host, X-Forwarded-For), 답을 다시 써서 돌려준다(Connection, Via). */
static void start_proxy(struct conn *c, struct proxy_route *route, long long now)
{
    const struct backend *b = config_pick_backend(route); /* Round-Robin */
    c->up_kind = UP_PROXY;
    snprintf(c->up_name, sizeof(c->up_name), "%s", b->name);

    c->up_req = proxy_build_request(&c->req, c->body, c->body_len, c->peer_ip, b->name,
                                    &c->up_send_len);
    if (!c->up_req) {
        upstream_fail(c, 500, "cannot build request", now);
        return;
    }
    c->up_send = c->up_req;
    c->up_send_sent = 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        upstream_fail(c, 502, strerror(errno), now);
        return;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    c->up_out_fd = fd;
    conn_log(c, "proxy %s %s -> %s (route %s)", c->req.method, c->req.target, b->name,
             route->prefix);

    /* non-blocking connect: 바로 끝나지 않으면 EINPROGRESS 를 돌려주고, 연결이 되든 실패하든
     * 소켓이 "쓸 수 있음(POLLOUT)"이 된다. 결과는 그때 SO_ERROR 로 확인한다. */
    if (connect(fd, (const struct sockaddr *)&b->addr, sizeof(b->addr)) == 0) {
        c->px_step = PX_SENDING; /* 로컬 백엔드면 바로 연결될 수도 있다 */
        set_state(c, ST_UPSTREAM, now + c->cfg->proxy_timeout_ms);
        upstream_write(c, now);
    } else if (errno == EINPROGRESS) {
        c->px_step = PX_CONNECTING;
        set_state(c, ST_UPSTREAM, now + c->cfg->proxy_connect_timeout_ms);
    } else {
        upstream_fail(c, 502, strerror(errno), now); /* 보통 ECONNREFUSED: 백엔드가 죽어 있다 */
    }
}

/* PX_CONNECTING 에서 POLLOUT 이 왔다 = connect 가 끝났다(성공이든 실패든). SO_ERROR 로 결과 확인. */
static void proxy_check_connect(struct conn *c, long long now)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(c->up_out_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        err = errno;
    if (err) {
        upstream_fail(c, 502, strerror(err), now);
        return;
    }
    c->px_step = PX_SENDING;
    c->deadline = now + c->cfg->proxy_timeout_ms; /* 여기부터는 "응답 대기" 마감 */
    upstream_write(c, now);
}

static int proxy_has_output(const struct conn *c)
{
    return c->out_sent < c->out_len || c->chunk_sent < c->chunk_len;
}

static void proxy_finish_input(struct conn *c)
{
    close(c->up_out_fd);
    c->up_out_fd = -1;
}

/* 출력 대기 중에는 백엔드를 poll에서 뺀다. 연결 하나가 본문 조각 하나만 보유한다. */
static void proxy_flush(struct conn *c, long long now)
{
    const char *buf;
    size_t len, *sent;
    if (c->out_sent < c->out_len) {
        buf = c->out;
        len = c->out_len;
        sent = &c->out_sent;
    } else {
        buf = c->chunk;
        len = c->chunk_len;
        sent = &c->chunk_sent;
    }
    ssize_t n = send(c->fd, buf + *sent, len - *sent, 0);
    if (n <= 0) {
        if (n == 0 || !would_block())
            close_now(c);
        return;
    }
    *sent += (size_t)n;
    if (proxy_has_output(c))
        c->deadline = now + SEND_TIMEOUT_MS;
    else if (c->up_out_fd < 0)
        finish_response(c, now);
    else {
        c->chunk_len = c->chunk_sent = 0;
        c->deadline = now + c->cfg->proxy_timeout_ms;
    }
}

static int proxy_queue_body(struct conn *c, size_t len, long long now)
{
    if (c->proxy_chunked_in) {
        int done = proxy_decode_chunks(&c->proxy_chunks, c->chunk, len, &len);
        if (done < 0) {
            upstream_fail(c, 502, "malformed chunked response", now);
            return -1;
        }
        if (c->proxy_chunked_out) {
            if (len > 0) {
                char prefix[PROXY_CHUNK_OVERHEAD];
                size_t prefix_len = (size_t)snprintf(prefix, sizeof(prefix), "%zx\r\n", len);
                memmove(c->chunk + prefix_len, c->chunk, len);
                memcpy(c->chunk, prefix, prefix_len);
                len += prefix_len;
                memcpy(c->chunk + len, "\r\n", 2);
                len += 2;
            }
            if (done) {
                memcpy(c->chunk + len, "0\r\n\r\n", 5);
                len += 5;
            }
        }
        if (done)
            proxy_finish_input(c);
    } else if (c->proxy_remaining >= 0) {
        if (len > (size_t)c->proxy_remaining)
            len = (size_t)c->proxy_remaining;
        c->proxy_remaining -= (long)len;
        if (c->proxy_remaining == 0)
            proxy_finish_input(c);
    }
    c->chunk_len = len;
    c->chunk_sent = 0;
    return 0;
}

static void proxy_read_head(struct conn *c, long long now)
{
    if (!c->chunk && !(c->chunk = malloc(PROXY_BUFFER_SIZE + PROXY_CHUNK_OVERHEAD))) {
        upstream_fail(c, 500, "out of memory", now);
        return;
    }
    size_t previous = c->up_len;
    ssize_t n = read(c->up_out_fd, c->chunk + previous, PROXY_HEADER_LIMIT - previous);
    if (n <= 0) {
        if (n == 0 || !would_block())
            upstream_fail(c, 502, "incomplete response header", now);
        return;
    }
    c->up_len += (size_t)n;
    c->deadline = now + c->cfg->proxy_timeout_ms;
    size_t start = previous > 3 ? previous - 3 : 0;
    struct upstream_response r;
    size_t head_len;
    for (;;) {
        long end = http_find_head_end(c->chunk + start, c->up_len - start);
        if (end < 0) {
            if (c->up_len == PROXY_HEADER_LIMIT)
                upstream_fail(c, 502, "response header too large", now);
            return;
        }

        head_len = start + (size_t)end;
        if (upstream_parse_response(c->chunk, head_len, UPSTREAM_HTTP, &r) < 0) {
            upstream_fail(c, 502, "malformed response header", now);
            return;
        }
        if (r.status >= 200)
            break;
        if (r.status == 101 || ++c->proxy_informational > PROXY_MAX_INFORMATIONAL) {
            upstream_fail(c, 502, "unexpected informational response", now);
            return;
        }
        /* 백엔드의 중간 응답 뒤에 최종 헤더가 같은 read에 올 수 있다. */
        c->up_len -= head_len;
        memmove(c->chunk, c->chunk + head_len, c->up_len);
        start = 0;
    }
    int no_body = c->head_only || r.status == 204 || r.status == 304;
    c->proxy_remaining = no_body ? 0 : r.content_length;
    c->proxy_chunked_in = !no_body && r.chunked;
    c->proxy_chunked_out = c->proxy_chunked_in && strcmp(c->req.version, "HTTP/1.1") == 0;
    if (c->proxy_remaining < 0 && !c->proxy_chunked_out)
        c->keep_alive = 0; /* 길이 없는 본문은 클라이언트에게도 EOF로 끝을 알린다. */
    c->out = upstream_build_proxy_head(&r, c->req.version, c->proxy_chunked_out,
                                      c->keep_alive && !c->stopping, &c->out_len);
    if (!c->out) {
        upstream_fail(c, 500, "cannot build response header", now);
        return;
    }
    snprintf(c->summary, sizeof(c->summary), "<- %d %s from proxy %s",
             r.status, r.reason, c->up_name);
    c->px_step = PX_RELAYING;
    size_t received = c->up_len - head_len;
    memmove(c->chunk, c->chunk + head_len, received);
    if (proxy_queue_body(c, received, now) == 0)
        c->deadline = now + SEND_TIMEOUT_MS;
}

static void proxy_read_body(struct conn *c, long long now)
{
    size_t limit = PROXY_BUFFER_SIZE;
    if (c->proxy_remaining >= 0 && (size_t)c->proxy_remaining < limit)
        limit = (size_t)c->proxy_remaining;
    ssize_t n = read(c->up_out_fd, c->chunk, limit);
    if (n < 0) {
        if (!would_block())
            upstream_fail(c, 502, strerror(errno), now);
    } else if (n == 0) {
        if (c->proxy_remaining >= 0 || c->proxy_chunked_in)
            upstream_fail(c, 502, "response truncated", now);
        else
            finish_response(c, now);
    } else if (proxy_queue_body(c, (size_t)n, now) == 0) {
        if (proxy_has_output(c))
            c->deadline = now + SEND_TIMEOUT_MS;
        else if (c->up_out_fd < 0)
            finish_response(c, now);
        else
            c->deadline = now + c->cfg->proxy_timeout_ms;
    }
}

/* 백엔드 소켓에 신호가 왔다. 지금 단계가 할 일 하나만 한다. */
static void proxy_io(struct conn *c, short revents, long long now)
{
    switch (c->px_step) {
    case PX_CONNECTING: proxy_check_connect(c, now); break; /* 다 되면 → PX_SENDING */
    case PX_SENDING: upstream_write(c, now); break;
    case PX_READING_HEAD:
        if (revents & (POLLIN | POLLHUP | POLLERR))
            proxy_read_head(c, now);
        break;
    case PX_RELAYING:
        if (revents & (POLLIN | POLLHUP | POLLERR))
            proxy_read_body(c, now);
        break;
    case PX_NONE:       break;
    }
}

/* ------------------------------------------------------------------ */
/* 어디로 보낼까                                                         */
/* ------------------------------------------------------------------ */

/* 우선순위: 프록시 경로 → /cgi-bin/ → /builtin/ → 정적 파일 */
static void dispatch(struct conn *c, long long now)
{
    const char *t = c->req.target, *m = c->req.method;

    struct proxy_route *route = config_match_route(c->cfg, t);
    if (route) {
        start_proxy(c, route, now);
        return;
    }
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

/* ---- Transfer-Encoding: chunked 요청 본문 ----
 *
 *   "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"  →  body = "hello world" (11 바이트)
 *
 * 끝이 어디인지 미리 모르므로 받은 만큼 디코더에 넣고, 디코딩된 바이트만 body 에 붙인다.
 * 원본 바이트는 buf 의 consumed 뒤에 받아서 그 자리에서 디코딩하므로, 끝난 뒤 남는 바이트
 * (파이프라이닝된 다음 요청)는 그대로 buf 에 남아 finish_response 가 앞으로 당긴다.
 * 디코딩이 끝나면 body_len 이 정해지므로 CGI/프록시/내장 핸들러는 Content-Length 요청과
 * 똑같이 다룬다(프록시는 백엔드에 Content-Length 로 보낸다). */

#define CHUNKED_MIN_ROOM 1024 /* buf 남은 공간이 이보다 작으면 임시 버퍼로 받는다 */

static ssize_t recv_or_close(struct conn *c, void *dst, size_t room);

/* 디코딩된 n 바이트를 body 뒤에 붙인다. 한도를 넘거나 메모리가 없으면 응답까지 하고 -1. */
static int append_body(struct conn *c, const char *p, size_t n, long long now)
{
    if (n == 0)
        return 0;
    if (n > HTTP_MAX_BODY - c->body_len) {
        conn_log(c, "chunked body exceeds %d bytes", HTTP_MAX_BODY);
        fail(c, 413, now);
        return -1;
    }
    if (c->body_len + n > c->body_cap) {
        size_t cap = c->body_cap ? c->body_cap : 4096;
        while (cap < c->body_len + n)
            cap *= 2;
        if (cap > HTTP_MAX_BODY)
            cap = HTTP_MAX_BODY;
        char *nb = realloc(c->body, cap);
        if (!nb) {
            conn_log_errno(c, "realloc");
            fail(c, 500, now);
            return -1;
        }
        c->body = nb;
        c->body_cap = cap;
    }
    memcpy(c->body + c->body_len, p, n);
    c->body_len += n;
    c->body_have = c->body_len;
    return 0;
}

/* 원본 바이트 p[0..n) 을 디코딩해 body 에 붙인다.
 * 1 = 본문 끝(*used 뒤는 다음 요청), 0 = 더 받아야 함(n 전부 소비), -1 = 이미 에러 응답함. */
static int chunked_input(struct conn *c, char *p, size_t n, size_t *used, long long now)
{
    size_t decoded;
    int done = http_decode_chunks(&c->body_chunks, p, n, &decoded, used);
    if (done < 0) {
        conn_log(c, "malformed chunked request body");
        fail(c, 400, now);
        return -1;
    }
    if (append_body(c, p, decoded, now) < 0)
        return -1;
    return done;
}

/* buf[consumed..len) 에 있는 원본 바이트를 처리한다. 끝까지 받았으면 dispatch. */
static void feed_chunked_buf(struct conn *c, long long now)
{
    size_t used;
    int done = chunked_input(c, c->buf + c->consumed, c->len - c->consumed, &used, now);
    if (done < 0)
        return;
    c->consumed += used;
    if (!done) {
        c->len = c->consumed; /* 처리한 원본은 버리고 그 자리에 다음 조각을 받는다 */
        return;
    }
    conn_log(c, "chunked request body: %zu bytes", c->body_len);
    dispatch(c, now);
}

static void start_chunked_body(struct conn *c, size_t head_len, long long now)
{
    c->body_chunked = 1;
    c->body_chunks = (struct proxy_chunks){ 0 };
    c->consumed = head_len;
    set_state(c, ST_READ_BODY, now + c->cfg->body_timeout_ms);
    if (c->len > c->consumed)
        feed_chunked_buf(c, now); /* 헤더와 함께 딸려 온 본문부터 */
}

/* ST_READ_BODY + chunked: 받은 원본을 디코딩한다. */
static void read_chunked_body(struct conn *c, long long now)
{
    size_t room = sizeof(c->buf) - c->len;
    if (room >= CHUNKED_MIN_ROOM) {
        ssize_t n = recv_or_close(c, c->buf + c->len, room);
        if (n == 0)
            conn_log(c, "client closed before finishing chunked body (%zu bytes)", c->body_len);
        if (n <= 0)
            return;
        c->len += (size_t)n;
        feed_chunked_buf(c, now);
        return;
    }

    /* 헤더가 buf 를 거의 다 채웠다. 남는 바이트를 다음 요청으로 보관할 자리가 없으므로
     * 임시 버퍼로 받고, 이번 응답 뒤에 연결을 닫는다(keep-alive 포기). */
    char tmp[16 * 1024];
    c->keep_alive = 0;
    ssize_t n = recv_or_close(c, tmp, sizeof(tmp));
    if (n == 0)
        conn_log(c, "client closed before finishing chunked body (%zu bytes)", c->body_len);
    if (n <= 0)
        return;
    size_t used;
    int done = chunked_input(c, tmp, (size_t)n, &used, now);
    if (done == 1) {
        conn_log(c, "chunked request body: %zu bytes", c->body_len);
        dispatch(c, now);
    }
}

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

    enum http_body_kind kind;
    size_t clen;
    int status = http_body_framing(&c->req, &kind, &clen);
    if (status != 0) {
        /* 본문 끝을 알 수 없으니 이 연결은 더 쓸 수 없다 → fail 이 keep-alive 를 끈다 */
        conn_log(c, "bad body framing -> %d", status);
        fail(c, status, now);
        return;
    }
    if (kind == HTTP_BODY_CHUNKED) {
        start_chunked_body(c, head_len, now);
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
            set_state(c, ST_READ_BODY, now + c->cfg->body_timeout_ms);
            return; /* 나머지는 POLLIN 이 올 때마다 on_read_body 가 채운다 */
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

/* 클라이언트 소켓에서 recv 를 "한 번만" 한다. (더 남아 있으면 poll 이 다음 바퀴에 또 알려 준다)
 *   > 0 : 받은 바이트 수
 *     0 : 상대가 닫았거나 에러 → 이미 close_now 했음
 *    -1 : 지금은 받을 게 없음(EAGAIN) → 다음 POLLIN 을 기다리면 됨 */
static ssize_t recv_or_close(struct conn *c, void *dst, size_t room)
{
    ssize_t n = recv(c->fd, dst, room, 0);
    if (n > 0)
        return n;
    if (n < 0 && would_block())
        return -1;
    if (n < 0 && errno != ECONNRESET && c->state != ST_LINGER)
        conn_log_errno(c, "recv");
    close_now(c);
    return 0;
}

/* ---- 상태마다 함수 하나: conn_handle 이 지금 상태에 맞는 것 하나만 부른다 ---- */

/* ST_READ_HEAD: 받아서 buf 에 쌓기만 한다. "\r\n\r\n" 찾기와 파싱은 pump → try_head 가 한다. */
static void on_read_head(struct conn *c, short revents, long long now)
{
    if (!(revents & (POLLIN | POLLHUP | POLLERR)))
        return;
    ssize_t n = recv_or_close(c, c->buf + c->len, sizeof(c->buf) - c->len);
    if (n <= 0)
        return;
    if (c->len == 0)
        c->deadline = now + c->cfg->header_timeout_ms; /* 첫 바이트: 여기서부터 헤더 마감 시작 */
    c->len += (size_t)n;
}

/* ST_READ_BODY: Content-Length 만큼 body 에 채운다. 다 차면 어디로 보낼지 정한다. */
static void on_read_body(struct conn *c, short revents, long long now)
{
    if (!(revents & (POLLIN | POLLHUP | POLLERR)))
        return;
    if (c->body_chunked) {
        read_chunked_body(c, now);
        return;
    }
    ssize_t n = recv_or_close(c, c->body + c->body_have, c->body_len - c->body_have);
    if (n == 0)
        conn_log(c, "client closed before sending full body (%zu/%zu)", c->body_have, c->body_len);
    if (n <= 0)
        return;
    c->body_have += (size_t)n;
    if (c->body_have == c->body_len)
        dispatch(c, now); /* → 프록시 / CGI 면 ST_UPSTREAM, 나머지는 ST_WRITE */
}

/* ST_UPSTREAM: CGI 파이프 또는 프록시의 소켓 이벤트를 처리한다. */
static void on_upstream(struct conn *c, const struct pollfd *pfds, int n, long long now)
{
    if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        /* events=0 이어도 이 셋은 보고된다. 무시하면 poll 이 매번 즉시 돌아와 헛돈다. */
        conn_log(c, "client went away while waiting for %s", up_label(c));
        release_upstream(c);
        close_now(c);
        return;
    }
    if (c->up_kind == UP_PROXY && (pfds[0].revents & POLLOUT)) {
        proxy_flush(c, now);
        return;
    }
    for (int i = 1; i < n && c->state == ST_UPSTREAM; i++) {
        if (!pfds[i].revents)
            continue;
        if (pfds[i].fd == c->up_in_fd)
            upstream_write(c, now);                 /* CGI stdin: 본문 쓰기 */
        else if (pfds[i].fd == c->up_out_fd && c->up_kind == UP_PROXY)
            proxy_io(c, pfds[i].revents, now);      /* 백엔드 소켓: 단계별로 */
        else if (pfds[i].fd == c->up_out_fd)
            cgi_read(c, now);                       /* CGI stdout: 출력 모으기 */
    }
}

/* ST_WRITE: 보낼 수 있는 만큼 보낸다. 다 보내면 do_write 가 finish_response 를 부른다.
 * POLLHUP/POLLERR 도 같이 처리한다: 이어지는 send 가 -1 로 이유를 알려 준다. */
static void on_write(struct conn *c, short revents, long long now)
{
    if (revents & (POLLOUT | POLLHUP | POLLERR))
        do_write(c, now);
}

/* ST_LINGER: 남은 입력을 읽어서 버린다. 상대가 닫거나(recv 0) 너무 많이 오면 끝. */
static void on_linger(struct conn *c, short revents)
{
    if (!(revents & (POLLIN | POLLHUP | POLLERR)))
        return;
    char sink[4096];
    ssize_t n = recv_or_close(c, sink, sizeof(sink));
    if (n <= 0)
        return;
    c->drained += (size_t)n;
    if (c->drained >= LINGER_MAX_BYTES)
        close_now(c);
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
    case ST_UPSTREAM: return c->up_kind == UP_PROXY && proxy_has_output(c) ? POLLOUT : 0;
    default:        return 0;
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
        if (c->up_out_fd >= 0 && (c->up_kind == UP_CGI || !proxy_has_output(c))) {
            short ev = POLLIN;
            if (c->px_step == PX_CONNECTING || c->px_step == PX_SENDING)
                ev = POLLOUT; /* 연결 완료 / 요청 보내기를 기다리는 중 */
            out[n++] = (struct pollfd){ .fd = c->up_out_fd, .events = ev };
        }
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

/* poll 이 이 연결의 fd 에 신호를 줬다. 지금 상태의 함수 하나만 부른다.
 * 그 함수가 상태를 바꿨을 수 있으므로(예: 응답을 다 보내고 READ_HEAD 로), 마지막에 pump 가
 * "기다리지 않고 더 할 수 있는 일"(buf 에 이미 와 있는 다음 요청 파싱)을 마저 한다. */
void conn_handle(struct conn *c, const struct pollfd *pfds, int n, long long now)
{
    short revents = pfds[0].revents; /* pfds[0] 은 항상 클라이언트 소켓 */
    switch (c->state) {
    case ST_READ_HEAD: on_read_head(c, revents, now);  break;
    case ST_READ_BODY: on_read_body(c, revents, now);  break;
    case ST_UPSTREAM:  on_upstream(c, pfds, n, now);   break;
    case ST_WRITE:     on_write(c, revents, now);      break;
    case ST_LINGER:    on_linger(c, revents);          break;
    case ST_DONE:      break;
    }
    pump(c, now);
}

void conn_timeout(struct conn *c, long long now)
{
    switch (c->state) {
    case ST_READ_HEAD:
        if (c->len == 0) {
            conn_log(c, c->served > 0 ? "idle %d ms, closing" : "no request within %d ms, closing",
                     c->cfg->keepalive_timeout_ms);
            close_now(c);
        } else {
            conn_log(c, "header timed out after %d ms (%zu bytes)", c->cfg->header_timeout_ms, c->len);
            fail(c, 408, now);
        }
        break;
    case ST_READ_BODY:
        conn_log(c, "body timed out (%zu/%zu)", c->body_have, c->body_len);
        release_response(c);
        fail(c, 408, now);
        break;
    case ST_UPSTREAM:
        if (c->up_kind == UP_CGI)
            upstream_fail(c, 504, "timed out, killing", now);
        else if (proxy_has_output(c)) {
            conn_log(c, "proxy send timed out (client not reading)");
            close_now(c);
        } else
            upstream_fail(c, 504, c->px_step == PX_CONNECTING ? "connect timed out" : "response timed out", now);
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
