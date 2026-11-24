/*
 * http_parser.c - HTTP/1.1 요청 수신 및 파싱
 *
 * 흐름: http_read_request()
 *         ├─ (1) \r\n\r\n 이 나올 때까지 recv 를 반복해 conn->buf 에 누적 (Partial Read)
 *         ├─ (2) http_parse_head(): 요청 줄 + 헤더 파싱 (소켓과 무관한 순수 함수)
 *         └─ (3) read_body(): Content-Length 만큼 본문 수신
 */
#include "http.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

/* ------------------------------------------------------------------ */
/* 작은 유틸                                                           */
/* ------------------------------------------------------------------ */

/* 
    memmem 은 glibc 에서 _GNU_SOURCE 가 필요하므로 이식성을 위해 직접 구현 
    바이트 배열에서 패턴의 위치를 찾습니다
*/
static long find_bytes(const char *hay, size_t hay_len, const char *needle, size_t n_len)
{
    if (n_len == 0 || hay_len < n_len)
        return -1;
    for (size_t i = 0; i + n_len <= hay_len; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, n_len) == 0)
            return (long)i;
    }
    return -1;
}

/* RFC 7230 token 문자 (메서드, 헤더 이름) */
static int is_tchar(unsigned char c)
{
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

//'0'~'9', 'a'~'f', 'A'~'F'를 0~15로 바꾸고, 그 외에는 -1을 반환합니다.
static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "%2e%2E" 같은 퍼센트 인코딩 해제. 잘못된 %XX, 제어문자(%00, %0D%0A 등)는 거부. */
static int percent_decode(const char *src, size_t src_len, char *dst, size_t dst_cap)
{
    size_t j = 0;

    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (i + 2 >= src_len)          /* '%' 뒤에 16진수 2글자가 없음 */
                return -1;
            int hi = hex_value((unsigned char)src[i + 1]);
            int lo = hex_value((unsigned char)src[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;
            c = (unsigned char)(hi * 16 + lo);
            i += 2;
        }
        if (c < 0x20 || c == 0x7f)
            return -1;
        if (j + 1 >= dst_cap)
            return -1;
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
    return 0;
}

/* conn->buf 앞쪽 n 바이트를 소비(제거)하고 나머지를 앞으로 당긴다. */
static void conn_consume(http_conn_t *conn, size_t n)
{
    if (n >= conn->len) {
        conn->len = 0;
        return;
    }
    memmove(conn->buf, conn->buf + n, conn->len - n);
    conn->len -= n;
}

/* recv()를 감싸서 EINTR(시그널로 인한 중단)이면 자동으로 다시 시도합니다. */
static ssize_t recv_some(int fd, void *buf, size_t cap)
{
    for (;;) {
        ssize_t n = recv(fd, buf, cap, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

/* ------------------------------------------------------------------ */
/* 초기화 / 해제                                                       */
/* ------------------------------------------------------------------ */

/*
구조체를 빈 상태로 초기화합니다. 특히 body = NULL이 중요합니다. 
파싱 중간에 실패하더라도 http_request_free()의 free(NULL)은 안전하기 때문에, 
어느 경로로 빠져나가든 해제 코드는 같습니다.
*/
void http_conn_init(http_conn_t *conn, int fd)
{
    conn->fd  = fd;
    conn->len = 0;
}


void http_request_init(http_request_t *req)
{
    req->method        = HTTP_METHOD_UNKNOWN;
    req->method_str[0] = '\0';
    req->target[0]     = '\0';
    req->path[0]       = '\0';
    req->query[0]      = '\0';
    req->version_major = 0;
    req->version_minor = 0;
    req->header_count  = 0;
    req->head[0]       = '\0';
    req->head_len      = 0;
    req->body          = NULL;
    req->body_len      = 0;
}


/*
body만 해제합니다. 나머지는 구조체 안의 배열이라 해제할 게 없습니다. 
이 프로그램에서 요청마다 malloc하는 것은 body 하나뿐이라 누수 관리가 단순합니다.
*/
void http_request_free(http_request_t *req)
{
    free(req->body);
    req->body     = NULL;
    req->body_len = 0;
}

/*
대소문자를 무시하고(strcasecmp) 헤더를 찾습니다. 
HTTP 헤더 이름은 대소문자를 구분하지 않으므로 content-length와 Content-Length는 같은 헤더입니다.
*/
const char *http_get_header(const http_request_t *req, const char *name)
{
    for (size_t i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

//같은 이름의 헤더 개수를 셉니다. Host가 중복됐는지 검사할 때 씁니다.
static size_t count_header(const http_request_t *req, const char *name)
{
    size_t n = 0;
    for (size_t i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 요청 줄: METHOD SP request-target SP HTTP-version                   */
/* ------------------------------------------------------------------ */

//"GET /index.html HTTP/1.1"을 공백 기준으로 세 부분으로 나눕니다.
static int parse_request_line(http_request_t *req, char *line)
{
    /* 1) METHOD */
    /*
    strcmp로 GET/POST를 판별합니다. 
    대소문자를 구분하므로 get은 UNKNOWN이 되어 405입니다. 
    RFC상 메서드는 대소문자를 구분합니다.
    */
    char *sp1 = strchr(line, ' ');
    if (sp1 == NULL)
        return 400;
    size_t mlen = (size_t)(sp1 - line);
    if (mlen == 0 || mlen > HTTP_MAX_METHOD_LEN)
        return 400;
    for (size_t i = 0; i < mlen; i++) {
        if (!is_tchar((unsigned char)line[i]))
            return 400;
    }
    memcpy(req->method_str, line, mlen);
    req->method_str[mlen] = '\0';

    /* 메서드 이름은 대소문자를 구분한다 (RFC 7230 3.1.1) */
    if (strcmp(req->method_str, "GET") == 0)
        req->method = HTTP_METHOD_GET;
    else if (strcmp(req->method_str, "POST") == 0)
        req->method = HTTP_METHOD_POST;
    else
        req->method = HTTP_METHOD_UNKNOWN;

    /* 2) request-target */
    /*
    공백, 제어문자, DEL(0x7f)이 있으면 400입니다.
    /로 시작하지 않으면 400입니다. http://x/ 같은 absolute-form은 프록시용 형식이라 지원하지 않습니다.
    ?를 기준으로 path와 query를 나누고, path만 퍼센트 디코딩합니다.
    */
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (sp2 == NULL)
        return 400;
    size_t tlen = (size_t)(sp2 - target);
    if (tlen == 0)
        return 400;
    if (tlen > HTTP_MAX_URI_LEN)
        return 414;
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c <= 0x20 || c == 0x7f)        /* 공백/제어문자 금지 */
            return 400;
    }
    if (target[0] != '/')                  /* origin-form 만 지원 */
        return 400;
    memcpy(req->target, target, tlen);
    req->target[tlen] = '\0';

    /* path / query 분리 후 path 만 디코딩 */
    const char *q = strchr(req->target, '?');
    size_t raw_path_len = q ? (size_t)(q - req->target) : tlen;
    if (q != NULL) {
        strcpy(req->query, q + 1);         /* 길이 <= tlen 이므로 안전 */
    }
    if (percent_decode(req->target, raw_path_len, req->path, sizeof(req->path)) < 0)
        return 400;

    /* 3) HTTP-version : "HTTP/" DIGIT "." DIGIT */
    //정확히 HTTP/숫자.숫자 형식(8글자)이어야 합니다. 아니면 400입니다.
    const char *ver = sp2 + 1;
    if (strlen(ver) != 8 || strncmp(ver, "HTTP/", 5) != 0 ||
        ver[5] < '0' || ver[5] > '9' || ver[6] != '.' || ver[7] < '0' || ver[7] > '9')
        return 400;
    req->version_major = ver[5] - '0';
    req->version_minor = ver[7] - '0';
    if (req->version_major != 1)
        return 505;

    return 0;
}

/* ------------------------------------------------------------------ */
/* 헤더 줄: field-name ":" OWS field-value OWS                          */
/* ------------------------------------------------------------------ */
//헤더 한 줄 파싱
static int parse_header_line(http_request_t *req, char *line)
{
    /* obs-fold(앞이 공백인 줄 이어붙이기)는 RFC 7230 3.2.4 에 따라 거부 */
    if (*line == ' ' || *line == '\t')
        return 400;

    char *colon = strchr(line, ':');
    if (colon == NULL || colon == line)
        return 400;
    for (char *p = line; p < colon; p++) {
        if (!is_tchar((unsigned char)*p))  /* "Host : x" 처럼 이름 뒤 공백도 여기서 거부 */
            return 400;
    }
    *colon = '\0';

    char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
        value++;
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = '\0';

    for (char *p = value; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return 400;
    }

    if (req->header_count >= HTTP_MAX_HEADERS)
        return 413;
    req->headers[req->header_count].name  = line;
    req->headers[req->header_count].value = value;
    req->header_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 헤더 블록 파싱 (순수 함수)                                           */
/* ------------------------------------------------------------------ */
//순수 파서
int http_parse_head(http_request_t *req, const char *data, size_t len)
{
    if (len > HTTP_MAX_HEADER_SIZE)
        return 413;
    if (memchr(data, '\0', len) != NULL)   /* NUL 바이트가 끼면 문자열 함수가 오동작 */
        return 400;

    memcpy(req->head, data, len);
    req->head[len] = '\0';
    req->head_len  = len;
    req->header_count = 0;

    /* 요청 줄 */
    char *line = req->head;
    char *eol  = strstr(line, "\r\n");
    if (eol == NULL)
        return 400;
    *eol = '\0';
    int st = parse_request_line(req, line);
    if (st != 0)
        return st;

    /* 헤더 줄들 — 빈 줄(\r\n\r\n)을 만나면 끝 */
    char *p = eol + 2;
    int found_end = 0;
    while (*p != '\0') {
        eol = strstr(p, "\r\n");
        if (eol == NULL)
            return 400;
        if (eol == p) {
            found_end = 1;
            break;
        }
        *eol = '\0';
        st = parse_header_line(req, p);
        if (st != 0)
            return st;
        p = eol + 2;
    }
    if (!found_end)
        return 400;

    /* HTTP/1.1 요청은 Host 헤더가 정확히 1개 있어야 한다 (RFC 7230 5.4) */
    size_t hosts = count_header(req, "Host");
    if (hosts > 1 || (req->version_minor >= 1 && hosts == 0))
        return 400;

    return 0;
}

/* ------------------------------------------------------------------ */
/* 본문 (Content-Length)                                                */
/* ------------------------------------------------------------------ */

/* 숫자만 허용, 상한 초과 시 413. 성공 0 / 에러 상태 코드. */
static int parse_content_length(const char *s, size_t *out)
{
    size_t v = 0;

    if (*s == '\0')
        return 400;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return 400;
        v = v * 10 + (size_t)(*s - '0');
        if (v > (size_t)HTTP_MAX_BODY_SIZE)   /* 매 자리마다 검사 -> 오버플로 불가 */
            return 413;
    }
    *out = v;
    return 0;
}

//본문 수신
static int read_body(http_conn_t *conn, http_request_t *req)
{
    const char *te = http_get_header(req, "Transfer-Encoding");
    const char *cl = http_get_header(req, "Content-Length");

    /* TE + CL 동시 존재는 Request Smuggling 의 전형 -> 400 (RFC 7230 3.3.3) */
    if (te != NULL)
        return cl != NULL ? 400 : 501;    /* chunked 요청 본문은 미지원 */
    if (cl == NULL)
        return 0;                         /* 본문 없음 */

    /* Content-Length 가 여러 줄이면 값이 모두 같아야 한다 */
    for (size_t i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, "Content-Length") == 0 &&
            strcmp(req->headers[i].value, cl) != 0)
            return 400;
    }

    size_t len = 0;
    int st = parse_content_length(cl, &len);
    if (st != 0)
        return st;
    if (len == 0)
        return 0;

    req->body = malloc(len + 1);
    if (req->body == NULL)
        return 500;

    /* 헤더와 함께 이미 들어온 본문 조각을 먼저 가져온다 */
    size_t have = conn->len < len ? conn->len : len;
    memcpy(req->body, conn->buf, have);
    conn_consume(conn, have);

    /* "Expect: 100-continue" 클라이언트(curl 은 1MB 초과 POST 시 사용)는
     * 중간 응답을 받아야 본문을 보낸다. 안 보내면 약 1초 대기 후 전송한다. */
    const char *expect = http_get_header(req, "Expect");
    if (have < len && have == 0 && expect != NULL &&
        req->version_minor >= 1 && strcasecmp(expect, "100-continue") == 0) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (http_send_all(conn->fd, cont, sizeof(cont) - 1) < 0)
            return HTTP_READ_IO_ERROR;
    }

    /* 나머지는 버퍼를 거치지 않고 body 로 바로 수신 (Partial Read 반복) */
    while (have < len) {
        ssize_t n = recv_some(conn->fd, req->body + have, len - have);
        if (n < 0)
            return HTTP_READ_IO_ERROR;
        if (n == 0)
            return 400;                   /* Content-Length 보다 일찍 끊김 */
        have += (size_t)n;
    }
    req->body[len] = '\0';
    req->body_len  = len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 공개 API: 요청 하나 수신                                             */
/* ------------------------------------------------------------------ */
//공개 진입점
int http_read_request(http_conn_t *conn, http_request_t *req)
{
    long end;

    for (;;) {
        /* 요청 앞의 빈 줄은 무시 (RFC 7230 3.5) */
        while (conn->len >= 2 && conn->buf[0] == '\r' && conn->buf[1] == '\n')
            conn_consume(conn, 2);

        end = find_bytes(conn->buf, conn->len, "\r\n\r\n", 4);
        if (end >= 0)
            break;

        if (conn->len == sizeof(conn->buf)) {
            /* 8KB 안에 줄바꿈조차 없으면 요청 줄(URI)이 너무 긴 것 */
            return find_bytes(conn->buf, conn->len, "\r\n", 2) < 0 ? 414 : 413;
        }

        ssize_t n = recv_some(conn->fd, conn->buf + conn->len, sizeof(conn->buf) - conn->len);
        if (n < 0)
            return HTTP_READ_IO_ERROR;
        if (n == 0)
            return conn->len == 0 ? HTTP_READ_EOF : 400;   /* 헤더 도중 끊김 */
        conn->len += (size_t)n;
    }

    size_t head_len = (size_t)end + 4;
    int st = http_parse_head(req, conn->buf, head_len);
    conn_consume(conn, head_len);
    if (st != 0)
        return st;

    return read_body(conn, req);
}
