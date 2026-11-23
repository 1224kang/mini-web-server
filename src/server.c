#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* 끊긴 소켓에 write 할 때 SIGPIPE로 프로세스가 죽지 않도록 한다.
 * Linux: send()의 MSG_NOSIGNAL 플래그 / macOS: 소켓 옵션 SO_NOSIGPIPE.
 * (프로세스 전역 SIGPIPE 무시는 Phase 5에서 추가) */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
// Linux: send()할 때 SIGPIPE 보내지 마라
#else
#define SEND_FLAGS 0
#endif
//이렇게 하면 두 OS 모두 send()가 시그널 대신 -1과 errno = EPIPE를 반환합니다.

//고정 응답 본문입니다.
static const char RESPONSE_BODY[] =
    "Hello from mini-web-server (Phase 1)\n";

/* ------------------------------------------------------------------ */
/* I/O helpers                                                         */
/* ------------------------------------------------------------------ */

/* Partial write를 고려해 len 바이트를 모두 보낸다. 성공 0, 실패 -1. */
static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n = send(fd, p, len, SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 헤더 끝(\r\n\r\n)을 만나거나, 버퍼가 차거나, 상대가 연결을 닫을 때까지 읽는다.
 * buf는 항상 NUL 종료된다. 읽은 바이트 수를 반환, 오류 시 -1. */
static ssize_t recv_request(int fd, char *buf, size_t cap)
{
    size_t total = 0;

    buf[0] = '\0';
    while (total < cap - 1) {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)             /* peer closed */
            break;

        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL)
            break;
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

/* ------------------------------------------------------------------ */
/* Connection handling                                                 */
/* ------------------------------------------------------------------ */

static void log_request(const char *peer, const char *req, size_t len)
{
    printf("----- request from %s (%zu bytes) -----\n", peer, len);
    fwrite(req, 1, len, stdout);
    if (len > 0 && req[len - 1] != '\n')
        putchar('\n');
    printf("----- end of request -----\n");
    fflush(stdout);
}

static int send_hardcoded_response(int fd)
{
    char header[256];
    size_t body_len = sizeof(RESPONSE_BODY) - 1;

    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "Server: mini-web-server/0.1\r\n"
                        "\r\n",
                        body_len);
    if (hlen < 0 || (size_t)hlen >= sizeof(header))
        return -1;

    if (send_all(fd, header, (size_t)hlen) < 0)
        return -1;
    return send_all(fd, RESPONSE_BODY, body_len);
}

static void handle_client(int client_fd, const char *peer)
{
    char buf[SERVER_RECV_BUF_SIZE];

#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    ssize_t n = recv_request(client_fd, buf, sizeof(buf));
    if (n < 0) {
        fprintf(stderr, "[WARN] recv from %s failed: %s\n", peer, strerror(errno));
        return;
    }
    if (n == 0) {
        printf("[INFO] %s closed connection without sending data\n", peer);
        return;
    }

    log_request(peer, buf, (size_t)n);

    if (send_hardcoded_response(client_fd) < 0)
        fprintf(stderr, "[WARN] send to %s failed: %s\n", peer, strerror(errno));
    else
        printf("[INFO] 200 OK -> %s\n", peer);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int server_init(server_t *srv, uint16_t port)
{
    struct sockaddr_in addr;
    int opt = 1;

    srv->listen_fd = -1;
    srv->port      = port;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 서버 재시작 시 TIME_WAIT 상태 포트에 즉시 bind 가능하도록 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SERVER_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    srv->listen_fd = fd;
    return 0;
}

int server_run(server_t *srv)
{
    if (srv == NULL || srv->listen_fd < 0)
        return -1;

    printf("[INFO] listening on 0.0.0.0:%u\n", (unsigned)srv->port);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);

        int client_fd = accept(srv->listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            /* ECONNABORTED, EMFILE 등은 일시적 오류일 수 있으므로 계속 진행 */
            fprintf(stderr, "[WARN] accept failed: %s\n", strerror(errno));
            continue;
        }

        char ip[INET_ADDRSTRLEN] = "unknown";
        char peer[INET_ADDRSTRLEN + 8];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        snprintf(peer, sizeof(peer), "%s:%u", ip, (unsigned)ntohs(cli.sin_port));

        printf("[INFO] accepted connection from %s\n", peer);
        handle_client(client_fd, peer);
        close(client_fd);
        printf("[INFO] connection closed: %s\n", peer);
        fflush(stdout);
    }

    return 0;
}

void server_destroy(server_t *srv)
{
    if (srv == NULL)
        return;
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
}
