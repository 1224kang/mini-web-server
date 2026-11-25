#include "config.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>

void config_defaults(struct server_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->keepalive_timeout_ms = 5000;
    cfg->header_timeout_ms = 10000;
    cfg->body_timeout_ms = 30000;
    cfg->cgi_timeout_ms = 5000;
    cfg->proxy_connect_timeout_ms = 1000;
    cfg->proxy_timeout_ms = 5000;
    cfg->proxy_fail_timeout_ms = 10000;

    struct stat st;
    if (stat("cgi-bin", &st) == 0 && S_ISDIR(st.st_mode) && !realpath("cgi-bin", cfg->cgi_dir))
        cfg->cgi_dir[0] = '\0';
}

/* "127.0.0.1:8001" / "localhost:8001" → sockaddr_in. 시작할 때 한 번만 하므로
 * 블로킹 getaddrinfo 를 써도 된다(이벤트 루프가 돌기 전). */
static int parse_backend(const char *s, struct backend *b)
{
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s || colon[1] == '\0')
        return -1;
    char host[64];
    size_t hlen = (size_t)(colon - s);
    if (hlen >= sizeof(host))
        return -1;
    memcpy(host, s, hlen);
    host[hlen] = '\0';

    char *end;
    long port = strtol(colon + 1, &end, 10);
    if (*end != '\0' || port < 1 || port > 65535)
        return -1;

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return -1;
    memcpy(&b->addr, res->ai_addr, sizeof(b->addr));
    b->addr.sin_port = htons((unsigned short)port);
    freeaddrinfo(res);
    snprintf(b->name, sizeof(b->name), "%s:%ld", host, port);
    return 0;
}

static int parse_ms(const char *s, int *out)
{
    char *end;
    long v = s ? strtol(s, &end, 10) : 0;
    if (!s || *end != '\0' || v < 1 || v > 600000)
        return -1;
    *out = (int)v;
    return 0;
}

int config_load(struct server_config *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }
    char line[1024];
    int lineno = 0, rc = 0;
    while (rc == 0 && fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *save;
        char *key = strtok_r(line, " \t\r\n", &save);
        if (!key)
            continue; /* 빈 줄 */
        char *val = strtok_r(NULL, " \t\r\n", &save);

        if (strcmp(key, "cgi_dir") == 0) {
            if (!val || !realpath(val, cfg->cgi_dir)) {
                fprintf(stderr, "%s:%d: cgi_dir not found: %s\n", path, lineno, val ? val : "");
                rc = -1;
            }
        } else if (strcmp(key, "workers") == 0) {
            char *end;
            long v = val ? strtol(val, &end, 10) : 0;
            if (!val || *end != '\0' || v < 1 || v > CONFIG_MAX_WORKERS)
                rc = -1;
            else
                cfg->workers = (int)v;
        } else if (strcmp(key, "keepalive_timeout_ms") == 0) {
            rc = parse_ms(val, &cfg->keepalive_timeout_ms);
        } else if (strcmp(key, "header_timeout_ms") == 0) {
            rc = parse_ms(val, &cfg->header_timeout_ms);
        } else if (strcmp(key, "body_timeout_ms") == 0) {
            rc = parse_ms(val, &cfg->body_timeout_ms);
        } else if (strcmp(key, "cgi_timeout_ms") == 0) {
            rc = parse_ms(val, &cfg->cgi_timeout_ms);
        } else if (strcmp(key, "proxy_connect_timeout_ms") == 0) {
            rc = parse_ms(val, &cfg->proxy_connect_timeout_ms);
        } else if (strcmp(key, "proxy_timeout_ms") == 0) {
            rc = parse_ms(val, &cfg->proxy_timeout_ms);
        } else if (strcmp(key, "proxy_fail_timeout_ms") == 0) {
            rc = parse_ms(val, &cfg->proxy_fail_timeout_ms);
        } else if (strcmp(key, "proxy") == 0) {
            if (cfg->nroutes == CONFIG_MAX_ROUTES || !val || val[0] != '/' ||
                strlen(val) >= CONFIG_MAX_PREFIX) {
                rc = -1;
                break;
            }
            struct proxy_route *r = &cfg->routes[cfg->nroutes];
            memset(r, 0, sizeof(*r));
            atomic_init(&r->next, 0);
            strcpy(r->prefix, val);
            char *b;
            while ((b = strtok_r(NULL, " \t\r\n", &save))) {
                if (r->nbackends == CONFIG_MAX_BACKENDS ||
                    parse_backend(b, &r->backends[r->nbackends]) < 0) {
                    fprintf(stderr, "%s:%d: bad backend: %s\n", path, lineno, b);
                    rc = -1;
                    break;
                }
                r->nbackends++;
            }
            if (rc == 0 && r->nbackends == 0)
                rc = -1;
            if (rc == 0)
                cfg->nroutes++;
        } else {
            fprintf(stderr, "%s:%d: unknown key: %s\n", path, lineno, key);
            rc = -1;
        }
        if (rc < 0)
            fprintf(stderr, "%s:%d: invalid line\n", path, lineno);
    }
    fclose(f);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 백엔드 장애 조치 (passive health check)                                 */
/* ------------------------------------------------------------------ */

struct backend *config_pick_alive(struct proxy_route *route, long long now, unsigned *tried)
{
    struct backend *fallback = NULL;
    /* Round-Robin 카운터를 한 칸씩 돌리며 최대 nbackends 번 본다.
     * 모두 살아 있으면 한 번에 끝나므로 순서는 config_pick_backend 와 같다. */
    for (int k = 0; k < route->nbackends; k++) {
        struct backend *b = (struct backend *)config_pick_backend(route);
        unsigned bit = 1u << (unsigned)(b - route->backends);
        if (*tried & bit)
            continue;
        if (config_is_down(b, now)) {
            if (!fallback)
                fallback = b;
            continue;
        }
        *tried |= bit;
        return b;
    }
    /* 살아 있는 백엔드가 없다: down 이라도 아직 안 해 본 것으로 복구를 확인한다 */
    if (fallback)
        *tried |= 1u << (unsigned)(fallback - route->backends);
    return fallback;
}

void config_mark_down(struct backend *b, long long until)
{
    atomic_store_explicit(&b->down_until, until, memory_order_relaxed);
}

int config_is_down(const struct backend *b, long long now)
{
    return now < atomic_load_explicit(&b->down_until, memory_order_relaxed);
}

struct proxy_route *config_match_route(struct server_config *cfg, const char *target)
{
    struct proxy_route *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < cfg->nroutes; i++) {
        size_t plen = strlen(cfg->routes[i].prefix);
        if (plen > best_len && strncmp(target, cfg->routes[i].prefix, plen) == 0) {
            best = &cfg->routes[i];
            best_len = plen;
        }
    }
    return best;
}

const struct backend *config_pick_backend(struct proxy_route *route)
{
    unsigned long n = atomic_fetch_add_explicit(&route->next, 1, memory_order_relaxed);
    return &route->backends[n % (unsigned long)route->nbackends];
}
