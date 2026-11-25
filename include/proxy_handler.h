/*
 * proxy_handler.h - 리버스 프록시 + Round-Robin 로드 밸런싱
 *
 *   클라이언트 ──▶ [mini-web-server] ──▶ 백엔드 A / B / A / B ... (요청마다 번갈아)
 *
 *   - 경로 prefix 가 가장 길게 일치하는 라우팅 규칙을 고른다 ("/api/v2/" > "/api/").
 *   - 규칙 안에서 Round-Robin 으로 백엔드를 고른다 (카운터는 mutex 로 보호).
 *   - 백엔드에 새 TCP 연결을 만들고, hop-by-hop 헤더를 정리한 요청을 보낸 뒤
 *     응답을 그대로 클라이언트에 중계한다.
 *   - 연결 실패 -> 502 Bad Gateway, 연결/응답 타임아웃 -> 504 Gateway Timeout.
 */
#ifndef MINI_WEB_SERVER_PROXY_HANDLER_H
#define MINI_WEB_SERVER_PROXY_HANDLER_H

#include <pthread.h>
#include <stddef.h>

#include "config.h"
#include "http.h"

typedef struct {
    char            prefix[CONFIG_MAX_PREFIX_LEN];
    size_t          prefix_len;
    backend_addr_t  backends[CONFIG_MAX_BACKENDS];
    size_t          backend_count;

    pthread_mutex_t lock;            /* next 보호: 여러 워커가 동시에 백엔드를 고른다 */
    size_t          next;
} proxy_route_t;

typedef struct {
    proxy_route_t routes[CONFIG_MAX_ROUTES];
    size_t        route_count;
    int           connect_timeout_ms;
    int           io_timeout_ms;
} proxy_table_t;

/* 설정의 라우팅 규칙으로 테이블 생성. 성공 0, 실패 -1. */
int  proxy_table_init(proxy_table_t *table, const server_config_t *cfg);
void proxy_table_destroy(proxy_table_t *table);

/* path 와 가장 길게 일치하는 규칙. 없으면 NULL. */
proxy_route_t *proxy_match(proxy_table_t *table, const char *path);

/* Round-Robin: 이번에 쓸 백엔드 인덱스를 돌려주고 카운터를 하나 전진. (스레드 안전) */
size_t proxy_next_backend(proxy_route_t *route);

/* 요청을 백엔드로 중계하고 클라이언트에 응답한다. 반환: 클라이언트에 보낸 상태 코드.
 * 백엔드 응답 길이를 알 수 없으면(chunked, Content-Length 없음) req->keep_alive 를 0 으로 내린다. */
int proxy_handle(int client_fd, http_request_t *req, proxy_table_t *table,
                 proxy_route_t *route, const char *client_ip);

#endif /* MINI_WEB_SERVER_PROXY_HANDLER_H */
