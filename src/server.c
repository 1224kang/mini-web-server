#include "server.h"
#include "http.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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
/* 보내기 헬퍼                                                          */
/* ------------------------------------------------------------------ */

/* send 는 요청한 만큼 다 보내지 못할 수 있으므로 남은 만큼 반복해서 보낸다. */
static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = send(fd, buf + sent, len - sent, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            perror("send");
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

/* 응답 = [상태 라인 + 헤더] 를 만들어 보내고, 이어서 본문을 보낸다. */
static void send_response(int fd, int status, const char *content_type,
                          const char *body, size_t body_len,
                          const char *extra_headers)
{
    char head[1024];
    int n = http_build_response_head(head, sizeof(head), status, content_type,
                                     body_len, extra_headers);
    if (n < 0) {
        fprintf(stderr, "response head too large\n");
        return;
    }
    if (send_all(fd, head, (size_t)n) < 0)
        return;
    if (body_len > 0)
        send_all(fd, body, body_len);
    printf("<- %d %s (%zu bytes body)\n", status, http_reason(status), body_len);
}

/* 에러 응답: 본문은 "404 Not Found\n" 처럼 상태를 그대로 적는다. */
static void send_error(int fd, int status, const char *extra_headers)
{
    char body[128];
    int n = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
    send_response(fd, status, "text/plain; charset=utf-8", body, (size_t)n,
                  extra_headers);
}

/* ------------------------------------------------------------------ */
/* 받기 헬퍼                                                            */
/* ------------------------------------------------------------------ */

enum { READ_CLOSED = 0, READ_ERROR = -1, READ_TOO_LARGE = -2 };

/* "\r\n\r\n" 이 보일 때까지 recv 를 반복한다.
 * TCP 는 바이트 스트림이라 recv 한 번에 요청이 다 온다는 보장이 없다.
 * ("GET / HT" + "TP/1.1\r\nHost: ..." 처럼 쪼개져 올 수 있다.)
 *
 * 성공하면 헤더 길이(>0)를 반환하고, *total 에는 지금까지 받은 전체 바이트 수
 * (헤더 뒤에 딸려 온 본문 일부 포함)를 넣는다. */
static long read_head(int fd, char *buf, size_t cap, size_t *total)
{
    *total = 0;
    for (;;) {
        ssize_t n = recv(fd, buf + *total, cap - *total, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                fprintf(stderr, "recv: timed out\n");
            else
                perror("recv");
            return READ_ERROR;
        }
        if (n == 0)
            return READ_CLOSED; /* 헤더를 다 보내기 전에 클라이언트가 끊음 */

        /* 새로 받은 조각과 이전 끝 3바이트에 걸쳐 "\r\n\r\n" 이 생길 수 있으므로
         * 이전 끝에서 3바이트 앞부터 다시 찾는다. */
        size_t from = *total >= 3 ? *total - 3 : 0;
        *total += (size_t)n;
        long end = http_find_head_end(buf + from, *total - from);
        if (end > 0)
            return (long)from + end;

        if (*total == cap)
            return READ_TOO_LARGE; /* 버퍼가 꽉 찼는데도 헤더 끝이 없음 → 431 */
    }
}

/* 본문을 정확히 content_length 바이트 받는다.
 * already 는 헤더와 같이 이미 받아 둔 본문 앞부분이다. 실패하면 NULL. */
static char *read_body(int fd, const char *already, size_t already_len,
                       size_t content_length)
{
    char *body = malloc(content_length);
    if (!body) {
        perror("malloc");
        return NULL;
    }

    size_t have = already_len < content_length ? already_len : content_length;
    memcpy(body, already, have);

    while (have < content_length) {
        ssize_t n = recv(fd, body + have, content_length - have, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            free(body);
            return NULL;
        }
        if (n == 0) { /* Content-Length 만큼 보내기 전에 끊김 */
            fprintf(stderr, "client closed before sending full body (%zu/%zu)\n",
                    have, content_length);
            free(body);
            return NULL;
        }
        have += (size_t)n;
    }
    return body;
}

/* 안 읽은 데이터가 수신 버퍼에 남은 채 close 하면 커널이 RST 를 보내고,
 * 그러면 클라이언트가 우리가 보낸 에러 응답을 읽기도 전에 연결이 깨질 수 있다.
 * 그래서 "더 안 보낸다(FIN)"를 먼저 알리고, 남은 입력을 조금 비운 뒤 닫는다. */
static void close_gracefully(int fd)
{
    shutdown(fd, SHUT_WR);
    char sink[4096];
    size_t drained = 0;
    while (drained < 64 * 1024) {
        ssize_t n = recv(fd, sink, sizeof(sink), 0);
        if (n <= 0)
            break; /* 상대도 닫았거나(0), 타임아웃/오류(-1) */
        drained += (size_t)n;
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 연결 하나 처리                                                        */
/* ------------------------------------------------------------------ */

static void dispatch(int fd, const struct http_request *req,
                     const char *body, size_t body_len)
{
    if (strcmp(req->method, "GET") == 0) {
        char msg[HTTP_MAX_HEAD + 128];
        int n = snprintf(msg, sizeof(msg),
                         "Hello from mini-web-server!\n"
                         "You requested: %s %s\n",
                         req->method, req->target);
        if (n < 0 || (size_t)n >= sizeof(msg))
            n = (int)strlen(msg);
        send_response(fd, 200, "text/plain; charset=utf-8", msg, (size_t)n, NULL);
        return;
    }

    if (strcmp(req->method, "POST") == 0) {
        /* echo: 받은 본문을 받은 Content-Type 그대로 돌려준다. */
        const char *ctype = http_get_header(req, "Content-Type");
        send_response(fd, 200, ctype ? ctype : "application/octet-stream",
                      body, body_len, NULL);
        return;
    }

    /* 405 응답에는 허용 메서드를 알려 주는 Allow 헤더가 필수다. */
    send_error(fd, 405, "Allow: GET, POST\r\n");
}

void server_handle_client(int client_fd)
{
    /* 느린(혹은 악의적인) 클라이언트가 단일 스레드 서버를 영원히 붙잡지 못하게
     * recv 에 5초 제한을 건다. (제대로 된 타임아웃 설계는 4단계에서) */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 1) 헤더 받기: "\r\n\r\n" 이 나올 때까지 */
    char buf[HTTP_MAX_HEAD];
    size_t total;
    long head_len = read_head(client_fd, buf, sizeof(buf), &total);
    if (head_len == READ_TOO_LARGE) {
        send_error(client_fd, 431, NULL);
        close_gracefully(client_fd);
        return;
    }
    if (head_len <= 0) {
        close(client_fd);
        return;
    }

    /* 2) 파싱: 요청 라인 + 헤더 */
    struct http_request req;
    enum http_parse_result pr = http_parse_request(buf, (size_t)head_len, &req);
    if (pr != HTTP_PARSE_OK) {
        int status = pr == HTTP_PARSE_TOO_MANY_HEADERS ? 431
                   : pr == HTTP_PARSE_BAD_VERSION      ? 505
                                                       : 400;
        send_error(client_fd, status, NULL);
        close_gracefully(client_fd);
        return;
    }
    printf("-> %s %s %s (%zu headers)\n",
           req.method, req.target, req.version, req.header_count);

    /* 3) 본문 받기: Content-Length 만큼 */
    size_t clen;
    if (http_content_length(&req, &clen) < 0) {
        send_error(client_fd, 400, NULL);
        close_gracefully(client_fd);
        return;
    }
    if (clen > HTTP_MAX_BODY) {
        send_error(client_fd, 413, NULL);
        close_gracefully(client_fd);
        return;
    }

    char *body = NULL;
    if (clen > 0) {
        body = read_body(client_fd, buf + head_len, total - (size_t)head_len, clen);
        if (!body) {
            close(client_fd);
            return;
        }
    }

    /* 4) 처리 + 응답 */
    dispatch(client_fd, &req, body, clen);

    free(body);
    close_gracefully(client_fd);
}
