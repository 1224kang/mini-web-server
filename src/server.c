#include "server.h"
#include "http.h"
#include "static_handler.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* 에러 응답 후 남은 입력을 버릴 때의 한계 */
#define DRAIN_TIMEOUT_USEC  500000          /* recv 1회당 최대 대기 0.5초 */
#define DRAIN_MAX_BYTES     (1024 * 1024)   /* 최대 1MB 까지만 읽어 버림  */

/* ------------------------------------------------------------------ */
/* Connection handling                                                 */
/* ------------------------------------------------------------------ */

/* 접근 로그: 127.0.0.1:34678 "GET /index.html HTTP/1.1" 200 */
static void log_access(const char *peer, const http_request_t *req, int status)
{
    if (req->method_str[0] != '\0' && req->target[0] != '\0' && req->version_major != 0)
        printf("[INFO] %s \"%s %s HTTP/%d.%d\" %d\n", peer, req->method_str, req->target,
               req->version_major, req->version_minor, status);
    else
        printf("[INFO] %s \"-\" %d\n", peer, status);
    fflush(stdout);
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

/* 파싱이 끝난 요청을 알맞은 핸들러로 보낸다. 반환: 응답한 상태 코드.
 * (Phase 4 에서 리버스 프록시 라우팅이 이 함수에 추가된다) */
static int dispatch(int client_fd, const http_request_t *req, const char *doc_root)
{
    if (req->method == HTTP_METHOD_UNKNOWN) {
        http_send_error(client_fd, 405, "Allow: GET, POST\r\n");
        return 405;
    }
    return static_handle(client_fd, req, doc_root);
}

//연결 하나를 처리: 요청 1개 수신 -> 파싱 -> 응답
static void handle_client(int client_fd, const char *peer, const char *doc_root)
{
    http_conn_t    conn;
    http_request_t req;

#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    http_conn_init(&conn, client_fd);
    http_request_init(&req);

    int rc = http_read_request(&conn, &req);
    if (rc == HTTP_READ_EOF) {
        printf("[INFO] %s closed connection without sending a request\n", peer);
    } else if (rc == HTTP_READ_IO_ERROR) {
        fprintf(stderr, "[WARN] %s recv/send failed: %s\n", peer, strerror(errno));
    } else if (rc > 0) {
        /* 파싱 단계 에러 (400/413/414/501/505 ...) */
        http_send_error(client_fd, rc, NULL);
        log_access(peer, &req, rc);
        drain_before_close(client_fd);
    } else {
        int status = dispatch(client_fd, &req, doc_root);
        log_access(peer, &req, status);
    }

    http_request_free(&req);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

//리스닝 소켓 준비
int server_init(server_t *srv, uint16_t port, const char *doc_root)
{
    struct sockaddr_in addr;
    struct stat st;
    int opt = 1;

    srv->listen_fd = -1;
    srv->port      = port;

    /* 정적 파일 루트를 절대 경로로 정규화해 두면, 요청마다 realpath() 결과와
     * 단순 문자열 접두사 비교만으로 "루트 밖인지" 판단할 수 있다. */
    if (realpath(doc_root, srv->doc_root) == NULL) {
        fprintf(stderr, "doc_root '%s': %s\n", doc_root, strerror(errno));
        return -1;
    }
    if (stat(srv->doc_root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "doc_root '%s' is not a directory\n", srv->doc_root);
        return -1;
    }

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

//accept 루프
int server_run(server_t *srv)
{
    if (srv == NULL || srv->listen_fd < 0)
        return -1;

    printf("[INFO] listening on 0.0.0.0:%u, doc_root=%s\n", (unsigned)srv->port, srv->doc_root);
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

        handle_client(client_fd, peer, srv->doc_root);
        close(client_fd);   /* Phase 2: 요청 1개 처리 후 항상 닫음 (keep-alive 는 Phase 3) */
    }

    return 0;
}

//리스닝 소켓을 닫고 -1로 표시합니다.
void server_destroy(server_t *srv)
{
    if (srv == NULL)
        return;
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
}
