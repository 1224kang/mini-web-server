#include "server.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* PATH_MAX */
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_THREADS  8
#define QUEUE_PER_THREAD 16 /* 큐 크기 = 워커 수 × 이 값. 넘치면 503 */

/* ------------------------------------------------------------------ */
/* 시그널 → 파이프 (self-pipe trick)                                    */
/* ------------------------------------------------------------------ */

/* 시그널 핸들러 안에서는 할 수 있는 일이 아주 적다(async-signal-safe 함수만).
 * printf, malloc, mutex 는 전부 금지. 그래서 "파이프에 1바이트 쓰기"만 한다.
 *
 * 이 파이프의 읽기 쪽(stop_pipe[0])은 아무도 비우지 않는다. 그래서 한 번 쓰이면
 * 영원히 "읽을 수 있음" 상태로 남는다 → poll 하는 모든 스레드(main, 워커 N개)가
 * 동시에, 놓치지 않고 종료를 알아챈다. */
static int stop_pipe[2] = { -1, -1 };
static volatile sig_atomic_t stop_count = 0;

static void on_stop_signal(int sig)
{
    (void)sig;
    int saved = errno; /* 핸들러가 끼어든 코드의 errno 를 망가뜨리지 않기 */
    if (++stop_count >= 2) {
        /* 두 번째 Ctrl+C: 기다리지 않고 바로 끝낸다. (exit 가 아니라 _exit: 안전함) */
        static const char msg[] = "\nforced exit\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(130);
    }
    (void)!write(stop_pipe[1], "x", 1);
    errno = saved;
}

static int setup_signals(void)
{
    if (pipe(stop_pipe) < 0) {
        perror("pipe");
        return -1;
    }
    /* 쓰기 쪽 non-blocking: 핸들러가 절대 멈추면 안 된다. */
    fcntl(stop_pipe[1], F_SETFL, fcntl(stop_pipe[1], F_GETFL) | O_NONBLOCK);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTART 없음: 막혀 있던 시스템 호출이 EINTR 로 깨어난다 */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return -1;
    }

    /* 클라이언트가 먼저 끊은 소켓에 send 하면 SIGPIPE 로 서버가 "통째로" 죽는다.
     * 무시하면 send 가 -1(EPIPE) 을 반환하므로 그 연결만 에러로 처리할 수 있다. */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* ------------------------------------------------------------------ */

static int parse_int(const char *s, long lo, long hi)
{
    char *end;
    //문자열 -> 10진수
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi)
        return -1;
    return (int)v;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s <port> [docroot=./www] [threads=%d]\n", argv[0],
                DEFAULT_THREADS);
        return 1;
    }
    int port = parse_int(argv[1], 1, 65535); //포트번호 검사
    if (port < 0) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }
    int nthreads = argc == 4 ? parse_int(argv[3], 1, 1024) : DEFAULT_THREADS;
    if (nthreads < 0) {
        fprintf(stderr, "invalid thread count (1..1024): %s\n", argv[3]);
        return 1;
    }

    /* 문서 루트를 "절대 경로 + 심볼릭 링크 해제" 형태로 한 번만 정규화해 둔다.
     * 요청마다 realpath(요청 파일) 결과가 이 문자열로 시작하는지 비교해서 탈출을 막는다. */
    static char docroot[PATH_MAX];
    const char *root_arg = argc >= 3 ? argv[2] : "www";
    struct stat st;
    if (!realpath(root_arg, docroot) || stat(docroot, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "docroot is not a directory: %s\n", root_arg);
        return 1;
    }
    if (strcmp(docroot, "/") == 0) {
        fprintf(stderr, "refusing to serve the whole filesystem (docroot=/)\n");
        return 1;
    }

    /* 로그를 파일로 리다이렉트해도 한 줄씩 바로 출력되게 한다. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (setup_signals() < 0)
        return 1;

    int listen_fd = server_listen(port);
    if (listen_fd < 0)
        return 1;
    /* poll 이 "연결 왔다"고 한 뒤 accept 전에 클라이언트가 RST 로 사라지면
     * 블로킹 accept 는 다음 연결이 올 때까지 멈춘다 → non-blocking 으로 둔다. */
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL) | O_NONBLOCK);

    /* 워커 스레드가 SIGINT/SIGTERM 을 받지 않게 한다.
     * 프로세스로 온 시그널은 "막지 않은 아무 스레드"에게 배달되는데, 워커가 받으면
     * 워커의 recv/poll 이 엉뚱하게 EINTR 로 깨진다. 새 스레드는 만든 스레드의 마스크를
     * 물려받으므로, 막은 상태로 풀을 만들고 main 에서만 다시 푼다. */
    sigset_t stop_sigs, old_mask;
    sigemptyset(&stop_sigs);
    sigaddset(&stop_sigs, SIGINT);
    sigaddset(&stop_sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_sigs, &old_mask);

    struct server_ctx ctx = { .docroot = docroot, .stop_fd = stop_pipe[0], .pool = NULL };
    struct thread_pool *pool = pool_create(nthreads, (size_t)nthreads * QUEUE_PER_THREAD,
                                           server_handle_client, &ctx);
    /* pool 은 워커가 첫 연결을 받기 전(= pool_submit 의 mutex 이전)에 써 두므로 안전하다. */
    ctx.pool = pool;
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    if (!pool) {
        fprintf(stderr, "failed to create thread pool\n");
        close(listen_fd);
        return 1;
    }

    printf("listening on http://localhost:%d (docroot: %s, %d workers, queue %d)\n",
           port, docroot, nthreads, nthreads * QUEUE_PER_THREAD);

    /* accept 루프: 이제 main 은 "접수 창구"만 한다. 연결을 받아 큐에 넣고 바로 다음 손님.
     * listen 소켓과 종료 파이프를 같이 poll 해서, 둘 중 먼저 오는 쪽에 반응한다. */
    for (;;) {
        struct pollfd pfd[2] = {
            { .fd = listen_fd, .events = POLLIN },
            { .fd = stop_pipe[0], .events = POLLIN },
        };
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue; /* 시그널로 깨어남 → 다시 poll 하면 파이프가 읽을 수 있음으로 보인다 */
            perror("poll");
            break;
        }
        if (pfd[1].revents)
            break; /* 종료 신호 */
        if (!(pfd[0].revents & POLLIN))
            continue;

        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client, &clen);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            if (errno == EMFILE || errno == ENFILE)
                usleep(100 * 1000); /* fd 고갈: 바로 다시 돌면 CPU 100% 로 헛돈다 */
            continue;
        }
        /* macOS(BSD)에서는 accept 한 소켓이 listen 소켓의 O_NONBLOCK 을 물려받는다(Linux 는 아님).
         * 워커는 블로킹 소켓 + poll 마감 시간 방식으로 짜여 있으므로 꺼 준다. */
        fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) & ~O_NONBLOCK);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));

        if (pool_submit(pool, client_fd) < 0) {
            printf("[main] busy: rejecting %s:%d with 503\n", ip, ntohs(client.sin_port));
            server_reject_busy(client_fd);
            continue;
        }
        printf("[main] accepted %s:%d (queued: %zu)\n", ip, ntohs(client.sin_port),
               pool_pending(pool));
    }

    /* ---- 우아한 종료(graceful shutdown) ----
     * 1) 새 연결 그만 받기: listen 소켓을 닫으면 이후 접속은 커널이 거절(RST)한다.
     * 2) 큐에서 기다리던 연결은 닫고, 요청을 처리하던 워커는 그 응답을 끝까지 보낸 뒤
     *    (Connection: close) 나온다. keep-alive 로 놀던 워커는 종료 파이프를 보고 바로 나온다.
     * 3) 모든 워커를 join 한 뒤에야 공유 자원(통계, docroot 등)을 정리한다. */
    printf("\n[main] shutting down: no new connections, waiting for workers...\n");
    close(listen_fd);
    pool_destroy(pool);

    struct server_stats s;
    server_stats_get(&s);
    printf("[main] bye (connections: %lu, requests: %lu, still active: %lu)\n",
           s.connections, s.requests, s.active);
    close(stop_pipe[0]);
    close(stop_pipe[1]);
    return 0;
}
