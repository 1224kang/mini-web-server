#ifndef UPSTREAM_H
#define UPSTREAM_H

/* CGI 요청 환경변수와 출력 변환. 파이프 입출력은 conn.c 에서 맡는다. */

#include "http.h"

#include <stddef.h>

struct upstream_response {
    int status;
    char reason[64];
    const char *head;     /* CGI 출력 맨 앞부터 헤더 줄들 */
    size_t head_len;      /* 빈 줄 직전까지 */
    const char *body;
    size_t body_len;
    long content_length;  /* 업스트림이 보낸 Content-Length (없으면 -1). HEAD 응답에 쓴다 */
};

/* 업스트림이 보낸 전체 출력 buf[0..len) 을 해석한다. 형식이 틀렸으면 -1 (→ 502). */
int upstream_parse_response(const char *buf, size_t len,
                            struct upstream_response *out);

/* 클라이언트에게 보낼 응답(상태 줄 + 헤더 + 본문)을 malloc 해서 돌려준다.
 * 업스트림 헤더는 대부분 그대로 옮기되, "이 연결에서만 의미 있는" hop-by-hop 헤더
 * (Connection, Keep-Alive, Transfer-Encoding, ...)와 Content-Length 는 버리고 우리 값으로 다시 쓴다.
 * 실패하면 NULL. */
char *upstream_build_client_response(const struct upstream_response *r, int head_only,
                                     int keep_alive, size_t *out_len);

/* CGI/1.1 환경변수 배열을 만든다 ("NAME=value" 문자열들, 끝은 NULL).
 * 요청 헤더 "User-Agent: x" 는 "HTTP_USER_AGENT=x" 가 된다.
 * 다 쓰면 cgi_free_env 로 푼다. 실패하면 NULL. */
struct cgi_env_info {
    const char *script_name;  /* "/cgi-bin/add" */
    const char *path_info;    /* "/extra/path" 또는 "" */
    const char *query;        /* "a=1&b=2" 또는 "" */
    const char *remote_addr;
    int remote_port;
    int server_port;
    const char *path_env;     /* 자식에게 물려줄 PATH (스크립트의 #!/usr/bin/env python3 용) */
};
char **cgi_build_env(const struct http_request *req, size_t body_len,
                     const struct cgi_env_info *info);
void cgi_free_env(char **envp);

/* hop-by-hop 헤더인가? (대소문자 무시) */
int upstream_is_hop_by_hop(const char *name, size_t len);

#endif
