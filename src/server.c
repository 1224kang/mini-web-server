#include "server.h"
#include "cgi.h"
#include "conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* INT_MAX, LLONG_MAX */
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * 멀티 이벤트 루프 (acceptor 1개 + 이벤트 루프 스레드 N개).
 *
 *   [메인 스레드 = acceptor]                    [루프 스레드 x N]  (각자 독립된 poll 루프)
 *   poll(listen, stop)                          for (;;) {
 *     accept → 연결 수 한도 검사(넘치면 503)        poll([wake, stop, 자식회수, 자기 연결들...])
 *            → 연결이 가장 적은 루프를 골라   ──▶    wake: 받은 연결을 꺼내 conn_new
 *              그 루프의 inbox 에 넣고 wake          준비된 연결마다 conn_handle / conn_timeout
 *     stop  → listen 을 닫고 루프들을 join          끝난 연결 정리
 *                                               }
 *
 * - 연결은 처음 받은 루프에서만 처리된다(스레드 사이를 옮겨 다니지 않음).
 *   그래서 struct conn 과 conn.c 의 상태 머신은 lock 없이 단일 스레드 때와 똑같이 돈다.
 * - 스레드끼리 공유하는 것은 적다: 연결 수(atomic), Round-Robin 카운터(atomic),
 *   요청 통계(atomic), CGI 고아 목록(mutex), inbox(mutex).
 * - 루프 하나 안의 코드는 여전히 "기다리면" 안 된다. 하나가 멈추면 그 루프의 연결이 전부 멈춘다.
 *   대신 다른 루프는 계속 돈다.
 */

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
    /* FD_CLOEXEC: CGI 를 exec 할 때 자식에게 listen 소켓이 새지 않게 */
    fcntl(fd, F_SETFD, FD_CLOEXEC);

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
/* 공용                                                                 */
/* ------------------------------------------------------------------ */

#define ACCEPT_BATCH 64          /* 한 바퀴에 accept 할 최대 연결 수 */
#define ACCEPT_PAUSE_MS 100      /* fd 고갈(EMFILE) 때 accept 를 쉬는 시간 */

void server_reject_busy(int client_fd)
{
    /* acceptor 를 막으면 안 된다. 소켓이 이미 non-blocking 이라 send 는
     * 들어가는 만큼만 넣고 바로 돌아온다. 갓 만든 연결의 송신 버퍼는 비어 있어서
     * 이 정도 크기는 한 번에 들어간다. */
    static const char resp[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 24\r\n"
        "Connection: close\r\n"
        "Retry-After: 1\r\n"
        "\r\n"
        "503 Service Unavailable\n";
    (void)!send(client_fd, resp, sizeof(resp) - 1, 0);
    shutdown(client_fd, SHUT_WR);
    close(client_fd);
}

/* acceptor 가 루프에게 넘기는 "받은 연결" 하나 */
struct handoff {
    int fd;
    unsigned long id;
    char ip[INET_ADDRSTRLEN];
    int port;
    struct handoff *next;
};

struct server;

struct loop {
    int index;                /* 로그용 번호: [loop1], [loop2], ... */
    struct server *srv;
    pthread_t thread;

    /* acceptor → 이 루프. inbox 는 lock 으로 보호하고, wake 파이프에 1바이트 써서 깨운다. */
    pthread_mutex_t lock;
    struct handoff *inbox_head, *inbox_tail;
    int wake[2];
    atomic_int open;          /* 이 루프가 맡은 연결 수 (acceptor 가 분배할 때 읽는다) */

    /* 여기부터는 이 루프 스레드만 만진다 */
    struct conn **conns;      /* 살아 있는 연결들 (순서 없음) */
    int nconns;
    struct pollfd *pfds;
    int *first, *count;       /* pfds 안에서 연결 i 의 시작 칸과 칸 수 */
};

struct server {
    struct server_config *cfg;
    int listen_fd;
    int stop_fd;
    int max_conns;            /* 모든 루프를 합친 동시 연결 한도 */
    atomic_int open_total;    /* 모든 루프를 합친 연결 수 */
    int nloops;
    struct loop *loops;

    /* acceptor 만 만진다 */
    unsigned long next_id;
    unsigned long rejected;
    unsigned next_rr;
    long long accept_paused_until;
};

static void drain_pipe(int fd)
{
    char buf[256];
    while (read(fd, buf, sizeof(buf)) > 0)
        ;
}

static int make_pipe(int p[2])
{
    if (pipe(p) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        fcntl(p[i], F_SETFD, FD_CLOEXEC);
        fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 이벤트 루프 스레드                                                     */
/* ------------------------------------------------------------------ */

/* inbox 에 쌓인 연결을 한꺼번에 꺼내 이 루프의 연결로 만든다. */
static void adopt_new(struct loop *L, long long now, int stopping)
{
    pthread_mutex_lock(&L->lock);
    struct handoff *h = L->inbox_head;
    L->inbox_head = L->inbox_tail = NULL;
    pthread_mutex_unlock(&L->lock);

    while (h) {
        struct handoff *next = h->next;
        struct conn *c = NULL;
        if (L->nconns < L->srv->max_conns)
            c = conn_new(h->fd, h->id, L->srv->cfg, h->ip, h->port, now);
        if (!c) {
            close(h->fd);
            atomic_fetch_sub(&L->open, 1);
            atomic_fetch_sub(&L->srv->open_total, 1);
        } else {
            L->conns[L->nconns++] = c;
            printf("[loop%d] accepted %s:%d as c%lu (open: %d)\n", L->index, h->ip, h->port, h->id,
                   L->nconns);
            if (stopping) /* 종료 직전에 넘겨받은 연결: 요청이 없으면 바로 닫힌다 */
                conn_shutdown(c, now);
        }
        free(h);
        h = next;
    }
}

static int inbox_empty(struct loop *L)
{
    pthread_mutex_lock(&L->lock);
    int empty = L->inbox_head == NULL;
    pthread_mutex_unlock(&L->lock);
    return empty;
}

/* 끝난 연결을 정리한다. 배열 중간을 지울 땐 맨 뒤 원소로 메운다(O(1), 순서는 상관없음). */
static void sweep(struct loop *L)
{
    for (int i = 0; i < L->nconns;) {
        if (conn_done(L->conns[i])) {
            conn_free(L->conns[i]);
            L->conns[i] = L->conns[--L->nconns];
            atomic_fetch_sub(&L->open, 1);
            atomic_fetch_sub(&L->srv->open_total, 1);
        } else {
            i++;
        }
    }
}

static void *loop_main(void *arg)
{
    struct loop *L = arg;
    struct server *S = L->srv;
    int reap_fd = cgi_reaper_fd();
    int stopping = 0;

    for (;;) {
        if (stopping && L->nconns == 0 && inbox_empty(L))
            break;
        long long now = now_ms();

        /* ---- 1) 이번 바퀴에 감시할 fd 목록 만들기 ----
         * poll 배열: [wake][stop][reap][연결 0 의 fd 1~3개][연결 1 의 fd 1~3개]...
         * 연결마다 fd 수가 달라서(정적 파일 1개, CGI 3개, 프록시 2개) 시작 칸과 칸 수를 따로 적는다. */
        int np = 0, stop_idx = -1;
        long long next_deadline = LLONG_MAX;
        int wake_idx = np;
        L->pfds[np++] = (struct pollfd){ .fd = L->wake[0], .events = POLLIN };
        if (!stopping) {
            /* 종료 파이프는 한 번 울리면 계속 "읽을 수 있음"이다(모든 스레드에게 알리려고 비우지 않음).
             * 종료를 시작한 뒤에도 넣어 두면 poll 이 매번 즉시 돌아와 헛돈다 → stopping 이후엔 뺀다. */
            stop_idx = np;
            L->pfds[np++] = (struct pollfd){ .fd = S->stop_fd, .events = POLLIN };
        }
        /* SIGCHLD → 파이프. 어느 루프가 깨어나 거둬도 된다(cgi_reap 은 고아 목록 전체를 훑는다). */
        int reap_idx = np;
        L->pfds[np++] = (struct pollfd){ .fd = reap_fd, .events = POLLIN };

        int n = L->nconns; /* 이번 바퀴에서 다룰 연결 수 (도중에 받은 건 다음 바퀴에) */
        for (int i = 0; i < n; i++) {
            L->first[i] = np;
            L->count[i] = conn_pollfds(L->conns[i], &L->pfds[np]);
            np += L->count[i];
            long long d = conn_deadline(L->conns[i]);
            if (d < next_deadline)
                next_deadline = d;
        }

        /* ---- 2) 무언가 일어나거나, 가장 가까운 마감이 될 때까지 잠든다 ---- */
        int timeout = -1;
        if (next_deadline != LLONG_MAX) {
            long long left = next_deadline - now;
            timeout = left <= 0 ? 0 : left > INT_MAX ? INT_MAX : (int)left;
        }
        if (poll(L->pfds, (nfds_t)np, timeout) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        now = now_ms();

        /* ---- 3) 준비된 연결을 하나씩 "조금씩" 진행시킨다 ---- */
        for (int i = 0; i < n; i++) {
            struct conn *c = L->conns[i];
            int ready = 0;
            for (int k = 0; k < L->count[i]; k++)
                ready |= L->pfds[L->first[i] + k].revents != 0;
            if (ready)
                conn_handle(c, &L->pfds[L->first[i]], L->count[i], now);
            if (!conn_done(c) && conn_deadline(c) <= now)
                conn_timeout(c, now);
        }

        /* ---- 3.5) 끝난 CGI 자식 거두기 (좀비 방지) ---- */
        if (L->pfds[reap_idx].revents)
            cgi_reap();

        /* ---- 4) 종료 신호: 놀던 연결은 닫고, 처리 중인 연결은 응답을 마친 뒤 닫힌다 ---- */
        if (stop_idx >= 0 && L->pfds[stop_idx].revents) {
            stopping = 1;
            for (int i = 0; i < L->nconns; i++)
                conn_shutdown(L->conns[i], now);
        }

        /* ---- 5) acceptor 가 넘긴 새 연결 받기 ---- */
        if (L->pfds[wake_idx].revents) {
            drain_pipe(L->wake[0]);
            adopt_new(L, now, stopping);
        }

        /* ---- 6) 끝난 연결 정리 ---- */
        sweep(L);
    }

    for (int i = 0; i < L->nconns; i++) { /* poll 오류로 나온 경우 */
        conn_free(L->conns[i]);
        atomic_fetch_sub(&S->open_total, 1);
    }
    L->nconns = 0;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* acceptor (메인 스레드)                                                */
/* ------------------------------------------------------------------ */

/* 연결이 가장 적은 루프. 같으면 돌아가며 고른다(처음 몇 개가 한 루프에 몰리지 않게). */
static struct loop *pick_loop(struct server *S)
{
    struct loop *best = NULL;
    int best_open = INT_MAX;
    for (int k = 0; k < S->nloops; k++) {
        struct loop *L = &S->loops[(S->next_rr + (unsigned)k) % (unsigned)S->nloops];
        int open = atomic_load(&L->open);
        if (open < best_open) {
            best = L;
            best_open = open;
        }
    }
    S->next_rr++;
    return best;
}

static void hand_off(struct server *S, int fd, const char *ip, int port)
{
    struct handoff *h = malloc(sizeof(*h));
    if (!h) {
        close(fd);
        return;
    }
    h->fd = fd;
    h->id = ++S->next_id;
    snprintf(h->ip, sizeof(h->ip), "%s", ip);
    h->port = port;
    h->next = NULL;

    struct loop *L = pick_loop(S);
    atomic_fetch_add(&S->open_total, 1);
    atomic_fetch_add(&L->open, 1);

    pthread_mutex_lock(&L->lock);
    if (L->inbox_tail)
        L->inbox_tail->next = h;
    else
        L->inbox_head = h;
    L->inbox_tail = h;
    pthread_mutex_unlock(&L->lock);
    (void)!write(L->wake[1], "x", 1); /* 꽉 차 있으면 이미 깨울 예정이므로 무시 */
}

/* 대기 중인 연결을 한 번에 여러 개 받는다. listen 소켓이 non-blocking 이라
 * 더 없으면 EAGAIN 으로 바로 돌아온다. */
static void accept_new(struct server *S, long long now)
{
    for (int k = 0; k < ACCEPT_BATCH; k++) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        /* accept 와 FD_CLOEXEC 사이에 루프 스레드가 CGI 를 fork 하면 이 소켓이 자식에게 샌다
         * (자식이 쥐고 있으면 우리가 close 해도 클라이언트는 연결이 끝난 걸 모른다). */
        cgi_fd_guard_enter();
        int fd = accept(S->listen_fd, (struct sockaddr *)&client, &clen);
        int err = errno;
        if (fd >= 0)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        cgi_fd_guard_leave();
        if (fd < 0) {
            if (err == EAGAIN || err == EWOULDBLOCK)
                return; /* 다 받았다 */
            if (err == EINTR || err == ECONNABORTED)
                continue;
            fprintf(stderr, "accept: %s\n", strerror(err));
            if (err == EMFILE || err == ENFILE) {
                /* fd 고갈. listen 소켓은 계속 "읽을 수 있음"이라 그냥 두면 poll 이
                 * 즉시 돌아오기를 반복하며 CPU 100% 로 헛돈다 → 잠깐 감시에서 뺀다. */
                S->accept_paused_until = now + ACCEPT_PAUSE_MS;
            }
            return;
        }
        /* 이벤트 루프의 소켓은 전부 non-blocking 이어야 한다. 하나라도 블로킹이면
         * 그 recv/send 에서 루프 전체(= 그 루프의 모든 연결)가 멈춘다.
         * (macOS 는 listen 소켓의 O_NONBLOCK 을 물려주지만 Linux 는 아니므로 직접 켠다) */
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        int port = ntohs(client.sin_port);

        int open = atomic_load(&S->open_total);
        if (open >= S->max_conns) {
            printf("[accept] busy (%d connections): rejecting %s:%d with 503\n", open, ip, port);
            S->rejected++;
            server_reject_busy(fd);
            continue;
        }
        hand_off(S, fd, ip, port);
    }
}

static void loop_destroy(struct loop *L)
{
    for (struct handoff *h = L->inbox_head; h;) { /* 끝내 넘겨주지 못한 연결 */
        struct handoff *next = h->next;
        close(h->fd);
        free(h);
        h = next;
    }
    L->inbox_head = L->inbox_tail = NULL;
    for (int i = 0; i < 2; i++)
        if (L->wake[i] >= 0)
            close(L->wake[i]);
    pthread_mutex_destroy(&L->lock);
    free(L->conns);
    free(L->pfds);
    free(L->first);
    free(L->count);
}

static int loop_init(struct loop *L, struct server *S, int index)
{
    memset(L, 0, sizeof(*L));
    L->index = index;
    L->srv = S;
    L->wake[0] = L->wake[1] = -1;
    atomic_init(&L->open, 0);
    pthread_mutex_init(&L->lock, NULL);
    /* 한 루프가 모든 연결을 맡게 될 수도 있으므로 배열은 전체 한도만큼 잡는다 */
    size_t max = (size_t)S->max_conns;
    L->conns = calloc(max, sizeof(*L->conns));
    L->pfds = calloc(max * CONN_MAX_FDS + 3, sizeof(*L->pfds));
    L->first = calloc(max, sizeof(*L->first));
    L->count = calloc(max, sizeof(*L->count));
    if (!L->conns || !L->pfds || !L->first || !L->count || make_pipe(L->wake) < 0) {
        perror("loop init");
        return -1;
    }
    return 0;
}

int server_run(int listen_fd, int stop_fd, struct server_config *cfg, int max_conns, int workers,
               struct server_stats *stats)
{
    struct server S = {
        .cfg = cfg,
        .listen_fd = listen_fd,
        .stop_fd = stop_fd,
        .max_conns = max_conns,
        .nloops = workers,
    };
    atomic_init(&S.open_total, 0);
    S.loops = calloc((size_t)workers, sizeof(*S.loops));
    if (!S.loops) {
        perror("calloc");
        return -1;
    }
    int ninit = 0, rc = 0;
    for (; ninit < workers; ninit++) {
        if (loop_init(&S.loops[ninit], &S, ninit + 1) < 0) {
            loop_destroy(&S.loops[ninit]);
            rc = -1;
            break;
        }
    }

    /* 루프 스레드는 SIGINT/SIGTERM/SIGCHLD 를 막은 채로 만든다 → 시그널은 메인 스레드로만 온다.
     * 핸들러는 파이프에 1바이트 쓰기만 하므로, 루프들은 그 파이프를 poll 해서 알게 된다. */
    int nstarted = 0;
    if (rc == 0) {
        sigset_t block, old;
        sigemptyset(&block);
        sigaddset(&block, SIGINT);
        sigaddset(&block, SIGTERM);
        sigaddset(&block, SIGCHLD);
        pthread_sigmask(SIG_BLOCK, &block, &old);
        for (; nstarted < workers; nstarted++) {
            if (pthread_create(&S.loops[nstarted].thread, NULL, loop_main, &S.loops[nstarted]) != 0) {
                fprintf(stderr, "pthread_create failed after %d loops\n", nstarted);
                break;
            }
        }
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        if (nstarted == 0)
            rc = -1;
        S.nloops = nstarted; /* 일부만 떴으면 뜬 만큼만 쓴다 */
    }

    /* 이벤트가 왔다고 accept 했는데 그 사이 클라이언트가 RST 로 사라지면
     * 블로킹 accept 는 다음 손님이 올 때까지 멈춘다 → non-blocking. */
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL) | O_NONBLOCK);

    while (rc == 0) {
        long long now = now_ms();
        struct pollfd p[2];
        int np = 0, listen_idx = -1;
        int timeout = -1;
        if (now >= S.accept_paused_until) {
            listen_idx = np;
            p[np++] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
        } else {
            long long left = S.accept_paused_until - now;
            timeout = left > INT_MAX ? INT_MAX : (int)left;
        }
        int stop_idx = np;
        p[np++] = (struct pollfd){ .fd = stop_fd, .events = POLLIN };

        if (poll(p, (nfds_t)np, timeout) < 0) {
            if (errno == EINTR)
                continue; /* 시그널로 깨어남 → 다시 돌면 종료 파이프가 보인다 */
            perror("poll");
            break;
        }
        if (p[stop_idx].revents)
            break;
        if (listen_idx >= 0 && (p[listen_idx].revents & POLLIN))
            accept_new(&S, now_ms());
    }

    /* ---- 종료: 새 연결은 받지 않고(커널이 거절), 루프들이 열린 연결을 마칠 때까지 기다린다 ---- */
    close(listen_fd);
    if (nstarted > 0)
        printf("\n[accept] shutting down: no new connections, %d loop(s) finishing %d open connection(s)...\n",
               nstarted, atomic_load(&S.open_total));
    for (int i = 0; i < nstarted; i++)
        pthread_join(S.loops[i].thread, NULL);
    for (int i = 0; i < ninit; i++)
        loop_destroy(&S.loops[i]);
    free(S.loops);
    cgi_reap();

    stats->connections = S.next_id;
    stats->requests = conn_total_requests();
    stats->rejected = S.rejected;
    return rc;
}
