/*
 * tests/test_parser.c - 단위 테스트 (외부 테스트 프레임워크 없이 assert 매크로로 작성)
 *
 *   make test        : 일반 빌드로 실행
 *   make test-asan   : AddressSanitizer + LeakSanitizer 로 실행
 *   make valgrind    : valgrind 로 실행
 *
 * 다루는 범위
 *   - http_parse_head      : 요청 줄 / 헤더 / 에러 코드 / keep-alive 판정 (소켓 없이)
 *   - http_read_request    : socketpair 로 Partial Read, 파이프라이닝, 본문, 타임아웃, 100-continue
 *   - static_mime_type, http_path_has_dotdot, http_header_has_token
 *   - thread_pool          : 모든 작업 실행, 큐 가득 참(FULL)
 *   - proxy                : Round-Robin 순서 / 동시성, 최장 prefix 매칭
 *   - config               : 백엔드 주소 파싱, 설정 파일, CLI 라우팅 규칙
 */
#include "config.h"
#include "http.h"
#include "proxy_handler.h"
#include "static_handler.h"
#include "thread_pool.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* 미니 테스트 프레임워크                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (cond) {                                                                   \
            g_pass++;                                                                 \
        } else {                                                                      \
            g_fail++;                                                                 \
            fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                             \
    } while (0)

#define CHECK_INT(actual, expected)                                                   \
    do {                                                                              \
        long a_ = (long)(actual), e_ = (long)(expected);                              \
        if (a_ == e_) {                                                               \
            g_pass++;                                                                 \
        } else {                                                                      \
            g_fail++;                                                                 \
            fprintf(stderr, "    FAIL %s:%d: %s = %ld, expected %ld\n",               \
                    __FILE__, __LINE__, #actual, a_, e_);                             \
        }                                                                             \
    } while (0)

#define CHECK_STR(actual, expected)                                                   \
    do {                                                                              \
        const char *a_ = (actual), *e_ = (expected);                                  \
        if (a_ != NULL && strcmp(a_, e_) == 0) {                                      \
            g_pass++;                                                                 \
        } else {                                                                      \
            g_fail++;                                                                 \
            fprintf(stderr, "    FAIL %s:%d: %s = \"%s\", expected \"%s\"\n",         \
                    __FILE__, __LINE__, #actual, a_ ? a_ : "(null)", e_);             \
        }                                                                             \
    } while (0)

#define RUN(fn)                                       \
    do {                                              \
        int before_ = g_fail;                         \
        fn();                                         \
        printf("  %-4s %s\n", g_fail == before_ ? "ok" : "FAIL", #fn); \
    } while (0)

/* 요청 구조체는 13KB 정도라 정적으로 둔다 */
static http_request_t R;
static http_conn_t    C;

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int parse(const char *raw)
{
    http_request_init(&R);
    return http_parse_head(&R, raw, strlen(raw));
}

/* ------------------------------------------------------------------ */
/* 1. 순수 파서                                                          */
/* ------------------------------------------------------------------ */

static void test_parse_basic_get(void)
{
    CHECK_INT(parse("GET /index.html HTTP/1.1\r\nHost: localhost\r\nUser-Agent: t\r\n\r\n"), 0);
    CHECK_INT(R.method, HTTP_METHOD_GET);
    CHECK_STR(R.method_str, "GET");
    CHECK_STR(R.target, "/index.html");
    CHECK_STR(R.path, "/index.html");
    CHECK_STR(R.query, "");
    CHECK_INT(R.version_major, 1);
    CHECK_INT(R.version_minor, 1);
    CHECK_INT(R.header_count, 2);
    CHECK_STR(http_get_header(&R, "host"), "localhost");          /* 대소문자 무시 */
    CHECK_STR(http_get_header(&R, "USER-AGENT"), "t");
    CHECK(http_get_header(&R, "Accept") == NULL);
    CHECK_INT(R.keep_alive, 1);                                   /* 1.1 기본 keep-alive */
}

static void test_parse_post_and_query(void)
{
    CHECK_INT(parse("POST /a%20b.html?x=1&y=%41 HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\n"), 0);
    CHECK_INT(R.method, HTTP_METHOD_POST);
    CHECK_STR(R.target, "/a%20b.html?x=1&y=%41");                 /* 원본 보존 */
    CHECK_STR(R.path, "/a b.html");                               /* path 만 디코딩 */
    CHECK_STR(R.query, "x=1&y=%41");                              /* query 는 그대로 */
}

static void test_parse_header_whitespace(void)
{
    CHECK_INT(parse("GET / HTTP/1.1\r\nHost:    spaced.example   \r\nX-Tab:\tv\t\r\n\r\n"), 0);
    CHECK_STR(http_get_header(&R, "Host"), "spaced.example");
    CHECK_STR(http_get_header(&R, "X-Tab"), "v");
}

static void test_parse_unknown_method(void)
{
    CHECK_INT(parse("PUT / HTTP/1.1\r\nHost: h\r\n\r\n"), 0);      /* 문법은 OK -> 405 는 라우터 몫 */
    CHECK_INT(R.method, HTTP_METHOD_UNKNOWN);
    CHECK_INT(parse("get / HTTP/1.1\r\nHost: h\r\n\r\n"), 0);      /* 대소문자 구분 */
    CHECK_INT(R.method, HTTP_METHOD_UNKNOWN);
}

static void test_parse_errors(void)
{
    static char long_uri[HTTP_MAX_URI_LEN + 64];
    static char long_uri_req[HTTP_MAX_URI_LEN + 128];
    memset(long_uri, 'a', sizeof(long_uri) - 1);
    long_uri[0] = '/';
    long_uri[sizeof(long_uri) - 1] = '\0';
    snprintf(long_uri_req, sizeof(long_uri_req), "GET %s HTTP/1.1\r\nHost: h\r\n\r\n", long_uri);

    static const struct { const char *raw; int expect; } cases[] = {
        { "GARBAGE\r\n\r\n",                                         400 },
        { "GET / HTTP/1.1\r\n\r\n",                                  400 },  /* Host 없음 */
        { "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",            400 },  /* Host 중복 */
        { "GET / HTTP/2.0\r\nHost: h\r\n\r\n",                       505 },
        { "GET / HTTX/1.1\r\nHost: h\r\n\r\n",                       400 },
        { "GET / HTTP/1.1 \r\nHost: h\r\n\r\n",                      400 },  /* 버전 뒤 공백 */
        { "GET  / HTTP/1.1\r\nHost: h\r\n\r\n",                      400 },  /* 공백 2개 */
        { "GET http://x/ HTTP/1.1\r\nHost: h\r\n\r\n",               400 },  /* absolute-form */
        { "GET /a%zz HTTP/1.1\r\nHost: h\r\n\r\n",                   400 },
        { "GET /a%2 HTTP/1.1\r\nHost: h\r\n\r\n",                    400 },
        { "GET /a%00b HTTP/1.1\r\nHost: h\r\n\r\n",                  400 },  /* NUL 주입 */
        { "GET /a%0d%0aX:1 HTTP/1.1\r\nHost: h\r\n\r\n",             400 },  /* 헤더 주입 */
        { "GET / HTTP/1.1\r\nHost: h\r\nNoColon\r\n\r\n",            400 },
        { "GET / HTTP/1.1\r\nHost : h\r\n\r\n",                      400 },  /* 이름 뒤 공백 */
        { "GET / HTTP/1.1\r\nHost: h\r\nX: 1\r\n folded\r\n\r\n",    400 },  /* obs-fold */
        { "G\x01T / HTTP/1.1\r\nHost: h\r\n\r\n",                    400 },  /* 메서드에 제어문자 */
        { "GET / HTTP/1.1\r\nHost: h\x7f\r\n\r\n",                   400 },  /* 값에 DEL */
        { "VERYLONGMETHODNAME / HTTP/1.1\r\nHost: h\r\n\r\n",        400 },
        { "GET / HTTP/1.0\r\n\r\n",                                    0 },  /* 1.0 은 Host 선택 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = parse(cases[i].raw);
        if (got != cases[i].expect)
            fprintf(stderr, "    case %zu: %s", i, cases[i].raw);
        CHECK_INT(got, cases[i].expect);
    }
    CHECK_INT(parse(long_uri_req), 414);

    /* NUL 바이트가 끼어든 헤더 블록 */
    static const char with_nul[] = "GET / HTTP/1.1\r\nHost: h\0x\r\n\r\n";
    http_request_init(&R);
    CHECK_INT(http_parse_head(&R, with_nul, sizeof(with_nul) - 1), 400);
}

static void test_parse_too_many_headers(void)
{
    static char buf[HTTP_MAX_HEADER_SIZE];
    size_t len = (size_t)snprintf(buf, sizeof(buf), "GET / HTTP/1.1\r\nHost: h\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS + 1; i++)
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "X-%d: v\r\n", i);
    snprintf(buf + len, sizeof(buf) - len, "\r\n");
    CHECK_INT(parse(buf), 413);
}

static void test_keep_alive_rules(void)
{
    parse("GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK_INT(R.keep_alive, 1);
    parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    CHECK_INT(R.keep_alive, 0);
    parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: Upgrade, CLOSE\r\n\r\n");
    CHECK_INT(R.keep_alive, 0);
    parse("GET / HTTP/1.0\r\n\r\n");
    CHECK_INT(R.keep_alive, 0);
    parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
    CHECK_INT(R.keep_alive, 1);
}

static void test_header_token_and_dotdot(void)
{
    CHECK(http_header_has_token("keep-alive", "keep-alive"));
    CHECK(http_header_has_token(" Upgrade ,  Keep-Alive ", "keep-alive"));
    CHECK(!http_header_has_token("keep-alive-ish", "keep-alive"));
    CHECK(!http_header_has_token(NULL, "close"));
    CHECK(!http_header_has_token("", "close"));

    CHECK(http_path_has_dotdot("/.."));
    CHECK(http_path_has_dotdot("/a/../b"));
    CHECK(http_path_has_dotdot("/a/.."));
    CHECK(http_path_has_dotdot("//..//x"));
    CHECK(!http_path_has_dotdot("/"));
    CHECK(!http_path_has_dotdot("/a..b"));
    CHECK(!http_path_has_dotdot("/..hidden/x"));
    CHECK(!http_path_has_dotdot("/./a"));
}

static void test_mime_types(void)
{
    CHECK_STR(static_mime_type("/www/index.html"), "text/html; charset=utf-8");
    CHECK_STR(static_mime_type("/x/STYLE.CSS"), "text/css; charset=utf-8");
    CHECK_STR(static_mime_type("a.js"), "text/javascript; charset=utf-8");
    CHECK_STR(static_mime_type("a.json"), "application/json");
    CHECK_STR(static_mime_type("a.png"), "image/png");
    CHECK_STR(static_mime_type("a.JPG"), "image/jpeg");
    CHECK_STR(static_mime_type("a.jpeg"), "image/jpeg");
    CHECK_STR(static_mime_type("a.gif"), "image/gif");
    CHECK_STR(static_mime_type("a.svg"), "image/svg+xml");
    CHECK_STR(static_mime_type("favicon.ico"), "image/x-icon");
    CHECK_STR(static_mime_type("a.bin"), "application/octet-stream");
    CHECK_STR(static_mime_type("noext"), "application/octet-stream");
    CHECK_STR(static_mime_type("/v1.2/file"), "application/octet-stream"); /* 디렉토리의 점 무시 */
    CHECK_STR(static_mime_type("trailingdot."), "application/octet-stream");
}

static void test_status_text(void)
{
    CHECK_STR(http_status_text(200), "OK");
    CHECK_STR(http_status_text(404), "Not Found");
    CHECK_STR(http_status_text(408), "Request Timeout");
    CHECK_STR(http_status_text(502), "Bad Gateway");
    CHECK_STR(http_status_text(503), "Service Unavailable");
    CHECK_STR(http_status_text(504), "Gateway Timeout");
    CHECK_STR(http_status_text(799), "Unknown");
}

/* ------------------------------------------------------------------ */
/* 2. 소켓 위에서 http_read_request (socketpair)                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int          fd;
    const char  *pieces[8];
    int          delay_ms;
    int          close_after;
} writer_arg_t;

/* 조각을 하나씩, 사이사이 쉬면서 보낸다 -> 수신 측은 Partial Read 를 겪는다 */
static void *writer_main(void *arg)
{
    writer_arg_t *w = arg;
    for (int i = 0; i < 8 && w->pieces[i] != NULL; i++) {
        if (send(w->fd, w->pieces[i], strlen(w->pieces[i]), 0) < 0)
            break;
        sleep_ms(w->delay_ms);
    }
    if (w->close_after)
        shutdown(w->fd, SHUT_WR);
    return NULL;
}

static void set_rcvtimeo_ms(int fd, int ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void test_read_partial_pieces(void)
{
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    writer_arg_t w = { .fd = sv[1], .delay_ms = 20, .close_after = 0,
                       .pieces = { "PO", "ST /submit HT", "TP/1.1\r\nHost: x\r\nContent-Le",
                                   "ngth: 10\r\n", "\r\n01234", "56789", NULL } };
    pthread_t th;
    pthread_create(&th, NULL, writer_main, &w);

    http_conn_init(&C, sv[0]);
    http_request_init(&R);
    CHECK_INT(http_read_request(&C, &R), HTTP_READ_OK);
    CHECK_INT(R.method, HTTP_METHOD_POST);
    CHECK_STR(R.path, "/submit");
    CHECK_INT(R.body_len, 10);
    CHECK(R.body != NULL && memcmp(R.body, "0123456789", 10) == 0);
    http_request_free(&R);

    pthread_join(th, NULL);
    close(sv[0]);
    close(sv[1]);
}

static void test_read_pipelined_then_eof(void)
{
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    /* 요청 두 개를 한 번에 보낸 뒤 닫는다 -> 두 번 읽고, 세 번째는 EOF */
    const char *two = "GET /one HTTP/1.1\r\nHost: x\r\n\r\n"
                      "POST /two HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi";
    CHECK(send(sv[1], two, strlen(two), 0) == (ssize_t)strlen(two));
    shutdown(sv[1], SHUT_WR);

    http_conn_init(&C, sv[0]);

    http_request_init(&R);
    CHECK_INT(http_read_request(&C, &R), HTTP_READ_OK);
    CHECK_STR(R.path, "/one");
    CHECK(C.len > 0);                                   /* 두 번째 요청이 버퍼에 남아 있음 */
    http_request_free(&R);

    http_request_init(&R);
    CHECK_INT(http_read_request(&C, &R), HTTP_READ_OK);
    CHECK_STR(R.path, "/two");
    CHECK_INT(R.body_len, 2);
    CHECK_STR(R.body, "hi");
    http_request_free(&R);

    http_request_init(&R);
    CHECK_INT(http_read_request(&C, &R), HTTP_READ_EOF);
    http_request_free(&R);

    close(sv[0]);
    close(sv[1]);
}

/* raw 를 보내고 닫은 뒤 http_read_request 결과 */
static int read_result_of(const char *raw, size_t len, int close_writer, int timeout_ms)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -999;
    if (len > 0 && send(sv[1], raw, len, 0) != (ssize_t)len) {
        close(sv[0]);
        close(sv[1]);
        return -998;
    }
    if (close_writer)
        shutdown(sv[1], SHUT_WR);
    if (timeout_ms > 0)
        set_rcvtimeo_ms(sv[0], timeout_ms);

    http_conn_init(&C, sv[0]);
    http_request_init(&R);
    int rc = http_read_request(&C, &R);
    http_request_free(&R);
    close(sv[0]);
    close(sv[1]);
    return rc;
}

static void test_read_errors(void)
{
    static char big[HTTP_MAX_HEADER_SIZE + 512];

    /* 헤더 9KB (줄바꿈 있음) -> 413 */
    int n = snprintf(big, sizeof(big), "GET / HTTP/1.1\r\nHost: x\r\nX-Big: ");
    memset(big + n, 'a', sizeof(big) - (size_t)n - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK_INT(read_result_of(big, strlen(big), 1, 0), 413);

    /* 줄바꿈 없는 9KB -> 요청 줄이 너무 김 -> 414 */
    memset(big, 'a', sizeof(big) - 1);
    memcpy(big, "GET /", 5);
    CHECK_INT(read_result_of(big, strlen(big), 1, 0), 414);

    const char *chunked = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK_INT(read_result_of(chunked, strlen(chunked), 1, 0), 501);

    const char *te_cl = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK_INT(read_result_of(te_cl, strlen(te_cl), 1, 0), 400);

    const char *cl_conflict = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab";
    CHECK_INT(read_result_of(cl_conflict, strlen(cl_conflict), 1, 0), 400);

    const char *cl_bad = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n\r\n";
    CHECK_INT(read_result_of(cl_bad, strlen(cl_bad), 1, 0), 400);

    const char *cl_huge = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n";
    CHECK_INT(read_result_of(cl_huge, strlen(cl_huge), 1, 0), 413);

    const char *short_body = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nonly-a-bit";
    CHECK_INT(read_result_of(short_body, strlen(short_body), 1, 0), 400);

    const char *cut_head = "GET / HTTP/1.1\r\nHost: x\r\n";
    CHECK_INT(read_result_of(cut_head, strlen(cut_head), 1, 0), 400);

    CHECK_INT(read_result_of("", 0, 1, 0), HTTP_READ_EOF);

    const char *leading = "\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK_INT(read_result_of(leading, strlen(leading), 1, 0), HTTP_READ_OK);
}

static void test_read_timeouts(void)
{
    /* 아무것도 안 보냄 -> 조용히 닫을 대상 */
    CHECK_INT(read_result_of("", 0, 0, 100), HTTP_READ_TIMEOUT);
    /* 헤더를 보내다 멈춤 -> 408 */
    CHECK_INT(read_result_of("GET / HT", 8, 0, 100), 408);
    /* 본문을 보내다 멈춤 -> 408 */
    const char *partial_body = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n12";
    CHECK_INT(read_result_of(partial_body, strlen(partial_body), 0, 100), 408);
}

typedef struct {
    int  fd;
    char got[64];
} expect_client_t;

/* Expect 클라이언트: 헤더만 보내고 "100 Continue" 를 받은 뒤에 본문을 보낸다 */
static void *expect_client_main(void *arg)
{
    expect_client_t *c = arg;
    const char *head = "POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n";
    if (send(c->fd, head, strlen(head), 0) < 0)
        return NULL;
    ssize_t n = recv(c->fd, c->got, sizeof(c->got) - 1, 0);
    if (n > 0) {
        c->got[n] = '\0';
        if (send(c->fd, "hello", 5, 0) < 0)
            return NULL;
    }
    return NULL;
}

static void test_read_expect_continue(void)
{
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    set_rcvtimeo_ms(sv[0], 2000);
    set_rcvtimeo_ms(sv[1], 2000);

    expect_client_t c;
    memset(&c, 0, sizeof(c));
    c.fd = sv[1];
    pthread_t th;
    pthread_create(&th, NULL, expect_client_main, &c);

    http_conn_init(&C, sv[0]);
    http_request_init(&R);
    CHECK_INT(http_read_request(&C, &R), HTTP_READ_OK);
    CHECK_STR(R.body, "hello");
    http_request_free(&R);

    pthread_join(th, NULL);
    CHECK_STR(c.got, "HTTP/1.1 100 Continue\r\n\r\n");
    close(sv[0]);
    close(sv[1]);
}

/* ------------------------------------------------------------------ */
/* 3. 스레드 풀                                                          */
/* ------------------------------------------------------------------ */

static atomic_int g_counter;

static void count_task(void *arg)
{
    (void)arg;
    atomic_fetch_add(&g_counter, 1);
}

static void test_thread_pool_runs_all(void)
{
    enum { N = 5000 };
    atomic_store(&g_counter, 0);

    thread_pool_t *pool = thread_pool_create(4, 64);
    CHECK(pool != NULL);
    if (pool == NULL)
        return;

    for (int i = 0; i < N; i++) {
        while (thread_pool_submit(pool, count_task, NULL) == THREAD_POOL_FULL)
            sched_yield();                         /* 큐가 차면 잠깐 양보 후 재시도 */
    }
    thread_pool_destroy(pool);                     /* 남은 작업까지 실행 후 join */
    CHECK_INT(atomic_load(&g_counter), N);
}

static pthread_mutex_t g_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gate_cond = PTHREAD_COND_INITIALIZER;
static int             g_gate_open = 0;
static atomic_int      g_blocker_started;

static void blocking_task(void *arg)
{
    (void)arg;
    atomic_store(&g_blocker_started, 1);
    pthread_mutex_lock(&g_gate_lock);
    while (!g_gate_open)
        pthread_cond_wait(&g_gate_cond, &g_gate_lock);
    pthread_mutex_unlock(&g_gate_lock);
    atomic_fetch_add(&g_counter, 1);
}

static void test_thread_pool_full(void)
{
    atomic_store(&g_counter, 0);
    atomic_store(&g_blocker_started, 0);
    g_gate_open = 0;

    thread_pool_t *pool = thread_pool_create(1, 2);   /* 워커 1, 큐 2 */
    CHECK(pool != NULL);
    if (pool == NULL)
        return;

    CHECK_INT(thread_pool_submit(pool, blocking_task, NULL), THREAD_POOL_OK);
    while (!atomic_load(&g_blocker_started))          /* 워커가 첫 작업을 꺼내 갈 때까지 */
        sched_yield();
    CHECK_INT(thread_pool_submit(pool, blocking_task, NULL), THREAD_POOL_OK);
    CHECK_INT(thread_pool_submit(pool, blocking_task, NULL), THREAD_POOL_OK);
    CHECK_INT(thread_pool_submit(pool, blocking_task, NULL), THREAD_POOL_FULL);

    pthread_mutex_lock(&g_gate_lock);
    g_gate_open = 1;
    pthread_cond_broadcast(&g_gate_cond);
    pthread_mutex_unlock(&g_gate_lock);

    thread_pool_destroy(pool);
    CHECK_INT(atomic_load(&g_counter), 3);
}

/* ------------------------------------------------------------------ */
/* 4. 프록시 라우팅 / Round-Robin                                         */
/* ------------------------------------------------------------------ */

static server_config_t G_CFG;
static proxy_table_t   G_TABLE;

typedef struct {
    proxy_route_t *route;
    int            counts[CONFIG_MAX_BACKENDS];
} rr_worker_t;

static void *rr_worker_main(void *arg)
{
    rr_worker_t *w = arg;
    for (int i = 0; i < 3000; i++)
        w->counts[proxy_next_backend(w->route)]++;
    return NULL;
}

static void test_proxy_routing(void)
{
    config_set_defaults(&G_CFG);
    const char *api[]   = { "127.0.0.1:8001", "127.0.0.1:8002", "127.0.0.1:8003" };
    const char *apiv2[] = { "127.0.0.1:9001" };
    CHECK_INT(config_add_route(&G_CFG, "/api/", api, 3), 0);
    CHECK_INT(config_add_route(&G_CFG, "/api/v2/", apiv2, 1), 0);
    CHECK_INT(proxy_table_init(&G_TABLE, &G_CFG), 0);

    /* 최장 prefix 매칭 */
    proxy_route_t *r_api = proxy_match(&G_TABLE, "/api/users");
    proxy_route_t *r_v2  = proxy_match(&G_TABLE, "/api/v2/users");
    CHECK(r_api != NULL && strcmp(r_api->prefix, "/api/") == 0);
    CHECK(r_v2 != NULL && strcmp(r_v2->prefix, "/api/v2/") == 0);
    CHECK(proxy_match(&G_TABLE, "/api") == NULL);
    CHECK(proxy_match(&G_TABLE, "/apix/") == NULL);
    CHECK(proxy_match(&G_TABLE, "/index.html") == NULL);

    /* 순서: 0,1,2,0,1,2 */
    if (r_api != NULL) {
        for (int i = 0; i < 6; i++)
            CHECK_INT(proxy_next_backend(r_api), i % 3);

        /* 동시성: 8 스레드 x 3000 회 -> 정확히 균등 분배 (락이 없으면 깨진다) */
        rr_worker_t w[8];
        pthread_t   th[8];
        memset(w, 0, sizeof(w));
        for (int i = 0; i < 8; i++) {
            w[i].route = r_api;
            pthread_create(&th[i], NULL, rr_worker_main, &w[i]);
        }
        int total[3] = { 0, 0, 0 };
        for (int i = 0; i < 8; i++) {
            pthread_join(th[i], NULL);
            for (int b = 0; b < 3; b++)
                total[b] += w[i].counts[b];
        }
        CHECK_INT(total[0], 8000);
        CHECK_INT(total[1], 8000);
        CHECK_INT(total[2], 8000);
    }
    proxy_table_destroy(&G_TABLE);
}

/* ------------------------------------------------------------------ */
/* 5. 설정                                                              */
/* ------------------------------------------------------------------ */

static void test_config_backend_parse(void)
{
    backend_addr_t b;
    CHECK_INT(config_parse_backend("127.0.0.1:8001", &b), 0);
    CHECK_INT(ntohs(b.addr.sin_port), 8001);
    CHECK_STR(b.text, "127.0.0.1:8001");
    CHECK_INT(config_parse_backend("localhost:80", &b), 0);
    CHECK_INT(config_parse_backend("127.0.0.1", &b), -1);
    CHECK_INT(config_parse_backend(":80", &b), -1);
    CHECK_INT(config_parse_backend("127.0.0.1:0", &b), -1);
    CHECK_INT(config_parse_backend("127.0.0.1:70000", &b), -1);
    CHECK_INT(config_parse_backend("127.0.0.1:80x", &b), -1);
}

static int write_temp_config(char *path, size_t cap, const char *content)
{
    snprintf(path, cap, "/tmp/mws_test_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    size_t len = strlen(content);
    ssize_t n = write(fd, content, len);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static void test_config_file(void)
{
    char path[64];
    CHECK_INT(write_temp_config(path, sizeof(path),
                                "# comment line\n"
                                "port 9090   # trailing comment\n"
                                "root /tmp\n"
                                "\n"
                                "threads 3\n"
                                "queue_size 7\n"
                                "keepalive_timeout 2\n"
                                "request_timeout 4\n"
                                "max_keepalive_requests 9\n"
                                "proxy_connect_timeout 1\n"
                                "proxy_timeout 6\n"
                                "route /api/ 127.0.0.1:8001 127.0.0.1:8002\n"), 0);
    config_set_defaults(&G_CFG);
    CHECK_INT(config_load_file(&G_CFG, path), 0);
    unlink(path);
    CHECK_INT(G_CFG.port, 9090);
    CHECK_STR(G_CFG.doc_root, "/tmp");
    CHECK_INT(G_CFG.threads, 3);
    CHECK_INT(G_CFG.queue_size, 7);
    CHECK_INT(G_CFG.keepalive_timeout_sec, 2);
    CHECK_INT(G_CFG.request_timeout_sec, 4);
    CHECK_INT(G_CFG.max_keepalive_requests, 9);
    CHECK_INT(G_CFG.proxy_connect_timeout_sec, 1);
    CHECK_INT(G_CFG.proxy_timeout_sec, 6);
    CHECK_INT(G_CFG.route_count, 1);
    CHECK_INT(G_CFG.routes[0].backend_count, 2);

    /* CLI 규칙이 같은 prefix 를 덮어쓰기 */
    CHECK_INT(config_add_route_spec(&G_CFG, "/api/=127.0.0.1:7001"), 0);
    CHECK_INT(G_CFG.route_count, 1);
    CHECK_INT(G_CFG.routes[0].backend_count, 1);
    CHECK_INT(config_add_route_spec(&G_CFG, "/img/=127.0.0.1:7002,127.0.0.1:7003"), 0);
    CHECK_INT(G_CFG.route_count, 2);
    CHECK_INT(config_add_route_spec(&G_CFG, "no-equals-sign"), -1);
    CHECK_INT(config_add_route_spec(&G_CFG, "relative/=127.0.0.1:1"), -1);

    /* 잘못된 설정 파일들 (에러 메시지가 stderr 에 출력되는 것이 정상) */
    static const char *bad[] = {
        "unknown_key 1\n",
        "port 0\n",
        "port 8080 8081\n",
        "threads abc\n",
        "route /api/\n",
        "route /api/ not-an-address\n",
    };
    fprintf(stderr, "    (expected config errors below)\n");
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_INT(write_temp_config(path, sizeof(path), bad[i]), 0);
        config_set_defaults(&G_CFG);
        CHECK_INT(config_load_file(&G_CFG, path), -1);
        unlink(path);
    }
    config_set_defaults(&G_CFG);
    CHECK_INT(config_load_file(&G_CFG, "/nonexistent/mws.conf"), -1);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("mini-web-server unit tests\n");

    RUN(test_parse_basic_get);
    RUN(test_parse_post_and_query);
    RUN(test_parse_header_whitespace);
    RUN(test_parse_unknown_method);
    RUN(test_parse_errors);
    RUN(test_parse_too_many_headers);
    RUN(test_keep_alive_rules);
    RUN(test_header_token_and_dotdot);
    RUN(test_mime_types);
    RUN(test_status_text);
    RUN(test_read_partial_pieces);
    RUN(test_read_pipelined_then_eof);
    RUN(test_read_errors);
    RUN(test_read_timeouts);
    RUN(test_read_expect_continue);
    RUN(test_thread_pool_runs_all);
    RUN(test_thread_pool_full);
    RUN(test_proxy_routing);
    RUN(test_config_backend_parse);
    RUN(test_config_file);

    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
