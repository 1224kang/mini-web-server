/* src/static_files.c 단위 테스트: 파일 시스템 없이 경로 문자열만 검사한다.
 * 실행: make test */
#include "static_files.h"

#include <stdio.h>
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

#define ROOT "/srv/www"

/* target 을 풀어서 결과 코드를 돌려주고, 성공이면 out 에 경로를 남긴다. */
static enum static_resolve_result resolve(const char *target, char *out, int *slash)
{
    return static_resolve_path(ROOT, target, out, STATIC_MAX_PATH, slash);
}

/* 성공해야 하고, 결과 경로가 expect 여야 한다. */
static int resolves_to(const char *target, const char *expect)
{
    char out[STATIC_MAX_PATH];
    int slash;
    return resolve(target, out, &slash) == STATIC_OK && strcmp(out, expect) == 0;
}

static enum static_resolve_result code_of(const char *target)
{
    char out[STATIC_MAX_PATH];
    int slash;
    return resolve(target, out, &slash);
}

static void test_normal_paths(void)
{
    CHECK(resolves_to("/", ROOT));
    CHECK(resolves_to("/index.html", ROOT "/index.html"));
    CHECK(resolves_to("/css/style.css", ROOT "/css/style.css"));
    CHECK(resolves_to("/a//b///c", ROOT "/a/b/c"));       /* 연속 슬래시 */
    CHECK(resolves_to("/./a/./b", ROOT "/a/b"));          /* "." 무시 */
    CHECK(resolves_to("/a/b/../c", ROOT "/a/c"));         /* 안쪽에서의 ".." 은 허용 */
    CHECK(resolves_to("/a/..", ROOT));                    /* 루트로 돌아오는 것까지는 OK */
    CHECK(resolves_to("/..a/b..", ROOT "/..a/b.."));      /* ".." 으로 "시작"하는 이름은 그냥 이름 */
}

static void test_query_and_fragment(void)
{
    CHECK(resolves_to("/app.js?v=3", ROOT "/app.js"));
    CHECK(resolves_to("/page.html#top", ROOT "/page.html"));
    CHECK(resolves_to("/x?../../etc/passwd", ROOT "/x")); /* 쿼리 안의 ".." 은 경로가 아니다 */
}

static void test_percent_decoding(void)
{
    CHECK(resolves_to("/hello%20world.txt", ROOT "/hello world.txt"));
    CHECK(resolves_to("/%E1%84%92.txt", ROOT "/\xE1\x84\x92.txt")); /* UTF-8 */
    CHECK(code_of("/bad%2") == STATIC_BAD_REQUEST);    /* % 뒤 글자 모자람 */
    CHECK(code_of("/bad%zz") == STATIC_BAD_REQUEST);   /* 16진수 아님 */
    CHECK(code_of("/a.html%00.png") == STATIC_BAD_REQUEST); /* NUL 끼워 넣기 */
}

static void test_traversal_blocked(void)
{
    CHECK(code_of("/..") == STATIC_FORBIDDEN);
    CHECK(code_of("/../etc/passwd") == STATIC_FORBIDDEN);
    CHECK(code_of("/a/../../etc/passwd") == STATIC_FORBIDDEN);
    CHECK(code_of("/%2e%2e/etc/passwd") == STATIC_FORBIDDEN);     /* 인코딩된 ".." */
    CHECK(code_of("/%2E%2e%2Fetc%2fpasswd") == STATIC_FORBIDDEN); /* 인코딩된 "../" */
    CHECK(code_of("/.%2e/secret") == STATIC_FORBIDDEN);           /* 섞어서 */
    CHECK(code_of("//../etc") == STATIC_FORBIDDEN);
}

static void test_bad_targets(void)
{
    CHECK(code_of("index.html") == STATIC_BAD_REQUEST);          /* '/' 로 시작 안 함 */
    CHECK(code_of("http://example.com/") == STATIC_BAD_REQUEST); /* 절대 URI (6+단계) */
    CHECK(code_of("*") == STATIC_BAD_REQUEST);
}

static void test_trailing_slash(void)
{
    char out[STATIC_MAX_PATH];
    int slash = -1;
    CHECK(resolve("/docs/", out, &slash) == STATIC_OK && slash == 1);
    CHECK(resolve("/docs", out, &slash) == STATIC_OK && slash == 0);
    CHECK(resolve("/docs/?q=1", out, &slash) == STATIC_OK && slash == 1);
    CHECK(resolve("/", out, &slash) == STATIC_OK && slash == 1);
}

static void test_too_long(void)
{
    char out[32];
    int slash;
    /* ROOT(8) + "/abcdefghijklmnopqrstuvwxyz"(27) + NUL > 32 */
    CHECK(static_resolve_path(ROOT, "/abcdefghijklmnopqrstuvwxyz", out, sizeof(out), &slash)
          == STATIC_TOO_LONG);
    CHECK(static_resolve_path(ROOT, "/short", out, sizeof(out), &slash) == STATIC_OK);

    static char huge[STATIC_MAX_PATH + 10];
    memset(huge, 'a', sizeof(huge) - 1);
    huge[0] = '/';
    CHECK(code_of(huge) == STATIC_TOO_LONG);
}

static void test_mime(void)
{
    CHECK(strcmp(static_mime_type("/r/index.html"), "text/html; charset=utf-8") == 0);
    CHECK(strcmp(static_mime_type("/r/STYLE.CSS"), "text/css; charset=utf-8") == 0); /* 대소문자 무시 */
    CHECK(strcmp(static_mime_type("/r/app.js"), "text/javascript; charset=utf-8") == 0);
    CHECK(strcmp(static_mime_type("/r/a.min.js"), "text/javascript; charset=utf-8") == 0); /* 마지막 점 */
    CHECK(strcmp(static_mime_type("/r/logo.png"), "image/png") == 0);
    CHECK(strcmp(static_mime_type("/r/photo.JPEG"), "image/jpeg") == 0);
    CHECK(strcmp(static_mime_type("/r/Makefile"), "application/octet-stream") == 0);  /* 확장자 없음 */
    CHECK(strcmp(static_mime_type("/r/v1.2/README"), "application/octet-stream") == 0); /* 디렉토리 점 */
    CHECK(strcmp(static_mime_type("/r/.html"), "application/octet-stream") == 0);  /* 숨김 파일 */
    CHECK(strcmp(static_mime_type("/r/x.unknown"), "application/octet-stream") == 0);
}

static void test_is_under(void)
{
    CHECK(static_is_under("/srv/www", "/srv/www") == 1);
    CHECK(static_is_under("/srv/www", "/srv/www/a.html") == 1);
    CHECK(static_is_under("/srv/www", "/srv/www-secret/a") == 0); /* 앞부분만 같음 */
    CHECK(static_is_under("/srv/www", "/srv") == 0);
    CHECK(static_is_under("/srv/www", "/etc/passwd") == 0);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "normal_paths",       test_normal_paths },
        { "query_and_fragment", test_query_and_fragment },
        { "percent_decoding",   test_percent_decoding },
        { "traversal_blocked",  test_traversal_blocked },
        { "bad_targets",        test_bad_targets },
        { "trailing_slash",     test_trailing_slash },
        { "too_long",           test_too_long },
        { "mime",               test_mime },
        { "is_under",           test_is_under },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failed;
        tests[i].fn();
        printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
