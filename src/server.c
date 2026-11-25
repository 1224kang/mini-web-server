/*
 * server.c - 연결 수락(메인 스레드) + 연결 처리(워커 스레드) + 종료 처리
 *
 *   [메인 스레드]                         [워커 스레드 x N]
 *   poll(listen_fd, wake_pipe)            connection_task()
 *     ├─ accept -> 작업 큐에 submit  ──▶    for (keep-alive) {
 *     │   (큐가 가득 차면 503)                  다음 요청 대기 (poll + 타임아웃)
 *     └─ wake_pipe 읽기 가능 -> 종료             http_read_request -> dispatch -> 로그
 *                                           }
 *                                           close(fd)
 */
#include "server.h"
#include "http.h"
#include "proxy_handler.h"
#include "static_handler.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* 에러 응답 후 남은 입력을 버릴 때의 한계 */
#define DRAIN_TIMEOUT_USEC  500000          /* recv 1회당 최대 대기 0.5초 */
#define DRAIN_MAX_BYTES     (1024 * 1024)   /* 최대 1MB 까지만 읽어 버림  */
#define ACCEPT_BATCH        64              /* poll 한 번에 최대 몇 개까지 accept 할지 */

/* ------------------------------------------------------------------ */
/* Phase 5: 시그널 처리                                                 */
/* ------------------------------------------------------------------ */

/* lock-free atomic 은 시그널 핸들러에서 써도 안전하고(C11), 워커 스레드가 읽어도 데이터 경쟁이 없다 */
static atomic_int g_stopping = 0;
static int        g_wake_write_fd = -1;

/* 시그널 핸들러 안에서는 async-signal-safe 함수(write, _exit 등)만 쓸 수 있다.
 * printf/malloc/mutex 는 금지 -> 플래그를 세우고 파이프에 1바이트 써서 메인 스레드를 깨운다. */
static void on_shutdown_signal(int sig)
{
    int saved_errno = errno;

    if (atomic_exchange(&g_stopping, 1)) {
        /* 두 번째 Ctrl+C: 기다리지 않고 즉시 종료 */
        static const char msg[] = "\n[WARN] second signal received, forcing exit\n";
        if (write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0) { /* 무시 */ }
        _exit(128 + sig);
    }
    /* 파이프를 일부러 비우지 않는다 -> 계속 "읽기 가능" 상태로 남아서
     * poll 하는 모든 스레드(메인 + keep-alive 대기 중인 워커)에게 종료를 알리는 방송이 된다. */
    if (g_wake_write_fd >= 0 && write(g_wake_write_fd, "x", 1) < 0) { /* 무시 */ }
    errno = saved_errno;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    /* 끊긴 소켓에 쓸 때 프로세스가 죽지 않도록 (send 는 -1/EPIPE 로 실패) */
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
        return -1;

    /* SA_RESTART 를 주지 않는다 -> 블로킹 중인 시스템 콜이 EINTR 로 깨어난다 */
    sa.sa_handler = on_shutdown_signal;
    sa.sa_flags   = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    return 0;
}

static int server_is_stopping(void)
{
    return atomic_load(&g_stopping);
}

/* ------------------------------------------------------------------ */
/* 소켓 유틸                                                           */
/* ------------------------------------------------------------------ */

static int set_nonblocking(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags);
}

static void set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* Phase 3: 클라이언트 소켓 읽기/쓰기 타임아웃.
 * 이게 없으면 요청을 보내다 멈춘 클라이언트가 워커 하나를 영원히 붙잡는다. */
static void set_socket_timeouts(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* ------------------------------------------------------------------ */
/* Connection handling (워커 스레드에서 실행)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    server_t *srv;
    int       fd;
    char      ip[INET_ADDRSTRLEN];
    char      peer[INET_ADDRSTRLEN + 8];
} conn_task_t;

/* 접근 로그: 127.0.0.1:34678 "GET /index.html HTTP/1.1" 200
 * printf 는 POSIX 상 스레드 안전(FILE 단위 잠금)이므로 한 줄씩 섞이지 않는다. */
static void log_access(const char *peer, const http_request_t *req, int status)
{
    if (req->method_str[0] != '\0' && req->target[0] != '\0' && req->version_major != 0)
        printf("[INFO] %s \"%s %s HTTP/%d.%d\" %d\n", peer, req->method_str, req->target,
               req->version_major, req->version_minor, status);
    else
        printf("[INFO] %s \"-\" %d\n", peer, status);
}

/* 에러 응답 후 바로 close() 하면, 아직 읽지 않은 요청 데이터가 커널 수신 버퍼에 남아 있어
 * 커널이 FIN 대신 RST 를 보낸다. 그러면 클라이언트가 우리가 보낸 에러 응답을 읽기도 전에
 * "Connection reset by peer" 를 받을 수 있다.
 * 그래서 송신 방향만 먼저 닫고(FIN 전송), 남은 입력을 짧게 읽어 버린 뒤 닫는다 (lingering close). */
static void drain_before_close(int fd)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = DRAIN_TIMEOUT_USEC };
    char   sink[4096];
    size_t total = 0;

    shutdown(fd, SHUT_WR);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (total < DRAIN_MAX_BYTES) {
        ssize_t n = recv(fd, sink, sizeof(sink), 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)                 /* EOF, 타임아웃(EAGAIN), 에러 -> 끝 */
            break;
        total += (size_t)n;
    }
}

/* 다음 요청이 올 때까지 대기. 1 = 클라이언트 데이터 도착, 0 = 타임아웃/서버 종료/에러 */
static int wait_for_request(int fd, int wake_fd, int timeout_ms)
{
    struct pollfd p[2] = {
        { .fd = fd,      .events = POLLIN, .revents = 0 },
        { .fd = wake_fd, .events = POLLIN, .revents = 0 },
    };
    int r;

    do {
        r = poll(p, 2, timeout_ms);
    } while (r < 0 && errno == EINTR);

    if (r <= 0)
        return 0;
    /* 종료 중이어도 이미 도착한 요청은 처리해 준다 (graceful) */
    if (p[0].revents & (POLLIN | POLLHUP | POLLERR))
        return 1;
    return 0;
}

static int reply(int fd, http_request_t *req, int status, const char *extra)
{
    if (http_send_error(fd, status, extra, req->keep_alive) < 0)
        req->keep_alive = 0;
    return status;
}

/* 파싱이 끝난 요청을 알맞은 핸들러로 보낸다. 반환: 응답한 상태 코드. */
static int dispatch(server_t *srv, int client_fd, http_request_t *req, const char *client_ip)
{
    if (req->method == HTTP_METHOD_UNKNOWN)
        return reply(client_fd, req, 405, "Allow: GET, POST\r\n");

    /* 프록시로 가는 요청도 "/api/../secret" 같은 경로는 미리 막는다 */
    if (http_path_has_dotdot(req->path))
        return reply(client_fd, req, 403, NULL);

    /* Phase 4: 라우팅 규칙과 prefix 가 맞으면 리버스 프록시, 아니면 정적 파일 */
    proxy_route_t *route = proxy_match(&srv->proxy, req->path);
    if (route != NULL)
        return proxy_handle(client_fd, req, &srv->proxy, route, client_ip);

    return static_handle(client_fd, req, srv->doc_root);
}

//연결 하나를 처리: keep-alive 루프 (요청 수신 -> 파싱 -> 응답 -> 반복)
static void connection_task(void *arg)
{
    conn_task_t           *t   = arg;
    server_t              *srv = t->srv;
    const server_config_t *cfg = &srv->cfg;
    http_conn_t            conn;
    http_request_t         req;
    int                    served = 0;

#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(t->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    set_socket_timeouts(t->fd, cfg->request_timeout_sec);
    http_conn_init(&conn, t->fd);

    for (;;) {
        /* 버퍼에 다음 요청(파이프라이닝)이 이미 있으면 기다릴 필요 없음.
         * 첫 요청은 request_timeout, 이후 유휴 대기는 keepalive_timeout. */
        if (conn.len == 0) {
            int sec = served == 0 ? cfg->request_timeout_sec : cfg->keepalive_timeout_sec;
            if (!wait_for_request(t->fd, srv->wake_pipe[0], sec * 1000)) {
                if (served == 0)
                    printf("[INFO] %s closed: no request (timeout or shutdown)\n", t->peer);
                break;
            }
        }

        http_request_init(&req);
        int rc = http_read_request(&conn, &req);

        if (rc == HTTP_READ_EOF || rc == HTTP_READ_TIMEOUT) {
            if (served == 0)
                printf("[INFO] %s closed connection without sending a request\n", t->peer);
            http_request_free(&req);
            break;
        }
        if (rc == HTTP_READ_IO_ERROR) {
            fprintf(stderr, "[WARN] %s recv/send failed (errno=%d)\n", t->peer, errno);
            http_request_free(&req);
            break;
        }
        if (rc > 0) {
            /* 파싱 단계 에러 (400/408/413/414/501/505 ...) -> 응답 후 연결 종료 */
            http_send_error(t->fd, rc, NULL, 0);
            log_access(t->peer, &req, rc);
            http_request_free(&req);
            drain_before_close(t->fd);
            break;
        }

        served++;
        /* 서버 종료 중이거나 한 연결의 요청 수 한도에 닿으면 이번 응답을 마지막으로 */
        if (server_is_stopping() || served >= cfg->max_keepalive_requests)
            req.keep_alive = 0;

        int status = dispatch(srv, t->fd, &req, t->ip);
        log_access(t->peer, &req, status);

        int keep = req.keep_alive;
        http_request_free(&req);
        if (!keep)
            break;
    }

    close(t->fd);
    free(t);
}

/* ------------------------------------------------------------------ */
/* Accept (메인 스레드)                                                 */
/* ------------------------------------------------------------------ */

/* 큐가 가득 찼을 때: 메인 스레드가 막히지 않도록 짧게 503 을 보내고 바로 닫는다 */
static void reject_busy(int fd, const char *peer)
{
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    http_send_error(fd, 503, "Retry-After: 1\r\n", 0);
    close(fd);
    printf("[WARN] %s rejected: work queue full (503)\n", peer);
}

static void accept_connections(server_t *srv)
{
    for (int i = 0; i < ACCEPT_BATCH; i++) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);

        int fd = accept(srv->listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;                         /* 대기 중인 연결을 다 받았음 */
            if (errno == ECONNABORTED)
                continue;                       /* handshake 도중 클라이언트가 끊음 */
            fprintf(stderr, "[WARN] accept failed: %s\n", strerror(errno));
            if (errno == EMFILE || errno == ENFILE) {
                /* fd 고갈: 리스닝 소켓이 계속 "읽기 가능"이라 바로 재시도하면 CPU 를 태운다 */
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
            return;
        }

        /* macOS/BSD 는 accept 한 소켓이 리스닝 소켓의 O_NONBLOCK 을 물려받는다 -> 명시적으로 해제 */
        set_nonblocking(fd, 0);
        set_cloexec(fd);

        conn_task_t *t = malloc(sizeof(*t));
        if (t == NULL) {
            close(fd);
            continue;
        }
        t->srv = srv;
        t->fd  = fd;
        if (inet_ntop(AF_INET, &cli.sin_addr, t->ip, sizeof(t->ip)) == NULL)
            snprintf(t->ip, sizeof(t->ip), "unknown");
        snprintf(t->peer, sizeof(t->peer), "%s:%u", t->ip, (unsigned)ntohs(cli.sin_port));

        if (thread_pool_submit(srv->pool, connection_task, t) != THREAD_POOL_OK) {
            reject_busy(fd, t->peer);
            free(t);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

//리스닝 소켓 준비
int server_init(server_t *srv, const server_config_t *cfg)
{
    struct sockaddr_in addr;
    struct stat st;
    int opt = 1;

    memset(srv, 0, sizeof(*srv));
    srv->listen_fd    = -1;
    srv->wake_pipe[0] = -1;
    srv->wake_pipe[1] = -1;
    srv->cfg          = *cfg;

    /* 정적 파일 루트를 절대 경로로 정규화해 두면, 요청마다 realpath() 결과와
     * 단순 문자열 접두사 비교만으로 "루트 밖인지" 판단할 수 있다. */
    if (realpath(cfg->doc_root, srv->doc_root) == NULL) {
        fprintf(stderr, "doc_root '%s': %s\n", cfg->doc_root, strerror(errno));
        goto fail;
    }
    if (stat(srv->doc_root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "doc_root '%s' is not a directory\n", srv->doc_root);
        goto fail;
    }

    if (proxy_table_init(&srv->proxy, cfg) < 0) {
        fprintf(stderr, "failed to initialize proxy routes\n");
        goto fail;
    }
    srv->proxy_ready = 1;

    /* self-pipe: 시그널 핸들러가 여기에 쓰면 poll 중인 스레드들이 깨어난다 */
    if (pipe(srv->wake_pipe) < 0) {
        perror("pipe");
        goto fail;
    }
    for (int i = 0; i < 2; i++) {
        set_nonblocking(srv->wake_pipe[i], 1);
        set_cloexec(srv->wake_pipe[i]);
    }
    atomic_store(&g_stopping, 0);
    g_wake_write_fd = srv->wake_pipe[1];
    if (install_signal_handlers() < 0) {
        perror("sigaction");
        goto fail;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        goto fail;
    }
    srv->listen_fd = fd;
    set_cloexec(fd);

    /* 서버 재시작 시 TIME_WAIT 상태 포트에 즉시 bind 가능하도록 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        goto fail;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(cfg->port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto fail;
    }
    if (listen(fd, SERVER_BACKLOG) < 0) {
        perror("listen");
        goto fail;
    }
    /* poll 이 "읽기 가능"이라 했는데 그 사이 연결이 사라지면 accept 가 막힐 수 있다 -> non-blocking */
    if (set_nonblocking(fd, 1) < 0) {
        perror("fcntl(O_NONBLOCK)");
        goto fail;
    }

    srv->pool = thread_pool_create(cfg->threads, cfg->queue_size);
    if (srv->pool == NULL) {
        fprintf(stderr, "failed to create thread pool\n");
        goto fail;
    }
    return 0;

fail:
    server_destroy(srv);
    return -1;
}

//accept 루프
int server_run(server_t *srv)
{
    if (srv == NULL || srv->listen_fd < 0 || srv->pool == NULL)
        return -1;

    config_print(&srv->cfg);
    printf("[INFO] listening on 0.0.0.0:%u, doc_root=%s, %zu workers (Ctrl+C to stop)\n",
           (unsigned)srv->cfg.port, srv->doc_root, srv->cfg.threads);

    struct pollfd p[2] = {
        { .fd = srv->listen_fd,    .events = POLLIN, .revents = 0 },
        { .fd = srv->wake_pipe[0], .events = POLLIN, .revents = 0 },
    };

    while (!server_is_stopping()) {
        int r = poll(p, 2, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;                       /* 시그널 -> 루프 조건에서 종료 여부 확인 */
            perror("poll");
            break;
        }
        if (p[1].revents & POLLIN)
            break;
        if (p[0].revents & POLLIN)
            accept_connections(srv);
    }

    /* Graceful Shutdown
     *  1) 리스닝 소켓을 닫아 새 연결을 받지 않는다 (커널 backlog 에 있던 연결은 RST)
     *  2) 스레드 풀 종료: 큐에 남은 연결과 처리 중인 요청을 끝까지 마친 뒤 워커 join
     *     (keep-alive 대기 중인 워커는 wake_pipe 로 즉시 깨어나 연결을 닫는다) */
    printf("[INFO] shutdown requested: stop accepting, finishing in-flight requests...\n");
    fflush(stdout);
    close(srv->listen_fd);
    srv->listen_fd = -1;

    thread_pool_destroy(srv->pool);
    srv->pool = NULL;

    printf("[INFO] all workers finished, shutdown complete\n");
    fflush(stdout);
    return 0;
}

//리스닝 소켓을 닫고 -1로 표시합니다. (파이프, 스레드 풀, 라우팅 테이블까지 정리)
void server_destroy(server_t *srv)
{
    if (srv == NULL)
        return;

    if (srv->pool != NULL) {                    /* server_run 을 거치지 않은 경우 */
        thread_pool_destroy(srv->pool);
        srv->pool = NULL;
    }
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    if (g_wake_write_fd == srv->wake_pipe[1])
        g_wake_write_fd = -1;
    for (int i = 0; i < 2; i++) {
        if (srv->wake_pipe[i] >= 0) {
            close(srv->wake_pipe[i]);
            srv->wake_pipe[i] = -1;
        }
    }
    if (srv->proxy_ready) {
        proxy_table_destroy(&srv->proxy);
        srv->proxy_ready = 0;
    }
}
