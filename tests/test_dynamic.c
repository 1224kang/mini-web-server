/* 5단계 "문자열 변환" 함수 단위 테스트 (프로세스, 소켓 없이).
 *   - upstream_parse_response / upstream_build_client_response : CGI 출력 → 클라이언트 응답
 *   - cgi_build_env       : 요청 → CGI 환경변수
 *   - builtin_handle, query_get
 * 실행: make test */
#include "builtin.h"
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
    CHECK(upstream_parse_response(out, strlen(out), &r) == 0);
    CHECK(r.status == 200);
    CHECK(r.body_len == 5 && memcmp(r.body, "hello", 5) == 0);

    out = "Status: 404 Nope\r\nContent-Type: text/plain\r\n\r\nx";
    CHECK(upstream_parse_response(out, strlen(out), &r) == 0);
    CHECK(r.status == 404 && strcmp(r.reason, "Nope") == 0);

    out = "Location: /there\r\n\r\n"; /* Status 없이 Location → 302 */
    CHECK(upstream_parse_response(out, strlen(out), &r) == 0);
    CHECK(r.status == 302 && strcmp(r.reason, "Found") == 0);

    CHECK(upstream_parse_response("", 0, &r) < 0);              /* 출력 없음 */
    out = "Content-Type: text/plain\r\nhello"; /* 빈 줄 없음 */
    CHECK(upstream_parse_response(out, strlen(out), &r) < 0);
    out = "no colon here\r\n\r\n";
    CHECK(upstream_parse_response(out, strlen(out), &r) < 0);
    out = "Status: abc\r\n\r\n";
    CHECK(upstream_parse_response(out, strlen(out), &r) < 0);
    out = "X-A: 1\rX-B: 2\r\n\r\n"; /* 줄 가운데 \r */
    CHECK(upstream_parse_response(out, strlen(out), &r) < 0);
    printf("PASS parse cgi output\n");
}

static void test_build_client_response(void)
{
    struct upstream_response r;
    /* CGI: Status 헤더는 상태 줄로 옮기고 헤더에선 뺀다 */
    size_t len;
    const char *out = "Status: 404 Not Found\nContent-Type: text/plain\n\nnope";
    CHECK(upstream_parse_response(out, strlen(out), &r) == 0);
    char *resp = upstream_build_client_response(&r, 0, 1, &len);
    CHECK(resp && strncmp(resp, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    LACKS(resp, "Status:");
    CONTAINS(resp, "Content-Length: 4\r\n");
    free(resp);
    printf("PASS build client response\n");
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

int main(void)
{
    test_parse_cgi();
    test_build_client_response();
    test_cgi_env();
    test_builtin();
    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
