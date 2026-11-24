#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [port] [doc_root]\n", prog);
    fprintf(stderr, "  port     : 1-65535 (default: %d)\n", SERVER_DEFAULT_PORT);
    fprintf(stderr, "  doc_root : static file directory (default: %s)\n", SERVER_DEFAULT_DOC_ROOT);
}

/* 문자열을 포트 번호로 변환. 성공 0, 실패 -1. */
static int parse_port(const char *s, uint16_t *out)
{
    char *end = NULL;

    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
        return -1;

    *out = (uint16_t)v;
    return 0;
}

int main(int argc, char *argv[])
{
    uint16_t port = SERVER_DEFAULT_PORT;
    const char *doc_root = SERVER_DEFAULT_DOC_ROOT;
    server_t srv;

    /* 로그가 파이프/파일로 리다이렉트돼도 줄 단위로 바로 출력되도록 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    //인자가 너무 많으면 사용법을 출력하고 실패로 끝냅니다.
    if (argc > 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    //-h/--help이면 사용법을 출력하고 성공으로 끝냅니다. 아니면 parse_port를 호출합니다.
    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (parse_port(argv[1], &port) < 0) {
            fprintf(stderr, "Invalid port: '%s'\n", argv[1]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    //두 번째 인자는 정적 파일 루트 디렉토리입니다.
    if (argc == 3)
        doc_root = argv[2];

    //실패하면(예: 포트가 이미 사용 중, 루트 디렉토리 없음) 메시지를 출력하고 종료합니다.
    if (server_init(&srv, port, doc_root) < 0) {
        fprintf(stderr, "Failed to start server on port %u\n", (unsigned)port);
        return EXIT_FAILURE;
    }

    //무한 루프에 들어가므로 정상 상황에서는 반환하지 않습니다.
    int rc = server_run(&srv);
    
    /*
        반환된 경우를 대비한 정리 코드입니다. 
        Phase 5에서 Graceful Shutdown이 붙으면 server_run이 실제로 반환하게 되고, 
        그때 이 줄이 의미를 갖습니다.
    */
    server_destroy(&srv);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
