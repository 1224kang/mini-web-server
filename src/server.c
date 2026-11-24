#include "server.h"
#include "conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* INT_MAX, LLONG_MAX */
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * 단일 스레드 이벤트 루프.
 *
 *   for (;;) {
 *       poll([listen, stop, 연결1, 연결2, ...], 가장 가까운 마감까지)
 *       준비된 연결마다 conn_handle()  ← 할 수 있는 만큼만 하고 즉시 돌아온다
 *       마감 지난 연결마다 conn_timeout()
 *       새 연결 accept, 끝난 연결 정리
 *   }
 *
 * 스레드가 하나뿐이라 공유 자원 동기화(mutex)가 필요 없다.
 * 대신 이 루프 안의 어떤 코드도 "기다리면" 안 된다. 하나가 멈추면 전부 멈춘다.
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
/* 이벤트 루프                                                          */
/* ------------------------------------------------------------------ */

#define ACCEPT_BATCH 64          /* 한 바퀴에 accept 할 최대 연결 수 (기존 연결도 돌봐야 하므로) */
#define ACCEPT_PAUSE_MS 100      /* fd 고갈(EMFILE) 때 accept 를 쉬는 시간 */

void server_reject_busy(int client_fd)
{
    /* 루프 안에서 호출되므로 막히면 안 된다. 소켓이 이미 non-blocking 이라 send 는
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

struct loop {
    const char *docroot; //정적 파일을 찾을 폴더 경로 
    int listen_fd; //listen 소켓의 fd 
    int max_conns; //동시에 열어 둘 수 있는 최대 연결 수 
    struct conn **conns; /* 살아 있는 연결들 (순서 없음) */
    int nconns; //conns 배열에서 실제로 쓰고 있는 칸 수 
    unsigned long next_id; //지금까지 accept한 연결 수 
    unsigned long rejected; //503으로 거절한 연결 수 
    long long accept_paused_until; //이 시각(ms)까지 accept를 멈춤. 
};

/* 대기 중인 연결을 한 번에 여러 개 받는다. listen 소켓이 non-blocking 이라
 * 더 없으면 EAGAIN 으로 바로 돌아온다. */
static void accept_new(struct loop *L, long long now)
{
    for (int k = 0; k < ACCEPT_BATCH; k++) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int fd = accept(L->listen_fd, (struct sockaddr *)&client, &clen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return; /* 다 받았다 */
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            if (errno == EMFILE || errno == ENFILE) {
                /* fd 고갈. listen 소켓은 계속 "읽을 수 있음"이라 그냥 두면 poll 이
                 * 즉시 돌아오기를 반복하며 CPU 100% 로 헛돈다 → 잠깐 감시에서 뺀다. */
                L->accept_paused_until = now + ACCEPT_PAUSE_MS;
            }
            return;
        }
        /* 이벤트 루프의 소켓은 전부 non-blocking 이어야 한다. 하나라도 블로킹이면
         * 그 recv/send 에서 루프 전체(= 모든 연결)가 멈춘다.
         * (macOS 는 listen 소켓의 O_NONBLOCK 을 물려주지만 Linux 는 아니므로 직접 켠다) */
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));

        if (L->nconns >= L->max_conns) {
            printf("[loop] busy (%d connections): rejecting %s:%d with 503\n", L->nconns, ip,
                   ntohs(client.sin_port));
            L->rejected++;
            server_reject_busy(fd);
            continue;
        }
        struct conn *c = conn_new(fd, ++L->next_id, L->docroot, now);
        if (!c) {
            close(fd);
            continue;
        }
        L->conns[L->nconns++] = c;
        printf("[loop] accepted %s:%d as c%lu (open: %d)\n", ip, ntohs(client.sin_port),
               L->next_id, L->nconns);
    }
}

/* 끝난 연결을 정리한다. 배열 중간을 지울 땐 맨 뒤 원소로 메운다(O(1), 순서는 상관없음). */
static void sweep(struct loop *L)
{
    for (int i = 0; i < L->nconns;) {
        if (conn_done(L->conns[i])) {
            conn_free(L->conns[i]);
            L->conns[i] = L->conns[--L->nconns];
        } else {
            i++;
        }
    }
}

int server_run(int listen_fd, int stop_fd, const char *docroot, int max_conns,
               struct server_stats *stats)
{
    struct loop L = {
        .docroot = docroot,
        .listen_fd = listen_fd,
        .max_conns = max_conns,
    };
    /* poll 배열: [listen][stop][연결 0][연결 1]... */
    L.conns = calloc((size_t)max_conns, sizeof(*L.conns));
    struct pollfd *pfds = calloc((size_t)max_conns + 2, sizeof(*pfds));
    if (!L.conns || !pfds) {
        perror("calloc");
        free(L.conns);
        free(pfds);
        return -1;
    }

    /* 이벤트가 왔다고 accept 했는데 그 사이 클라이언트가 RST 로 사라지면
     * 블로킹 accept 는 다음 손님이 올 때까지 루프를 멈춘다 → non-blocking. */
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL) | O_NONBLOCK);

    int stopping = 0;
    for (;;) {
        if (stopping && L.nconns == 0)
            break;
        long long now = now_ms();

        /* ---- 1) 이번 바퀴에 감시할 fd 목록 만들기 ---- */
        int np = 0, listen_idx = -1, stop_idx = -1;
        long long next_deadline = LLONG_MAX;
        if (!stopping) {
            if (now >= L.accept_paused_until) {
                listen_idx = np; //입구 벨 
                pfds[np++] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
            } else {
                next_deadline = L.accept_paused_until;
            }
            /* 종료 파이프는 한 번 울리면 계속 "읽을 수 있음"이다. 종료를 시작한 뒤에도
             * 넣어 두면 poll 이 매번 즉시 돌아와 헛돈다 → stopping 이후엔 뺀다. */
            stop_idx = np; //마감 벨. 
            pfds[np++] = (struct pollfd){ .fd = stop_fd, .events = POLLIN };
        }
        int base = np; // 클라이언트 소켓이 시작하는 칸 번호 
        int n = L.nconns; /* 이번 바퀴에서 다룰 연결 수 (도중에 accept 된 건 다음 바퀴에) */
        for (int i = 0; i < n; i++) {
            pfds[np++] = (struct pollfd){ .fd = conn_fd(L.conns[i]), .events = conn_events(L.conns[i]) };
            long long d = conn_deadline(L.conns[i]);
            if (d < next_deadline)
                next_deadline = d;
        }

        /* ---- 2) 무언가 일어나거나, 가장 가까운 마감이 될 때까지 잠든다 ----
         * 연결이 1만 개여도 잠든 동안 CPU 는 0%. 타이머를 연결마다 따로 걸지 않고
         * "가장 빠른 마감"까지만 자면 모든 타임아웃을 한 번에 처리할 수 있다. */
        int timeout = -1;
        // 가장 빠른 마감까지만 잠들어있기 
        if (next_deadline != LLONG_MAX) {
            long long left = next_deadline - now;
            timeout = left <= 0 ? 0 : left > INT_MAX ? INT_MAX : (int)left;
        }
        if (poll(pfds, (nfds_t)np, timeout) < 0) {
            if (errno == EINTR)
                continue; /* 시그널로 깨어남 → 다시 돌면 종료 파이프가 보인다 */
            perror("poll");
            break;
        }
        now = now_ms();

        /* ---- 3) 준비된 연결을 하나씩 "조금씩" 진행시킨다 ---- */
        for (int i = 0; i < n; i++) {
            struct conn *c = L.conns[i];
            if (pfds[base + i].revents) //손님 i 벨이 울렸을 때 
                conn_handle(c, pfds[base + i].revents, now);
            if (!conn_done(c) && conn_deadline(c) <= now)
                conn_timeout(c, now);
        }

        /* ---- 4) 종료 신호 ---- */
        if (stop_idx >= 0 && pfds[stop_idx].revents) {
            stopping = 1;
            close(listen_fd); /* 새 연결은 이제 커널이 거절한다 */
            listen_idx = -1;
            printf("\n[loop] shutting down: no new connections, finishing %d open connection(s)...\n",
                   L.nconns);
            for (int i = 0; i < L.nconns; i++)
                conn_shutdown(L.conns[i], now);
        }

        /* ---- 5) 새 연결 받기 ---- */
        if (listen_idx >= 0 && (pfds[listen_idx].revents & POLLIN))
            accept_new(&L, now);

        /* ---- 6) 끝난 연결 정리 ---- */
        sweep(&L);
    }

    for (int i = 0; i < L.nconns; i++) /* poll 오류로 나온 경우 */
        conn_free(L.conns[i]);
    free(L.conns);
    free(pfds);

    stats->connections = L.next_id;
    stats->requests = conn_total_requests();
    stats->rejected = L.rejected;
    return 0;
}
