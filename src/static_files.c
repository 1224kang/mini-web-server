#include "static_files.h"

#include <string.h>
#include <strings.h> /* strcasecmp */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* src[0..len) 의 %xx 를 풀어 dst 에 쓴다('\0' 으로 끝냄). dst 는 len+1 이상.
 * 잘못된 %, 또는 %00(문자열을 중간에서 끊어 검사를 속이는 수법)이면 -1. */
static int percent_decode(const char *src, size_t len, char *dst)
{
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] != '%') {
            dst[j++] = src[i];
            continue;
        }
        if (i + 2 >= len)
            return -1;
        int hi = hex_value(src[i + 1]);
        int lo = hex_value(src[i + 2]);
        if (hi < 0 || lo < 0)
            return -1;
        char c = (char)(hi * 16 + lo);
        if (c == '\0')
            return -1;
        dst[j++] = c;
        i += 2;
    }
    dst[j] = '\0';
    return 0;
}

enum static_resolve_result static_resolve_path(const char *docroot,
                                               const char *target,
                                               char *out, size_t cap,
                                               int *trailing_slash)
{
    *trailing_slash = 0; // 원래 '/'로 끝났는지 기억해둬야 함. 

    /* "http://..." 같은 절대 URI 나 "*" 는 아직 다루지 않는다(6+단계 forward proxy). */
    if (target[0] != '/')
        return STATIC_BAD_REQUEST;

    /* 1) 쿼리/프래그먼트 잘라내기 */
    size_t path_len = strcspn(target, "?#");

    /* 2) 퍼센트 디코딩. 디코딩하면 길이는 줄기만 하므로 path_len+1 이면 충분. */
    char decoded[STATIC_MAX_PATH];
    if (path_len + 1 > sizeof(decoded))
        return STATIC_TOO_LONG;
    if (percent_decode(target, path_len, decoded) < 0)
        return STATIC_BAD_REQUEST;

    size_t dlen = strlen(decoded);
    *trailing_slash = dlen > 0 && decoded[dlen - 1] == '/';

    /* 3) out = docroot 로 시작해서 조각을 하나씩 붙인다. */
    size_t root_len = strlen(docroot);
    if (root_len + 1 > cap)
        return STATIC_TOO_LONG;
    memcpy(out, docroot, root_len + 1);
    size_t n = root_len; /* out 의 현재 길이. 절대 root_len 아래로 내려가지 않는다 = 루트 밖으로 못 나감 */

    char *save = decoded;
    while (*save) {
        /* 다음 '/' 까지가 한 조각 */
        char *seg = save;
        size_t seg_len = strcspn(seg, "/");
        save = seg + seg_len;
        if (*save == '/')
            save++;

        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.'))
            continue; /* "//" 나 "/./" 는 없는 것과 같다 */

        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (n == root_len)
                return STATIC_FORBIDDEN; /* 루트에서 더 위로 가려 함 */
            while (n > root_len && out[n - 1] != '/')
                n--;
            n--; /* 그 '/' 도 지운다 */
            out[n] = '\0';
            continue;
        }

        if (n + 1 + seg_len + 1 > cap)
            return STATIC_TOO_LONG;
        out[n++] = '/';
        memcpy(out + n, seg, seg_len);
        n += seg_len;
        out[n] = '\0';
    }
    return STATIC_OK;
}

const char *static_mime_type(const char *path)
{
    static const struct { const char *ext; const char *type; } table[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "text/javascript; charset=utf-8" },
        { "mjs",  "text/javascript; charset=utf-8" },
        { "json", "application/json" },
        { "txt",  "text/plain; charset=utf-8" },
        { "xml",  "application/xml" },
        { "svg",  "image/svg+xml" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "webp", "image/webp" },
        { "ico",  "image/x-icon" },
        { "pdf",  "application/pdf" },
        { "wasm", "application/wasm" },
        { "woff2","font/woff2" },
        { "mp4",  "video/mp4" },
    };

    /* 마지막 '/' 뒤의 파일 이름에서 마지막 '.' 을 찾는다.
     * ("/a.b/file" 처럼 디렉토리 이름의 점에 속지 않도록) */
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) { /* ".bashrc" 같은 숨김 파일은 확장자가 아니다 */
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
            if (strcasecmp(dot + 1, table[i].ext) == 0)
                return table[i].type;
    }
    return "application/octet-stream";
}

int static_is_under(const char *root, const char *path)
{
    size_t n = strlen(root);
    if (strncmp(root, path, n) != 0)
        return 0;
    return path[n] == '\0' || path[n] == '/';
}
