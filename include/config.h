/*
 * config.h - 서버 설정 (기본값 -> config.conf -> CLI 인자 순으로 덮어쓴다)
 *
 * config.conf 형식 (한 줄에 "키 값...", '#' 뒤는 주석)
 *   port                    8080
 *   root                    ./www
 *   threads                 8
 *   queue_size              256
 *   keepalive_timeout       5        # 초
 *   request_timeout         10       # 초 (SO_RCVTIMEO / SO_SNDTIMEO)
 *   max_keepalive_requests  100
 *   proxy_connect_timeout   3        # 초
 *   proxy_timeout           10       # 초
 *   route  /api/  127.0.0.1:8001  127.0.0.1:8002
 */
#ifndef MINI_WEB_SERVER_CONFIG_H
#define MINI_WEB_SERVER_CONFIG_H

#include <limits.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_MAX_ROUTES      16
#define CONFIG_MAX_BACKENDS    16
#define CONFIG_MAX_PREFIX_LEN  256

typedef struct {
    char               text[128];   /* 로그용 "127.0.0.1:8001" */
    struct sockaddr_in addr;        /* 설정을 읽을 때 미리 해석(getaddrinfo)해 둔 주소 */
} backend_addr_t;

typedef struct {
    char           prefix[CONFIG_MAX_PREFIX_LEN];
    backend_addr_t backends[CONFIG_MAX_BACKENDS];
    size_t         backend_count;
} route_config_t;

typedef struct {
    uint16_t port;
    char     doc_root[PATH_MAX];
    size_t   threads;
    size_t   queue_size;
    int      keepalive_timeout_sec;
    int      request_timeout_sec;
    int      max_keepalive_requests;
    int      proxy_connect_timeout_sec;
    int      proxy_timeout_sec;

    route_config_t routes[CONFIG_MAX_ROUTES];
    size_t         route_count;
} server_config_t;

void config_set_defaults(server_config_t *cfg);

/* 파일을 읽어 cfg 를 덮어쓴다. 성공 0, 실패 -1 (파일명:줄번호 와 함께 stderr 출력). */
int config_load_file(server_config_t *cfg, const char *path);

/* "host:port" -> backend_addr_t. 성공 0, 실패 -1. */
int config_parse_backend(const char *spec, backend_addr_t *out);

/* prefix 하나와 백엔드 목록을 라우팅 규칙으로 추가. 같은 prefix 가 있으면 교체. 성공 0. */
int config_add_route(server_config_t *cfg, const char *prefix,
                     const char *const *backend_specs, size_t n);

/* CLI 형식 "/api/=127.0.0.1:8001,127.0.0.1:8002" 을 라우팅 규칙으로 추가. 성공 0. */
int config_add_route_spec(server_config_t *cfg, const char *spec);

/* 문자열 -> 범위 [min, max] 정수. 성공 0. */
int config_parse_long(const char *s, long min, long max, long *out);

void config_print(const server_config_t *cfg);

#endif /* MINI_WEB_SERVER_CONFIG_H */
