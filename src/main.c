#include "cgi.h"
#include "config.h"
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* PATH_MAX */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define FD_LIMIT_WANT   10240 /* macOS 에서 setrlimit 으로 올릴 수 있는 기본 상한(OPEN_MAX) */
#define FD_RESERVED     32    /* stdio, listen, 종료 파이프, 응답 중 여는 파일 등 */
#define MAX_CONNS_LIMIT 10000

/* ------------------------------------------------------------------ */
/* 시그널 → 파이프 (self-pipe trick)                                    */
/* ------------------------------------------------------------------ */

/* 시그널 핸들러 안에서는 할 수 있는 일이 아주 적다(async-signal-safe 함수만).
 * printf, malloc 은 전부 금지. 그래서 "파이프에 1바이트 쓰기"만 한다.
 * 이벤트 루프는 이 파이프도 poll 목록에 넣어 두므로, 연결 이벤트와 똑같이
 * "종료 이벤트"로 받아서 처리한다. 시그널이 일반 이벤트로 바뀌는 셈. */
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
    /* CGI 자식에게 새지 않게 */
    fcntl(stop_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(stop_pipe[1], F_SETFD, FD_CLOEXEC);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTART 없음: 잠들어 있던 poll 이 EINTR 로 깨어난다 */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return -1;
    }

    /* 클라이언트가 먼저 끊은 소켓에 send 하면 SIGPIPE 로 서버가 "통째로" 죽는다.
     * 무시하면 send 가 -1(EPIPE) 을 반환하므로 그 연결만 정리할 수 있다. */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* 연결 하나 = fd 하나. 프로세스가 열 수 있는 fd 수(macOS 터미널 기본 256)가 곧
 * 동시 연결 수의 천장이다. C10K 를 하려면 이 한도부터 올려야 한다. */
static long raise_fd_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) //지금 한도 읽기
        return 256;
    rlim_t want = FD_LIMIT_WANT; //목표: 10240
    if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max)
        want = rl.rlim_max; //hard보다 높게는 못 올림 
    if (rl.rlim_cur < want) { //지금이 목표보다 낮을 때만 
        struct rlimit nl = { .rlim_cur = want, .rlim_max = rl.rlim_max };
        if (setrlimit(RLIMIT_NOFILE, &nl) == 0) //soft만 올림(hard는 그대로)
            rl = nl;
    }
    return (long)rl.rlim_cur; //최종 soft 값을 돌려줌 
}

/* ------------------------------------------------------------------ */

static long parse_long(const char *s, long lo, long hi)
{
    char *end;
    //문자열 -> 10진수
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi)
        return -1;
    return v;
}

int main(int argc, char **argv)
{
    const char *prog = argv[0];
    const char *conf_path = NULL;
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) { /* -c <설정 파일> 은 맨 앞에만 */
        conf_path = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s [-c config] <port> [docroot=./www] [max_conns=auto]\n", prog);
        return 1;
    }
    long port = parse_long(argv[1], 1, 65535); //포트번호 검사
    if (port < 0) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }

    long fd_limit = raise_fd_limit();
    long max_conns = fd_limit - FD_RESERVED;
    if (max_conns > MAX_CONNS_LIMIT)
        max_conns = MAX_CONNS_LIMIT;
    if (argc == 4) {
        long want = parse_long(argv[3], 1, max_conns);
        if (want < 0) {
            fprintf(stderr, "invalid max_conns (1..%ld with fd limit %ld): %s\n", max_conns,
                    fd_limit, argv[3]);
            return 1;
        }
        max_conns = want;
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

    /* CGI 디렉토리, 프록시 경로 (cgi_dir 기본값 "./cgi-bin" 은 현재 디렉토리 기준) */
    static struct server_config cfg;
    config_defaults(&cfg);
    if (conf_path && config_load(&cfg, conf_path) < 0)
        return 1;
    cfg.port = (int)port;
    cfg.docroot = docroot;

    /* 이벤트 루프 스레드 수: 설정에 없으면 CPU 코어 수(최대 8) */
    int workers = cfg.workers;
    if (workers <= 0) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        workers = cpus < 1 ? 1 : cpus > 8 ? 8 : (int)cpus;
    }
    /* 루프마다 깨우기용 파이프 fd 2개를 더 쓴다 */
    if (max_conns > fd_limit - FD_RESERVED - 2L * workers)
        max_conns = fd_limit - FD_RESERVED - 2L * workers;
    if (max_conns < 1) {
        fprintf(stderr, "fd limit %ld is too low for %d workers\n", fd_limit, workers);
        return 1;
    }

    /* 로그를 파일로 리다이렉트해도 한 줄씩 바로 출력되게 한다. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (setup_signals() < 0 || cgi_reaper_init() < 0)
        return 1;

    int listen_fd = server_listen((int)port);
    if (listen_fd < 0)
        return 1;
    printf("listening on http://localhost:%ld (docroot: %s, %d event loop%s, max %ld connections, fd limit %ld)\n",
           port, docroot, workers, workers == 1 ? "" : "s", max_conns, fd_limit);
    if (cfg.cgi_dir[0])
        printf("  cgi:   /cgi-bin/* -> %s (timeout %d ms)\n", cfg.cgi_dir, cfg.cgi_timeout_ms);
    for (int i = 0; i < cfg.nroutes; i++) {
        printf("  proxy: %s* ->", cfg.routes[i].prefix);
        for (int j = 0; j < cfg.routes[i].nbackends; j++)
            printf(" %s", cfg.routes[i].backends[j].name);
        printf(" (round-robin, connect %d ms, response %d ms)\n", cfg.proxy_connect_timeout_ms,
               cfg.proxy_timeout_ms);
    }

    /* 여기서 루프가 돈다. 종료 신호를 받고 열린 연결이 다 끝나면 돌아온다. */
    struct server_stats s = { 0 };
    int rc = server_run(listen_fd, stop_pipe[0], &cfg, (int)max_conns, workers, &s);

    printf("[loop] bye (connections: %lu, requests: %lu, rejected: %lu)\n", s.connections,
           s.requests, s.rejected);
    close(stop_pipe[0]);
    close(stop_pipe[1]);
    return rc == 0 ? 0 : 1;
}
