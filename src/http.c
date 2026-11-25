#include "http.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

long http_find_head_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return (long)(i + 4);
    }
    return -1;
}

/* RFC 9110 의 token 문자: 메서드 이름과 헤더 이름에 쓸 수 있는 글자들. */
static int is_tchar(unsigned char c)
{
    return isalnum(c) || (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

/* *cur 부터 "\r\n" 까지를 한 줄로 잘라 반환한다('\r' 자리에 '\0').
 * *cur 는 다음 줄 시작으로 옮긴다.
 * 줄 안에 '\0', 단독 '\r', 단독 '\n' 이 있으면 NULL → 400.
 * (단독 LF 를 허용하면 앞단 프록시와 줄 경계를 다르게 해석하는 "request smuggling" 의 빌미가 된다.) */
static char *take_line(char **cur, char *end)
{
    char *start = *cur;
    for (char *p = start; p < end; p++) {
        if (*p == '\0' || *p == '\n')
            return NULL;
        if (*p == '\r') {
            if (p + 1 >= end || p[1] != '\n')
                return NULL;
            *p = '\0';
            *cur = p + 2;
            return start;
        }
    }
    return NULL;
}

/* 요청 라인: "METHOD SP target SP HTTP/x.y" (공백은 정확히 하나씩) */
static enum http_parse_result parse_request_line(char *line, struct http_request *req)
{
    char *sp1 = strchr(line, ' ');
    if (!sp1)
        return HTTP_PARSE_BAD_REQUEST;
    *sp1 = '\0';

    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2)
        return HTTP_PARSE_BAD_REQUEST;
    *sp2 = '\0';

    char *version = sp2 + 1;
    /* 빈 칸이 있으면("GET  / HTTP/1.1") 어느 하나가 빈 문자열이 된다. */
    if (*line == '\0' || *target == '\0' || *version == '\0' || strchr(version, ' '))
        return HTTP_PARSE_BAD_REQUEST;

    for (const char *p = line; *p; p++)
        if (!is_tchar((unsigned char)*p))
            return HTTP_PARSE_BAD_REQUEST;

    /* "HTTP/숫자.숫자" 꼴이 아니면 HTTP 요청 자체가 아니다 → 400.
     * 꼴은 맞는데 1.0/1.1 이 아니면 → 505. */
    if (strncmp(version, "HTTP/", 5) != 0 ||
        !isdigit((unsigned char)version[5]) || version[6] != '.' ||
        !isdigit((unsigned char)version[7]) || version[8] != '\0')
        return HTTP_PARSE_BAD_REQUEST;
    if (strcmp(version, "HTTP/1.0") != 0 && strcmp(version, "HTTP/1.1") != 0)
        return HTTP_PARSE_BAD_VERSION;

    req->method = line;
    req->target = target;
    req->version = version;
    return HTTP_PARSE_OK;
}

/* 헤더 한 줄: "Name:" OWS value OWS */
static enum http_parse_result parse_header_line(char *line, struct http_request *req)
{
    if (req->header_count == HTTP_MAX_HEADERS)
        return HTTP_PARSE_TOO_MANY_HEADERS;

    char *colon = strchr(line, ':');
    if (!colon || colon == line)
        return HTTP_PARSE_BAD_REQUEST;

    /* 이름에 공백이 섞이면 안 된다("Host : x" 는 400). 줄이 공백으로 시작하는
     * 옛날 줄 접기(obs-fold)도 여기서 걸러진다. */
    for (const char *p = line; p < colon; p++)
        if (!is_tchar((unsigned char)*p))
            return HTTP_PARSE_BAD_REQUEST;
    *colon = '\0';

    /* 값 앞뒤의 공백/탭 제거 */
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    char *ve = v + strlen(v);
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
        *--ve = '\0';

    req->headers[req->header_count].name = line;
    req->headers[req->header_count].value = v;
    req->header_count++;
    return HTTP_PARSE_OK;
}

enum http_parse_result http_parse_request(char *buf, size_t head_len,
                                          struct http_request *req)
{
    memset(req, 0, sizeof(*req));

    char *cur = buf;
    char *end = buf + head_len;

    char *line = take_line(&cur, end);
    if (!line)
        return HTTP_PARSE_BAD_REQUEST;
    enum http_parse_result r = parse_request_line(line, req);
    if (r != HTTP_PARSE_OK)
        return r;

    /* 빈 줄("\r\n")이 나올 때까지 헤더를 읽는다. */
    for (;;) {
        line = take_line(&cur, end);
        if (!line)
            return HTTP_PARSE_BAD_REQUEST;
        if (*line == '\0')
            break;
        r = parse_header_line(line, req);
        if (r != HTTP_PARSE_OK)
            return r;
    }
    if (cur != end) /* 빈 줄 뒤에 head 영역이 더 남았다 = head_len 이 잘못 주어짐 */
        return HTTP_PARSE_BAD_REQUEST;

    /* HTTP/1.1 은 Host 헤더가 필수다. */
    if (strcmp(req->version, "HTTP/1.1") == 0 && !http_get_header(req, "Host"))
        return HTTP_PARSE_BAD_REQUEST;

    return HTTP_PARSE_OK;
}

const char *http_get_header(const struct http_request *req, const char *name)
{
    for (size_t i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    return NULL;
}

int http_content_length(const struct http_request *req, size_t *out)
{
    *out = 0;
    const char *found = NULL;
    for (size_t i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, "Content-Length") == 0) {
            if (found) /* 두 번 나오면 어느 쪽을 믿을지 모호하다 → 거절 */
                return -1;
            found = req->headers[i].value;
        }
    }
    if (!found)
        return 0;
    if (*found == '\0')
        return -1;

    size_t v = 0;
    for (const char *p = found; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return -1; /* "-1", "10abc", "1 2" 모두 여기서 걸린다 */
        size_t d = (size_t)(*p - '0');
        if (v > (SIZE_MAX - d) / 10)
            return -1; /* 오버플로 */
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/* Transfer-Encoding 값들을 쉼표 목록으로 훑어 코딩 개수와 마지막 코딩이 chunked 인지 센다. */
struct te_scan {
    size_t codings;      /* 빈 항목을 뺀 코딩 수 */
    size_t chunked;      /* 그중 chunked 수 */
    int last_is_chunked;
};

static void scan_transfer_codings(const char *list, struct te_scan *s)
{
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if (end == start)
            continue;
        int is_chunked = (size_t)(end - start) == 7 && strncasecmp(start, "chunked", 7) == 0;
        s->codings++;
        s->chunked += (size_t)is_chunked;
        s->last_is_chunked = is_chunked;
    }
}

int http_body_framing(const struct http_request *req, enum http_body_kind *kind, size_t *length)
{
    *kind = HTTP_BODY_NONE;
    *length = 0;

    struct te_scan te = { 0 };
    int has_te = 0, has_cl = 0;
    for (size_t i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, "Transfer-Encoding") == 0) {
            has_te = 1;
            scan_transfer_codings(req->headers[i].value, &te);
        } else if (strcasecmp(req->headers[i].name, "Content-Length") == 0) {
            has_cl = 1;
        }
    }

    if (has_te) {
        /* 둘 다 있으면 앞단 프록시와 우리가 본문 끝을 다르게 볼 수 있다 → 거절 */
        if (has_cl || strcmp(req->version, "HTTP/1.0") == 0)
            return 400;
        /* chunked 가 마지막이 아니면 본문 끝을 알 수 없다 (RFC 9112 §6.3) */
        if (te.codings == 0 || !te.last_is_chunked || te.chunked > 1)
            return 400;
        if (te.codings > 1)
            return 501; /* gzip 같은 다른 코딩은 풀 줄 모른다 */
        *kind = HTTP_BODY_CHUNKED;
        return 0;
    }

    if (http_content_length(req, length) < 0)
        return 400;
    *kind = *length > 0 ? HTTP_BODY_LENGTH : HTTP_BODY_NONE;
    return 0;
}

/* "keep-alive, Upgrade" 처럼 쉼표로 나열된 토큰 중에 word 가 있는지 (대소문자 무시). */
static int has_token(const char *list, const char *word)
{
    size_t wlen = strlen(word);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if ((size_t)(end - start) == wlen && strncasecmp(start, word, wlen) == 0)
            return 1;
    }
    return 0;
}

int http_wants_keep_alive(const struct http_request *req)
{
    /* 기본값: HTTP/1.1 은 "계속 쓴다", HTTP/1.0 은 "끊는다". */
    int keep = strcmp(req->version, "HTTP/1.1") == 0;
    for (size_t i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, "Connection") != 0)
            continue;
        if (has_token(req->headers[i].value, "close"))
            return 0; /* close 가 하나라도 있으면 무조건 끊는다 */
        if (has_token(req->headers[i].value, "keep-alive"))
            keep = 1;
    }
    return keep;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int http_build_response_head(char *out, size_t cap, int status,
                             const char *content_type, size_t body_len,
                             const char *extra_headers, int keep_alive)
{
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "\r\n",
                     status, http_reason(status), content_type, body_len,
                     keep_alive ? "keep-alive" : "close",
                     extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= cap)
        return -1; /* 잘렸다 */
    return n;
}
