#include "config.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CLI_ROUTES CONFIG_MAX_ROUTES

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] [port] [doc_root]\n"
            "  -c FILE        config file (e.g. config.conf)\n"
            "  -p PORT        listen port            (default 8080)\n"
            "  -r DIR         static file root       (default ./www)\n"
            "  -t N           worker threads         (default 8)\n"
            "  -q N           connection queue size  (default 256)\n"
            "  -R SPEC        proxy route, repeatable: /api/=127.0.0.1:8001,127.0.0.1:8002\n"
            "  -h             show this help\n"
            "Priority: defaults < config file (-c) < command-line options.\n",
            prog);
}

int main(int argc, char *argv[])
{
    /* 설정 구조체가 수십 KB 라서 스택 대신 정적 영역에 둔다 */
    static server_config_t cfg;
    static server_t        srv;

    const char *config_path = NULL;
    const char *opt_port = NULL, *opt_root = NULL, *opt_threads = NULL, *opt_queue = NULL;
    const char *opt_routes[MAX_CLI_ROUTES];
    size_t      n_routes = 0;
    long        v;
    int         c;

    /* 로그가 파이프/파일로 리다이렉트돼도 줄 단위로 바로 출력되도록 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    //1) 옵션을 먼저 모아 두고, "기본값 -> 설정 파일 -> CLI" 순서로 적용합니다.
    while ((c = getopt(argc, argv, "c:p:r:t:q:R:h")) != -1) {
        switch (c) {
        case 'c': config_path = optarg; break;
        case 'p': opt_port    = optarg; break;
        case 'r': opt_root    = optarg; break;
        case 't': opt_threads = optarg; break;
        case 'q': opt_queue   = optarg; break;
        case 'R':
            if (n_routes == MAX_CLI_ROUTES) {
                fprintf(stderr, "too many -R options (max %d)\n", MAX_CLI_ROUTES);
                return EXIT_FAILURE;
            }
            opt_routes[n_routes++] = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    //Phase 1~2 와의 호환: 옵션 뒤의 위치 인자 [port] [doc_root]
    if (argc - optind > 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (optind < argc)
        opt_port = argv[optind];
    if (optind + 1 < argc)
        opt_root = argv[optind + 1];

    //2) 기본값 -> 설정 파일
    config_set_defaults(&cfg);
    if (config_path != NULL && config_load_file(&cfg, config_path) < 0)
        return EXIT_FAILURE;

    //3) CLI 로 덮어쓰기
    if (opt_port != NULL) {
        if (config_parse_long(opt_port, 1, 65535, &v) < 0) {
            fprintf(stderr, "Invalid port: '%s'\n", opt_port);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        cfg.port = (uint16_t)v;
    }
    if (opt_root != NULL) {
        if (strlen(opt_root) >= sizeof(cfg.doc_root)) {
            fprintf(stderr, "doc_root too long\n");
            return EXIT_FAILURE;
        }
        snprintf(cfg.doc_root, sizeof(cfg.doc_root), "%s", opt_root);
    }
    if (opt_threads != NULL) {
        if (config_parse_long(opt_threads, 1, 1024, &v) < 0) {
            fprintf(stderr, "Invalid thread count: '%s'\n", opt_threads);
            return EXIT_FAILURE;
        }
        cfg.threads = (size_t)v;
    }
    if (opt_queue != NULL) {
        if (config_parse_long(opt_queue, 1, 65536, &v) < 0) {
            fprintf(stderr, "Invalid queue size: '%s'\n", opt_queue);
            return EXIT_FAILURE;
        }
        cfg.queue_size = (size_t)v;
    }
    for (size_t i = 0; i < n_routes; i++) {
        if (config_add_route_spec(&cfg, opt_routes[i]) < 0)
            return EXIT_FAILURE;
    }

    //실패하면(예: 포트가 이미 사용 중, 루트 디렉토리 없음) 메시지를 출력하고 종료합니다.
    if (server_init(&srv, &cfg) < 0) {
        fprintf(stderr, "Failed to start server on port %u\n", (unsigned)cfg.port);
        return EXIT_FAILURE;
    }

    //SIGINT/SIGTERM 을 받을 때까지 accept 루프를 돕니다. 받으면 진행 중인 요청을 마치고 반환합니다.
    int rc = server_run(&srv);

    /*
        Phase 5: Graceful Shutdown 이 붙어서 server_run 이 실제로 반환합니다.
        여기서 리스닝 소켓, 파이프, 라우팅 테이블을 모두 정리하므로
        ASan(LeakSanitizer) 으로 종료 시 누수 0 을 확인할 수 있습니다.
    */
    server_destroy(&srv);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
