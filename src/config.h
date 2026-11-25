#ifndef CONFIG_H
#define CONFIG_H

/*
 * 서버 설정. 포트/docroot/max_conns 는 명령줄로, 나머지(CGI, 프록시)는 설정 파일로 받는다.
 *
 * 설정 파일 형식 (한 줄에 하나, '#' 뒤는 주석):
 *
 *   cgi_dir                  cgi-bin
 *   cgi_timeout_ms           5000
 *   proxy_connect_timeout_ms 1000
 *   proxy_timeout_ms         5000
 *   proxy  /api/  127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003
 *
 * proxy 줄은 "이 접두사로 시작하는 요청은 이 백엔드들에게 번갈아(Round-Robin) 넘긴다"는 뜻.
 * 여러 줄이 겹치면 가장 긴 접두사가 이긴다.
 */

#include <limits.h> /* PATH_MAX */
#include <netinet/in.h>

#define CONFIG_MAX_ROUTES   16
#define CONFIG_MAX_BACKENDS 8
#define CONFIG_MAX_PREFIX   128

struct backend {
    char name[64];             /* 로그와 헤더에 쓸 "127.0.0.1:8001" */
    struct sockaddr_in addr;   /* 시작할 때 한 번만 주소를 풀어 둔다(요청마다 DNS 조회 안 함) */
};

struct proxy_route {
    char prefix[CONFIG_MAX_PREFIX];
    struct backend backends[CONFIG_MAX_BACKENDS];
    int nbackends;
    unsigned long next; /* 다음에 고를 백엔드 번호. 스레드가 하나라 lock 이 필요 없다 */
};

struct server_config {
    int port;
    const char *docroot;          /* realpath 로 정규화된 절대 경로 */
    char cgi_dir[PATH_MAX];       /* realpath 로 정규화. 빈 문자열이면 CGI 끔 */
    int cgi_timeout_ms;
    int proxy_connect_timeout_ms;
    int proxy_timeout_ms;         /* 백엔드가 이 시간 동안 한 바이트도 안 보내면 504 */
    struct proxy_route routes[CONFIG_MAX_ROUTES];
    int nroutes;
};

/* 기본값을 채운다. cgi_dir 은 "./cgi-bin" 이 있으면 그걸 쓴다. */
void config_defaults(struct server_config *cfg);

/* 설정 파일을 읽어 cfg 를 덮어쓴다. 실패하면 stderr 에 이유를 쓰고 -1. */
int config_load(struct server_config *cfg, const char *path);

/* target 경로("/api/users?x=1")에 맞는 프록시 경로. 없으면 NULL. */
struct proxy_route *config_match_route(struct server_config *cfg, const char *target);

/* Round-Robin: 호출할 때마다 다음 백엔드를 돌려준다. */
const struct backend *config_pick_backend(struct proxy_route *route);

#endif
