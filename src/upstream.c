#include "upstream.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

/* ------------------------------------------------------------------ */
/* 늘어나는 문자열 버퍼                                                   */
/* ------------------------------------------------------------------ */

struct sb {
    char *p;
    size_t len, cap;
    int err;
};

static void sb_reserve(struct sb *s, size_t more)
{
    if (s->err || s->len + more + 1 <= s->cap)
        return;
    size_t cap = s->cap ? s->cap : 1024;
    while (cap < s->len + more + 1)
        cap *= 2;
    char *np = realloc(s->p, cap);
    if (!np) {
        s->err = 1;
        return;
    }
    s->p = np;
    s->cap = cap;
}

static void sb_mem(struct sb *s, const char *p, size_t n)
{
    sb_reserve(s, n);
    if (s->err)
        return;
    memcpy(s->p + s->len, p, n);
    s->len += n;
    s->p[s->len] = '\0';
}

static void sb_printf(struct sb *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        s->err = 1;
        return;
    }
    sb_reserve(s, (size_t)n);
    if (s->err)
        return;
    va_start(ap, fmt);
    vsnprintf(s->p + s->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    s->len += (size_t)n;
}

/* 성공하면 버퍼를 넘겨주고, 실패했으면 풀고 NULL. */
static char *sb_finish(struct sb *s, size_t *out_len)
{
    if (s->err || !s->p) {
        free(s->p);
        return NULL;
    }
    if (out_len)
        *out_len = s->len;
    return s->p;
}

/* ------------------------------------------------------------------ */
/* 헤더 이름 비교                                                        */
/* ------------------------------------------------------------------ */

static int name_is(const char *name, size_t len, const char *want)
{
    return strlen(want) == len && strncasecmp(name, want, len) == 0;
}

int upstream_is_hop_by_hop(const char *name, size_t len)
{
    /* 이 헤더들은 한 연결에만 적용된다. CGI 출력에서 가져오지 않고
     * 클라이언트 연결에 맞춰 응답을 만든다. (RFC 9110 7.6.1) */
    static const char *const hop[] = {
        "Connection", "Keep-Alive", "Proxy-Connection", "Transfer-Encoding",
        "TE", "Trailer", "Upgrade", "Proxy-Authenticate", "Proxy-Authorization",
    };
    for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); i++)
        if (name_is(name, len, hop[i]))
            return 1;
    return 0;
}

/* "Connection: close, X-Foo" 처럼 Connection 헤더에 이름이 적힌 헤더도 hop-by-hop 이다. */
static int listed_in_connection(const char *conn_value, size_t conn_len, const char *name,
                                size_t len)
{
    size_t i = 0;
    while (i < conn_len) {
        while (i < conn_len && strchr(", \t\r", conn_value[i]))
            i++;
        size_t start = i;
        while (i < conn_len && !strchr(", \t\r", conn_value[i]))
            i++;
        if (i > start && i - start == len && strncasecmp(conn_value + start, name, len) == 0)
            return 1;
    }
    return 0;
}

static int is_token_char(int ch)
{
    return isalnum(ch) || strchr("!#$%&'*+-.^_`|~", ch) != NULL;
}

/* ------------------------------------------------------------------ */
/* 업스트림 응답 해석                                                     */
/* ------------------------------------------------------------------ */

/* "200 OK" / "404" → status, reason. 실패하면 -1. */
static int parse_status(const char *p, size_t len, int *status, char *reason, size_t cap)
{
    if (len < 3 || !isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]) ||
        !isdigit((unsigned char)p[2]) || (len > 3 && p[3] != ' '))
        return -1;
    *status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    size_t rlen = len > 4 ? len - 4 : 0;
    if (rlen >= cap)
        rlen = cap - 1;
    memcpy(reason, p + (len > 4 ? 4 : len), rlen);
    reason[rlen] = '\0';
    return 0;
}

int upstream_parse_response(const char *buf, size_t len,
                            struct upstream_response *out)
{
    memset(out, 0, sizeof(*out));
    out->content_length = -1;
    size_t pos = 0;

    int has_status = 0, has_location = 0;
    out->head = buf + pos;
    for (;;) {
        const char *nl = memchr(buf + pos, '\n', len - pos);
        if (!nl)
            return -1; /* 빈 줄(헤더 끝)이 없다 */
        const char *line = buf + pos;
        size_t llen = (size_t)(nl - line);
        if (llen > 0 && line[llen - 1] == '\r')
            llen--;
        if (llen == 0) { /* 빈 줄 = 헤더 끝 */
            out->head_len = (size_t)(line - out->head);
            pos = (size_t)(nl - buf) + 1;
            break;
        }
        const char *colon = memchr(line, ':', llen);
        if (!colon || colon == line)
            return -1;
        size_t nlen = (size_t)(colon - line);
        for (size_t i = 0; i < nlen; i++)
            if (!is_token_char((unsigned char)line[i]))
                return -1;
        if (memchr(line, '\r', llen))
            return -1; /* 줄 가운데 \r: 헤더를 몰래 끼워 넣으려는 시도로 보고 거절 */
        const char *v = colon + 1;
        size_t vlen = llen - nlen - 1;
        while (vlen > 0 && (*v == ' ' || *v == '\t')) {
            v++;
            vlen--;
        }

        if (name_is(line, nlen, "Content-Length")) {
            long cl = 0;
            if (vlen == 0)
                return -1;
            for (size_t i = 0; i < vlen; i++) {
                if (!isdigit((unsigned char)v[i]) || cl > 100000000L)
                    return -1;
                cl = cl * 10 + (v[i] - '0');
            }
            out->content_length = cl;
        } else if (name_is(line, nlen, "Status")) {
            if (parse_status(v, vlen, &out->status, out->reason, sizeof(out->reason)) < 0)
                return -1;
            has_status = 1;
        } else if (name_is(line, nlen, "Location")) {
            has_location = 1;
        }
        pos = (size_t)(nl - buf) + 1;
    }

    if (!has_status) /* CGI 규칙: Status 가 없으면 200, Location 만 있으면 302 */
        out->status = has_location ? 302 : 200;
    if (out->status < 200 || out->status > 599)
        return -1;
    if (out->reason[0] == '\0')
        snprintf(out->reason, sizeof(out->reason), "%s", http_reason(out->status));

    out->body = buf + pos;
    out->body_len = len - pos;
    return 0;
}

static int status_has_no_body(int status)
{
    return status == 204 || status == 304;
}

char *upstream_build_client_response(const struct upstream_response *r, int head_only,
                                     int keep_alive, size_t *out_len)
{
    /* 업스트림 응답의 Connection 헤더 값 (거기 적힌 헤더도 빼야 한다) */
    const char *conn_v = "";
    size_t conn_len = 0;
    const char *end = r->head + r->head_len;
    for (const char *line = r->head; line < end;) {
        const char *nl = memchr(line, '\n', (size_t)(end - line));
        const char *le = nl ? nl : end;
        const char *colon = memchr(line, ':', (size_t)(le - line));
        if (colon && name_is(line, (size_t)(colon - line), "Connection")) {
            conn_v = colon + 1;
            conn_len = (size_t)(le - conn_v);
        }
        line = nl ? nl + 1 : end;
    }

    struct sb s = { 0 };
    sb_printf(&s, "HTTP/1.1 %d %s\r\n", r->status, r->reason);
    for (const char *line = r->head; line < end;) {
        const char *nl = memchr(line, '\n', (size_t)(end - line));
        const char *le = nl ? nl : end;
        size_t llen = (size_t)(le - line);
        if (llen > 0 && line[llen - 1] == '\r')
            llen--;
        const char *colon = memchr(line, ':', llen);
        size_t nlen = colon ? (size_t)(colon - line) : 0;
        int skip = !colon || upstream_is_hop_by_hop(line, nlen) ||
                   name_is(line, nlen, "Content-Length") || name_is(line, nlen, "Status") ||
                   listed_in_connection(conn_v, conn_len, line, nlen);
        if (!skip) {
            sb_mem(&s, line, llen);
            sb_mem(&s, "\r\n", 2);
        }
        line = nl ? nl + 1 : end;
    }

    int no_body = status_has_no_body(r->status);
    if (!no_body) {
        /* HEAD 응답에는 본문 없이 CGI 가 알려 준 Content-Length 를 쓴다 */
        size_t cl = head_only && r->content_length >= 0 ? (size_t)r->content_length : r->body_len;
        sb_printf(&s, "Content-Length: %zu\r\n", cl);
    }
    sb_printf(&s, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close");
    sb_mem(&s, "\r\n", 2);
    if (!head_only && !no_body)
        sb_mem(&s, r->body, r->body_len);
    return sb_finish(&s, out_len);
}

/* ------------------------------------------------------------------ */
/* CGI 환경변수                                                          */
/* ------------------------------------------------------------------ */

static char *env_pair(const char *name, const char *value)
{
    size_t n = strlen(name), v = strlen(value);
    char *s = malloc(n + v + 2);
    if (!s)
        return NULL;
    memcpy(s, name, n);
    s[n] = '=';
    memcpy(s + n + 1, value, v + 1);
    return s;
}

void cgi_free_env(char **envp)
{
    if (!envp)
        return;
    for (char **p = envp; *p; p++)
        free(*p);
    free(envp);
}

char **cgi_build_env(const struct http_request *req, size_t body_len,
                     const struct cgi_env_info *info)
{
    size_t cap = 20 + req->header_count + 1;
    char **env = calloc(cap, sizeof(*env));
    if (!env)
        return NULL;
    size_t n = 0;
    int bad = 0;
#define PUT(name, value)                                                                   \
    do {                                                                                   \
        if (!(env[n++] = env_pair((name), (value))))                                       \
            bad = 1;                                                                       \
    } while (0)

    char num[32], host[256];
    PUT("GATEWAY_INTERFACE", "CGI/1.1");
    PUT("SERVER_SOFTWARE", "mini-web-server");
    PUT("SERVER_PROTOCOL", req->version);
    const char *h = http_get_header(req, "Host");
    snprintf(host, sizeof(host), "%.*s", h ? (int)strcspn(h, ":") : 9, h ? h : "localhost");
    PUT("SERVER_NAME", host);
    snprintf(num, sizeof(num), "%d", info->server_port);
    PUT("SERVER_PORT", num);
    PUT("REQUEST_METHOD", req->method);
    PUT("SCRIPT_NAME", info->script_name);
    PUT("PATH_INFO", info->path_info);
    PUT("QUERY_STRING", info->query);
    PUT("REMOTE_ADDR", info->remote_addr);
    snprintf(num, sizeof(num), "%d", info->remote_port);
    PUT("REMOTE_PORT", num);
    if (info->path_env)
        PUT("PATH", info->path_env);
    if (body_len > 0 || strcmp(req->method, "POST") == 0) {
        snprintf(num, sizeof(num), "%zu", body_len);
        PUT("CONTENT_LENGTH", num);
    }
    const char *ct = http_get_header(req, "Content-Type");
    if (ct)
        PUT("CONTENT_TYPE", ct);

    /* 나머지 요청 헤더: "Accept-Language" → "HTTP_ACCEPT_LANGUAGE" */
    for (size_t i = 0; i < req->header_count && !bad; i++) {
        const char *name = req->headers[i].name;
        size_t nlen = strlen(name);
        /* Content-* 는 위에서 따로 넣었다. "Proxy" 헤더는 HTTP_PROXY 가 되어 CGI 안의
         * HTTP 라이브러리가 프록시 설정으로 착각한다(httpoxy 취약점) → 버린다. */
        if (name_is(name, nlen, "Content-Length") || name_is(name, nlen, "Content-Type") ||
            name_is(name, nlen, "Proxy") || nlen > 100)
            continue;
        char key[128] = "HTTP_";
        size_t k = 5;
        int ok = 1;
        for (size_t j = 0; j < nlen; j++) {
            unsigned char ch = (unsigned char)name[j];
            if (ch == '-')
                key[k++] = '_';
            else if (isalnum(ch))
                key[k++] = (char)toupper(ch);
            else
                ok = 0; /* 밑줄 등: "X_Foo" 와 "X-Foo" 가 같은 변수가 되는 혼동 방지 */
        }
        key[k] = '\0';
        if (ok)
            PUT(key, req->headers[i].value);
    }
#undef PUT
    env[n] = NULL;
    if (bad) {
        cgi_free_env(env);
        return NULL;
    }
    return env;
}
