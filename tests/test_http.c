/* src/http.c 단위 테스트. 소켓 없이 문자열만 넣어 보고 결과를 확인한다.
 * 실행: make test */
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_total;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_total++;                                                        \
        if (!(cond)) {                                                    \
            g_failed++;                                                   \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_STR(a, b) CHECK((a) != NULL && strcmp((a), (b)) == 0)

/* 문자열 리터럴은 수정할 수 없으므로 복사본을 만들어 파싱한다. */
static enum http_parse_result parse(const char *raw, struct http_request *req, char **owned)
{
    size_t len = strlen(raw);
    *owned = malloc(len + 1);
    memcpy(*owned, raw, len + 1);
    long end = http_find_head_end(*owned, len);
    if (end < 0)
        return HTTP_PARSE_BAD_REQUEST;
    return http_parse_request(*owned, (size_t)end, req);
}

static enum http_parse_result parse_only(const char *raw)
{
    struct http_request req;
    char *buf;
    enum http_parse_result r = parse(raw, &req, &buf);
    free(buf);
    return r;
}

static void test_find_head_end(void)
{
    const char *full = "GET / HTTP/1.1\r\nHost: a\r\n\r\nBODY";
    CHECK(http_find_head_end(full, strlen(full)) == (long)(strlen(full) - 4));

    /* 아직 덜 받은 상태들: 끝 표시가 없으면 -1 */
    CHECK(http_find_head_end("GET / HTTP/1.1\r\nHost: a\r\n", 25) == -1);
    CHECK(http_find_head_end("GET / HTTP/1.1\r\nHost: a\r\n\r", 26) == -1);
    CHECK(http_find_head_end("", 0) == -1);
    CHECK(http_find_head_end("\r\n\r\n", 4) == 4);
}

static void test_simple_get(void)
{
    struct http_request req;
    char *buf;
    enum http_parse_result r = parse(
        "GET /hello?x=1 HTTP/1.1\r\n"
        "Host: localhost:18080\r\n"
        "User-Agent:   curl/8.0   \r\n"
        "Accept: */*\r\n"
        "\r\n",
        &req, &buf);
    CHECK(r == HTTP_PARSE_OK);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.target, "/hello?x=1");
    CHECK_STR(req.version, "HTTP/1.1");
    CHECK(req.header_count == 3);
    CHECK_STR(http_get_header(&req, "Host"), "localhost:18080");
    CHECK_STR(http_get_header(&req, "user-agent"), "curl/8.0"); /* 대소문자 무시 + 공백 제거 */
    CHECK(http_get_header(&req, "Cookie") == NULL);

    size_t clen = 123;
    CHECK(http_content_length(&req, &clen) == 0 && clen == 0); /* 없으면 0 */
    free(buf);
}

static void test_post_content_length(void)
{
    struct http_request req;
    char *buf;
    const char *raw = "POST /echo HTTP/1.1\r\n"
                      "Host: a\r\n"
                      "Content-Length: 11\r\n"
                      "\r\n"
                      "hello world";
    CHECK(parse(raw, &req, &buf) == HTTP_PARSE_OK);
    size_t clen;
    CHECK(http_content_length(&req, &clen) == 0 && clen == 11);

    /* 파서는 헤더 뒤 본문을 건드리지 않아야 한다. */
    long end = http_find_head_end(raw, strlen(raw));
    CHECK(memcmp(buf + end, "hello world", 11) == 0);
    free(buf);
}

static void test_empty_header_value(void)
{
    struct http_request req;
    char *buf;
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nX-Empty:\r\n\r\n", &req, &buf) == HTTP_PARSE_OK);
    CHECK_STR(http_get_header(&req, "X-Empty"), "");
    free(buf);
}

static void test_bad_request_line(void)
{
    CHECK(parse_only("GET /\r\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);           /* 버전 없음 */
    CHECK(parse_only("GET  / HTTP/1.1\r\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST); /* 공백 두 개 */
    CHECK(parse_only("GET / HTTP/1.1 x\r\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);/* 토큰 4개 */
    CHECK(parse_only(" GET / HTTP/1.1\r\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST); /* 앞 공백 */
    CHECK(parse_only("G(T / HTTP/1.1\r\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);  /* 메서드에 이상한 문자 */
    CHECK(parse_only("GET / HTPT/1.1\r\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);  /* HTTP 가 아님 */
    CHECK(parse_only("GET / HTTP/1\r\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    CHECK(parse_only("\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);                           /* 빈 요청 */
}

static void test_bad_version(void)
{
    CHECK(parse_only("GET / HTTP/2.0\r\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_VERSION);
    CHECK(parse_only("GET / HTTP/0.9\r\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_VERSION);
    CHECK(parse_only("GET / HTTP/1.0\r\n\r\n") == HTTP_PARSE_OK); /* 1.0 은 Host 없어도 OK */
}

static void test_bad_headers(void)
{
    CHECK(parse_only("GET / HTTP/1.1\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);                 /* 1.1 인데 Host 없음 */
    CHECK(parse_only("GET / HTTP/1.1\r\nHost a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);       /* 콜론 없음 */
    CHECK(parse_only("GET / HTTP/1.1\r\n: a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);          /* 이름 없음 */
    CHECK(parse_only("GET / HTTP/1.1\r\nHost : a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);     /* 콜론 앞 공백 */
    CHECK(parse_only("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n") == HTTP_PARSE_BAD_REQUEST); /* obs-fold */
    CHECK(parse_only("GET / HTTP/1.1\nHost: a\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);        /* 단독 LF */
    CHECK(parse_only("GET / HTTP/1.1\r\nHost: a\rb\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);   /* 단독 CR */
}

static void test_embedded_nul(void)
{
    /* strlen 으로는 못 만드는 요청이라 직접 구성한다. */
    char raw[] = "GET / HTTP/1.1\r\nHost: a\0b\r\n\r\n";
    size_t len = sizeof(raw) - 1;
    long end = http_find_head_end(raw, len);
    CHECK(end == (long)len);
    struct http_request req;
    CHECK(http_parse_request(raw, (size_t)end, &req) == HTTP_PARSE_BAD_REQUEST);
}

static void test_too_many_headers(void)
{
    char raw[4096];
    size_t off = (size_t)snprintf(raw, sizeof(raw), "GET / HTTP/1.1\r\nHost: a\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS; i++) /* Host 포함해서 MAX+1 개 */
        off += (size_t)snprintf(raw + off, sizeof(raw) - off, "X-%d: v\r\n", i);
    snprintf(raw + off, sizeof(raw) - off, "\r\n");
    CHECK(parse_only(raw) == HTTP_PARSE_TOO_MANY_HEADERS);

    /* 딱 MAX 개는 OK */
    off = (size_t)snprintf(raw, sizeof(raw), "GET / HTTP/1.1\r\nHost: a\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS - 1; i++)
        off += (size_t)snprintf(raw + off, sizeof(raw) - off, "X-%d: v\r\n", i);
    snprintf(raw + off, sizeof(raw) - off, "\r\n");
    CHECK(parse_only(raw) == HTTP_PARSE_OK);
}

static int content_length_of(const char *value_line, size_t *out)
{
    char raw[256];
    snprintf(raw, sizeof(raw), "POST / HTTP/1.1\r\nHost: a\r\n%s\r\n", value_line);
    struct http_request req;
    char *buf;
    int rc = -2;
    if (parse(raw, &req, &buf) == HTTP_PARSE_OK)
        rc = http_content_length(&req, out);
    free(buf);
    return rc;
}

static void test_content_length_validation(void)
{
    size_t v;
    CHECK(content_length_of("Content-Length: 0\r\n", &v) == 0 && v == 0);
    CHECK(content_length_of("content-length: 42\r\n", &v) == 0 && v == 42);
    CHECK(content_length_of("Content-Length: abc\r\n", &v) == -1);
    CHECK(content_length_of("Content-Length: -1\r\n", &v) == -1);
    CHECK(content_length_of("Content-Length: 1 2\r\n", &v) == -1);
    CHECK(content_length_of("Content-Length:\r\n", &v) == -1);
    CHECK(content_length_of("Content-Length: 99999999999999999999999999\r\n", &v) == -1);
    CHECK(content_length_of("Content-Length: 5\r\nContent-Length: 5\r\n", &v) == -1);
}

static void test_response_head(void)
{
    char out[256];
    int n = http_build_response_head(out, sizeof(out), 404, "text/plain", 10, NULL);
    CHECK(n > 0);
    CHECK_STR(out,
              "HTTP/1.1 404 Not Found\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: 10\r\n"
              "Connection: close\r\n"
              "\r\n");

    n = http_build_response_head(out, sizeof(out), 405, "text/plain", 0, "Allow: GET, POST\r\n");
    CHECK(n > 0 && strstr(out, "HTTP/1.1 405 Method Not Allowed\r\n") == out);
    CHECK(strstr(out, "Allow: GET, POST\r\n\r\n") != NULL);

    char tiny[16];
    CHECK(http_build_response_head(tiny, sizeof(tiny), 200, "text/plain", 0, NULL) == -1);

    CHECK_STR(http_reason(505), "HTTP Version Not Supported");
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "find_head_end",            test_find_head_end },
        { "simple_get",               test_simple_get },
        { "post_content_length",      test_post_content_length },
        { "empty_header_value",       test_empty_header_value },
        { "bad_request_line",         test_bad_request_line },
        { "bad_version",              test_bad_version },
        { "bad_headers",              test_bad_headers },
        { "embedded_nul",             test_embedded_nul },
        { "too_many_headers",         test_too_many_headers },
        { "content_length_validation", test_content_length_validation },
        { "response_head",            test_response_head },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failed;
        tests[i].fn();
        printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
