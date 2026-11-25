#include "builtin.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    ch = tolower(ch);
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

int query_get(const char *target, const char *key, char *out, size_t cap)
{
    const char *q = strchr(target, '?');
    if (!q)
        return 0;
    q++;
    size_t klen = strlen(key);
    const char *end = q + strcspn(q, "#");

    while (q < end) {
        const char *amp = memchr(q, '&', (size_t)(end - q));
        const char *pend = amp ? amp : end;
        const char *eq = memchr(q, '=', (size_t)(pend - q));
        const char *kend = eq ? eq : pend;
        if ((size_t)(kend - q) == klen && memcmp(q, key, klen) == 0) {
            size_t n = 0;
            for (const char *p = eq ? eq + 1 : pend; p < pend; p++) {
                int ch = (unsigned char)*p;
                if (ch == '+') {
                    ch = ' ';
                } else if (ch == '%') {
                    int hi = p + 2 < pend ? hexval(p[1]) : -1;
                    int lo = hi >= 0 ? hexval(p[2]) : -1;
                    if (lo < 0 || (hi == 0 && lo == 0))
                        return -1; /* 잘못된 %xx, %00 */
                    ch = hi * 16 + lo;
                    p += 2;
                }
                if (n + 1 >= cap)
                    return -1;
                out[n++] = (char)ch;
            }
            out[n] = '\0';
            return 1;
        }
        q = amp ? amp + 1 : end;
    }
    return 0;
}

static int parse_ll(const char *s, long long *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno == ERANGE)
        return -1;
    *out = v;
    return 0;
}

static void text(struct builtin_response *out, int status, const char *fmt, const char *a)
{
    out->status = status;
    out->content_type = "text/plain; charset=utf-8";
    int n = snprintf(out->text, sizeof(out->text), fmt, a);
    out->body = out->text;
    out->body_len = n < 0 ? 0 : (size_t)n < sizeof(out->text) ? (size_t)n : sizeof(out->text) - 1;
}

static void handle_add(const struct http_request *req, struct builtin_response *out)
{
    char a[64], b[64];
    long long x, y, sum;
    if (query_get(req->target, "a", a, sizeof(a)) != 1 ||
        query_get(req->target, "b", b, sizeof(b)) != 1 || parse_ll(a, &x) < 0 ||
        parse_ll(b, &y) < 0) {
        text(out, 400, "%s", "usage: /builtin/add?a=<integer>&b=<integer>\n");
        return;
    }
    if (__builtin_add_overflow(x, y, &sum)) {
        text(out, 400, "%s", "overflow\n");
        return;
    }
    char num[32];
    snprintf(num, sizeof(num), "%lld", sum);
    text(out, 200, "%s\n", num);
}

int builtin_handle(const struct http_request *req, const char *body, size_t body_len,
                   struct builtin_response *out)
{
    const char *t = req->target;
    size_t plen = strlen(BUILTIN_PREFIX);
    if (strncmp(t, BUILTIN_PREFIX, plen) != 0)
        return 0;
    memset(out, 0, sizeof(*out));
    const char *name = t + plen;
    size_t nlen = strcspn(name, "?#");
    const char *m = req->method;
    int is_get = strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0;

    if (nlen == 3 && memcmp(name, "add", 3) == 0) {
        if (!is_get) {
            text(out, 405, "%s", "405 Method Not Allowed\n");
            out->extra_headers = "Allow: GET, HEAD\r\n";
            return 1;
        }
        handle_add(req, out);
    } else if (nlen == 4 && memcmp(name, "echo", 4) == 0) {
        if (strcmp(m, "POST") != 0) {
            text(out, 405, "%s", "405 Method Not Allowed\n");
            out->extra_headers = "Allow: POST\r\n";
            return 1;
        }
        const char *ct = http_get_header(req, "Content-Type");
        out->status = 200;
        out->content_type = ct ? ct : "application/octet-stream";
        out->body = body;
        out->body_len = body_len;
    } else {
        text(out, 404, "%s", "404 Not Found\n");
    }
    return 1;
}
