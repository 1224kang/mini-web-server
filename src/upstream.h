#ifndef UPSTREAM_H
#define UPSTREAM_H

/*
 * "업스트림" = 우리 서버가 요청을 대신 넘기는 상대. 5단계의 CGI 프로그램, 6단계의 백엔드 서버.
 *
 * 둘은 겉모습만 다르고 하는 일은 같다.
 *
 *   클라이언트 ──요청──▶ [우리 서버] ──(바꿔서) 넘김──▶ 업스트림
 *   클라이언트 ◀──응답── [우리 서버] ◀──(고쳐서) 받음── 업스트림
 *
 *   CGI : 요청 → 환경변수 + stdin,   응답 ← stdout ("Content-Type: ...\n\n본문")
 *   프록시: 요청 → HTTP 요청 다시 쓰기, 응답 ← HTTP 응답 ("HTTP/1.0 200 OK\r\n...\r\n\r\n본문")
 *
 * 이 파일은 소켓/프로세스를 전혀 모르는 "문자열 변환" 함수만 모았다 → 단위 테스트 가능.
 * 실제 fork/exec, connect, read/write 는 cgi.c 와 conn.c 가 한다.
 */

#include "http.h"

#include <stddef.h>

enum upstream_kind {
    UPSTREAM_CGI,  /* 응답 = CGI 출력 (상태는 "Status:" 헤더로) */
    UPSTREAM_HTTP, /* 응답 = HTTP 응답 (상태는 첫 줄로) */
};

struct upstream_response {
    int status;
    char reason[64];
    const char *head;     /* 헤더 줄들 (CGI: 출력 맨 앞부터, HTTP: 상태 줄 다음부터) */
    size_t head_len;      /* 빈 줄 직전까지 */
    const char *body;
    size_t body_len;
    long content_length;  /* 업스트림이 보낸 Content-Length (없으면 -1). HEAD 응답에 쓴다 */
    int chunked;
    char version_minor;
};

/* 업스트림이 보낸 전체 출력 buf[0..len) 을 해석한다. 형식이 틀렸으면 -1 (→ 502). */
int upstream_parse_response(const char *buf, size_t len, enum upstream_kind kind,
                            struct upstream_response *out);

/* 클라이언트에게 보낼 응답(상태 줄 + 헤더 + 본문)을 malloc 해서 돌려준다.
 * 업스트림 헤더는 대부분 그대로 옮기되, "이 연결에서만 의미 있는" hop-by-hop 헤더
 * (Connection, Keep-Alive, Transfer-Encoding, ...)와 Content-Length 는 버리고 우리 값으로 다시 쓴다.
 * via 가 NULL 이 아니면 "Via: <via>" 를 붙인다. 실패하면 NULL. */
char *upstream_build_client_response(const struct upstream_response *r, int head_only,
                                     int keep_alive, const char *via, size_t *out_len);

/* 프록시 중계용 헤더. 본문은 호출자가 조각 단위로 전송한다. */
char *upstream_build_proxy_head(const struct upstream_response *r, const char *version,
                                int send_chunked, int keep_alive, size_t *out_len);

enum chunk_step { CHUNK_SIZE, CHUNK_DATA, CHUNK_DATA_CR, CHUNK_DATA_LF, CHUNK_TRAILERS };

struct proxy_chunks {
    enum chunk_step step;
    size_t left;
    size_t line_len;
    size_t trailer_bytes;
    char line[1024];
};

/* 조각 경계를 검증하고 buf 앞에 본문을 모은다. 0은 계속, 1은 완료, -1은 형식 오류. */
int proxy_decode_chunks(struct proxy_chunks *chunks, char *buf, size_t len, size_t *decoded);

/* proxy_decode_chunks 와 같고, *used 에 buf 에서 소비한 바이트 수를 돌려준다.
 * 완료(1)면 마지막 빈 줄 바로 뒤까지이고, 그 뒤 바이트는 다음 메시지(파이프라이닝)다.
 * 계속(0)이면 len 전부를 소비한다. 요청 본문(Transfer-Encoding: chunked)을 읽을 때 쓴다. */
int http_decode_chunks(struct proxy_chunks *chunks, char *buf, size_t len, size_t *decoded,
                       size_t *used);

/* 프록시: 클라이언트 요청을 백엔드에게 보낼 요청으로 다시 쓴다 (malloc, 실패하면 NULL).
 *
 *   - 백엔드와는 HTTP/1.1 + "Connection: close": 요청마다 새 연결을 사용한다.
 *   - Host                → 백엔드 주소로 바꾸고, 원래 Host 는 X-Forwarded-Host 로 보낸다.
 *   - X-Forwarded-For     → 기존 값 뒤에 ", <클라이언트 IP>" 를 덧붙인다(프록시를 여러 번 거쳐도 경로가 남게).
 *   - X-Forwarded-Proto   → "http"
 *   - hop-by-hop 헤더와 Expect 는 버린다. */
char *proxy_build_request(const struct http_request *req, const char *body, size_t body_len,
                          const char *client_ip, const char *backend_host, size_t *out_len);

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
