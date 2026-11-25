/* 5·6단계 "문자열 변환" 함수 단위 테스트 (프로세스, 소켓 없이).
 *   - upstream_parse_response / upstream_build_client_response : CGI·백엔드 응답 → 클라이언트 응답
 *   - proxy_build_request : 클라이언트 요청 → 백엔드 요청 (Host, X-Forwarded-For 재작성)
 *   - cgi_build_env       : 요청 → CGI 환경변수
 *   - builtin_handle, query_get, config_match_route / config_pick_backend
 * 실행: make test */
#include "builtin.h"
#include "config.h"
#include "http.h"
#include "upstream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_total;

#define CHECK(cond)                                                           \
    do {                                                                      \
        g_total++;                                                            \
        if (!(cond)) {                                                        \
            g_failed++;                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

#define CONTAINS(hay, needle) CHECK((hay) != NULL && strstr((hay), (needle)) != NULL)
#define LACKS(hay, needle)    CHECK((hay) != NULL && strstr((hay), (needle)) == NULL)

static char *parse_req(const char *raw, struct http_request *req)
{
    size_t len = strlen(raw);
    char *buf = malloc(len + 1);
    memcpy(buf, raw, len + 1);
    long end = http_find_head_end(buf, len);
    if (end < 0 || http_parse_request(buf, (size_t)end, req) != HTTP_PARSE_OK) {
        free(buf);
        return NULL;
    }
    return buf;
}

static void test_parse_cgi(void)
{
    struct upstream_response r;
    const char *out = "Content-Type: text/plain\n\nhello";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_CGI, &r) == 0);
    CHECK(r.status == 200);
    CHECK(r.body_len == 5 && memcmp(r.body, "hello", 5) == 0);

    out = "Status: 404 Nope\r\nContent-Type: text/plain\r\n\r\nx";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_CGI, &r) == 0);
    CHECK(r.status == 404 && strcmp(r.reason, "Nope") == 0);

    out = "Location: /there\r\n\r\n"; /* Status 없이 Location → 302 */
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_CGI, &r) == 0);
    CHECK(r.status == 302 && strcmp(r.reason, "Found") == 0);

    CHECK(upstream_parse_response("", 0, UPSTREAM_CGI, &r) < 0);              /* 출력 없음 */
    out = "Content-Type: text/plain\r\nhello"; /* 빈 줄 없음 */
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_CGI, &r) < 0);
    out = "no colon here\r\n\r\n";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_CGI, &r) < 0);
    out = "Status: abc\r\n\r\n";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_CGI, &r) < 0);
    out = "X-A: 1\rX-B: 2\r\n\r\n"; /* 줄 가운데 \r */
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_CGI, &r) < 0);
    printf("PASS parse cgi output\n");
}

static void test_parse_http(void)
{
    struct upstream_response r;
    const char *out = "HTTP/1.0 201 Created\r\nContent-Length: 3\r\nX-A: 1\r\n\r\nabcEXTRA";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_HTTP, &r) == 0);
    CHECK(r.status == 201 && strcmp(r.reason, "Created") == 0);
    CHECK(r.content_length == 3 && r.body_len == 3); /* 남는 바이트는 버림 */

    out = "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\nabc"; /* 덜 옴: 판단은 호출한 쪽이 */
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_HTTP, &r) == 0);
    CHECK(r.content_length == 10 && r.body_len == 3);

    out = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_HTTP, &r) == 0 && r.chunked);
    out = "garbage\r\n\r\n";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_HTTP, &r) < 0);
    out = "HTTP/1.0 999 What\r\n\r\n";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_HTTP, &r) < 0);
    printf("PASS parse http response\n");
}

static void test_decode_chunks(void)
{
    struct proxy_chunks chunks = { 0 };
    char body[9] = { 0 };
    size_t used = 0;
    const char *encoded = "3;name=value\r\nabc\r\n5\r\ndefgh\r\n0\r\nX-Test: yes\r\n\r\n";
    int done = 0;
    for (size_t i = 0; encoded[i] && done == 0; i++) {
        char ch = encoded[i];
        size_t decoded;
        done = proxy_decode_chunks(&chunks, &ch, 1, &decoded);
        if (decoded && used < sizeof(body) - 1)
            body[used++] = ch;
    }
    CHECK(done == 1);
    CHECK(strcmp(body, "abcdefgh") == 0);

    chunks = (struct proxy_chunks){ 0 };
    char joined[] = "2\r\nab\r\n2\r\ncd\r\n0\r\n\r\n";
    size_t decoded;
    CHECK(proxy_decode_chunks(&chunks, joined, sizeof(joined) - 1, &decoded) == 1);
    CHECK(decoded == 4 && memcmp(joined, "abcd", 4) == 0);

    chunks = (struct proxy_chunks){ 0 };
    char invalid[] = "oops\r\n";
    CHECK(proxy_decode_chunks(&chunks, invalid, sizeof(invalid) - 1, &decoded) < 0);
    printf("PASS decode incrementally framed chunks\n");
}

static void test_build_client_response(void)
{
    struct upstream_response r;
    const char *out = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close, X-Secret\r\n"
                      "Keep-Alive: timeout=5\r\nX-Secret: 1\r\nContent-Length: 2\r\nX-Keep: yes\r\n\r\nhi";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_HTTP, &r) == 0);
    size_t len;
    char *resp = upstream_build_client_response(&r, 0, 1, "1.1 test", &len);
    CHECK(resp && strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CONTAINS(resp, "Content-Type: text/plain\r\n");
    CONTAINS(resp, "X-Keep: yes\r\n");
    CONTAINS(resp, "Connection: keep-alive\r\n"); /* 백엔드의 close 가 아니라 우리 사정 */
    CONTAINS(resp, "Via: 1.1 test\r\n");
    CONTAINS(resp, "Content-Length: 2\r\n");
    LACKS(resp, "Keep-Alive:");
    LACKS(resp, "X-Secret"); /* Connection 에 이름이 적힌 헤더도 hop-by-hop */
    LACKS(resp, "close");
    CHECK(resp && len >= 2 && memcmp(resp + len - 6, "\r\n\r\nhi", 6) == 0);
    free(resp);

    /* HEAD: 본문 없이, 백엔드가 알려 준 Content-Length 그대로 */
    out = "HTTP/1.0 200 OK\r\nContent-Length: 1234\r\n\r\n";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_HTTP, &r) == 0);
    resp = upstream_build_client_response(&r, 1, 0, NULL, &len);
    CONTAINS(resp, "Content-Length: 1234\r\n");
    CONTAINS(resp, "Connection: close\r\n");
    CHECK(resp && len >= 4 && memcmp(resp + len - 4, "\r\n\r\n", 4) == 0);
    free(resp);

    /* CGI: Status 헤더는 상태 줄로 옮기고 헤더에선 뺀다 */
    out = "Status: 404 Not Found\nContent-Type: text/plain\n\nnope";
    CHECK(upstream_parse_response(out, strlen(out), UPSTREAM_CGI, &r) == 0);
    resp = upstream_build_client_response(&r, 0, 1, NULL, &len);
    CHECK(resp && strncmp(resp, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    LACKS(resp, "Status:");
    CONTAINS(resp, "Content-Length: 4\r\n");
    free(resp);
    printf("PASS build client response\n");
}

static void test_proxy_request(void)
{
    struct http_request req;
    char *buf = parse_req("POST /api/x?y=1 HTTP/1.1\r\nHost: example.com:18080\r\n"
                          "Connection: keep-alive, X-Hop\r\nX-Hop: 1\r\nKeep-Alive: 5\r\n"
                          "X-Forwarded-For: 1.1.1.1\r\nUser-Agent: t\r\nContent-Length: 3\r\n"
                          "Expect: 100-continue\r\n\r\n", &req);
    CHECK(buf != NULL);
    if (!buf)
        return;
    size_t len;
    char *out = proxy_build_request(&req, "abc", 3, "10.0.0.7", "127.0.0.1:8001", &len);
    CHECK(out && strncmp(out, "POST /api/x?y=1 HTTP/1.1\r\n", 26) == 0);
    CONTAINS(out, "Host: 127.0.0.1:8001\r\n");
    CONTAINS(out, "X-Forwarded-Host: example.com:18080\r\n");
    CONTAINS(out, "X-Forwarded-For: 1.1.1.1, 10.0.0.7\r\n");
    CONTAINS(out, "X-Forwarded-Proto: http\r\n");
    CONTAINS(out, "Via: 1.1 mini-web-server\r\n");
    CONTAINS(out, "User-Agent: t\r\n");
    CONTAINS(out, "Content-Length: 3\r\n");
    CONTAINS(out, "Connection: close\r\n");
    LACKS(out, "keep-alive");
    LACKS(out, "X-Hop");
    LACKS(out, "Expect");
    LACKS(out, "\r\nHost: example.com");
    CHECK(out && memcmp(out + len - 7, "\r\n\r\nabc", 7) == 0);
    free(out);
    free(buf);

    buf = parse_req("GET / HTTP/1.1\r\nHost: h\r\n\r\n", &req);
    out = proxy_build_request(&req, NULL, 0, "10.0.0.7", "b:1", &len);
    CONTAINS(out, "X-Forwarded-For: 10.0.0.7\r\n");
    LACKS(out, "Content-Length");
    free(out);
    free(buf);
    printf("PASS proxy request rewrite\n");
}

static const char *env_get(char **env, const char *name)
{
    size_t n = strlen(name);
    for (; env && *env; env++)
        if (strncmp(*env, name, n) == 0 && (*env)[n] == '=')
            return *env + n + 1;
    return NULL;
}

static void test_cgi_env(void)
{
    struct http_request req;
    char *buf = parse_req("POST /cgi-bin/form/a/b?x=1 HTTP/1.1\r\nHost: localhost:18080\r\n"
                          "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 7\r\n"
                          "Accept-Language: ko\r\nProxy: http://evil\r\nX_Under: 1\r\n\r\n", &req);
    CHECK(buf != NULL);
    if (!buf)
        return;
    struct cgi_env_info info = { "/cgi-bin/form", "/a/b", "x=1", "10.0.0.7", 5555, 18080, "/usr/bin" };
    char **env = cgi_build_env(&req, 7, &info);
    CHECK(env != NULL);
    const char *v;
    CHECK((v = env_get(env, "REQUEST_METHOD")) && strcmp(v, "POST") == 0);
    CHECK((v = env_get(env, "SCRIPT_NAME")) && strcmp(v, "/cgi-bin/form") == 0);
    CHECK((v = env_get(env, "PATH_INFO")) && strcmp(v, "/a/b") == 0);
    CHECK((v = env_get(env, "QUERY_STRING")) && strcmp(v, "x=1") == 0);
    CHECK((v = env_get(env, "CONTENT_LENGTH")) && strcmp(v, "7") == 0);
    CHECK((v = env_get(env, "CONTENT_TYPE")) && strcmp(v, "application/x-www-form-urlencoded") == 0);
    CHECK((v = env_get(env, "SERVER_NAME")) && strcmp(v, "localhost") == 0);
    CHECK((v = env_get(env, "SERVER_PORT")) && strcmp(v, "18080") == 0);
    CHECK((v = env_get(env, "REMOTE_ADDR")) && strcmp(v, "10.0.0.7") == 0);
    CHECK((v = env_get(env, "HTTP_ACCEPT_LANGUAGE")) && strcmp(v, "ko") == 0);
    CHECK(env_get(env, "HTTP_PROXY") == NULL);          /* httpoxy */
    CHECK(env_get(env, "HTTP_CONTENT_LENGTH") == NULL); /* 따로 넣었음 */
    CHECK(env_get(env, "HTTP_X_UNDER") == NULL);        /* 밑줄 헤더는 버림 */
    cgi_free_env(env);
    free(buf);
    printf("PASS cgi env\n");
}

static void test_builtin(void)
{
    char v[32];
    CHECK(query_get("/x?a=1&b=2", "b", v, sizeof(v)) == 1 && strcmp(v, "2") == 0);
    CHECK(query_get("/x?ab=1&a=%2D3", "a", v, sizeof(v)) == 1 && strcmp(v, "-3") == 0);
    CHECK(query_get("/x?a=hi+there#frag", "a", v, sizeof(v)) == 1 && strcmp(v, "hi there") == 0);
    CHECK(query_get("/x?a=1", "b", v, sizeof(v)) == 0);
    CHECK(query_get("/x", "a", v, sizeof(v)) == 0);
    CHECK(query_get("/x?a=%zz", "a", v, sizeof(v)) == -1);

    struct http_request req;
    struct builtin_response br;
    char *buf = parse_req("GET /builtin/add?a=40&b=2 HTTP/1.1\r\nHost: h\r\n\r\n", &req);
    CHECK(builtin_handle(&req, NULL, 0, &br) == 1 && br.status == 200);
    CHECK(br.body_len == 3 && memcmp(br.body, "42\n", 3) == 0);
    free(buf);
    buf = parse_req("GET /builtin/add?a=9223372036854775807&b=1 HTTP/1.1\r\nHost: h\r\n\r\n", &req);
    CHECK(builtin_handle(&req, NULL, 0, &br) == 1 && br.status == 400);
    free(buf);
    buf = parse_req("POST /builtin/add HTTP/1.1\r\nHost: h\r\n\r\n", &req);
    CHECK(builtin_handle(&req, NULL, 0, &br) == 1 && br.status == 405 && br.extra_headers);
    free(buf);
    buf = parse_req("POST /builtin/echo HTTP/1.1\r\nHost: h\r\nContent-Type: text/x\r\n\r\n", &req);
    CHECK(builtin_handle(&req, "ping", 4, &br) == 1 && br.status == 200 && br.body_len == 4);
    CHECK(strcmp(br.content_type, "text/x") == 0);
    free(buf);
    buf = parse_req("GET /index.html HTTP/1.1\r\nHost: h\r\n\r\n", &req);
    CHECK(builtin_handle(&req, NULL, 0, &br) == 0);
    free(buf);
    printf("PASS builtin handlers\n");
}

static void test_routes(void)
{
    struct server_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.nroutes = 2;
    strcpy(cfg.routes[0].prefix, "/api/");
    cfg.routes[0].nbackends = 3;
    strcpy(cfg.routes[0].backends[0].name, "a");
    strcpy(cfg.routes[0].backends[1].name, "b");
    strcpy(cfg.routes[0].backends[2].name, "c");
    strcpy(cfg.routes[1].prefix, "/api/v2/");
    cfg.routes[1].nbackends = 1;

    CHECK(config_match_route(&cfg, "/api/users") == &cfg.routes[0]);
    CHECK(config_match_route(&cfg, "/api/v2/users") == &cfg.routes[1]); /* 가장 긴 접두사 */
    CHECK(config_match_route(&cfg, "/apix") == NULL);
    CHECK(config_match_route(&cfg, "/index.html") == NULL);

    const char *seq = "abcab";
    for (int i = 0; seq[i]; i++)
        CHECK(config_pick_backend(&cfg.routes[0])->name[0] == seq[i]);
    printf("PASS routes + round-robin\n");
}

int main(void)
{
    test_parse_cgi();
    test_parse_http();
    test_decode_chunks();
    test_build_client_response();
    test_proxy_request();
    test_cgi_env();
    test_builtin();
    test_routes();
    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
