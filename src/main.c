#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int parse_port(const char *s)
{
    char *end;
    //문자열 -> 10진수
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < 1 || v > 65535)
        return -1;
    return (int)v;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }
    int port = parse_port(argv[1]); //포트번호 검사
    if (port < 0) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }

    /* 클라이언트가 먼저 끊은 소켓에 send 하면 SIGPIPE 로 서버가 죽는다.
     * 무시하면 send 가 -1(EPIPE) 을 반환하므로 에러로 처리할 수 있다. */
    signal(SIGPIPE, SIG_IGN);

    /* 로그를 파일로 리다이렉트해도 한 줄씩 바로 출력되게 한다. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int listen_fd = server_listen(port);
    if (listen_fd < 0)
        return 1;
    printf("listening on http://localhost:%d\n", port);

    /* 4) accept 루프: 연결이 올 때까지 여기서 멈춰(블로킹) 있다.
     * 한 번에 한 연결만 처리하므로, 처리 중엔 다음 손님이 큐에서 기다린다. */
    for (;;) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client, &clen);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        printf("accepted %s:%d\n", ip, ntohs(client.sin_port));

        server_handle_client(client_fd);
    }

    close(listen_fd); /* 도달하지 않음. 안전한 종료는 4단계에서 */
    return 0;
}
