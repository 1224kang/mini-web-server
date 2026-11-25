/*
 * config.c - config.conf / CLI 라우팅 규칙 파싱
 */
#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define CONFIG_LINE_MAX 2048
#define CONFIG_MAX_TOKENS (CONFIG_MAX_BACKENDS + 2)

void config_set_defaults(server_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port                      = 8080;
    snprintf(cfg->doc_root, sizeof(cfg->doc_root), "%s", "./www");
    cfg->threads                   = 8;
    cfg->queue_size                = 256;
    cfg->keepalive_timeout_sec     = 5;
    cfg->request_timeout_sec       = 10;
    cfg->max_keepalive_requests    = 100;
    cfg->proxy_connect_timeout_sec = 3;
    cfg->proxy_timeout_sec         = 10;
    cfg->route_count               = 0;
}

int config_parse_long(const char *s, long min, long max, long *out)
{
    char *end = NULL;

    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

int config_parse_backend(const char *spec, backend_addr_t *out)
{
    char host[96];                     /* host + ":" + port 가 backend_addr_t.text(128) 에 항상 들어가도록 */
    const char *colon = strrchr(spec, ':');
    long port;

    if (colon == NULL || colon == spec)
        return -1;
    size_t hlen = (size_t)(colon - spec);
    if (hlen >= sizeof(host))
        return -1;
    memcpy(host, spec, hlen);
    host[hlen] = '\0';
    if (config_parse_long(colon + 1, 1, 65535, &port) < 0)
        return -1;

    /* 호스트 이름 해석은 시작할 때 한 번만 한다 (요청마다 DNS 조회 X) */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL)
        return -1;

    memset(&out->addr, 0, sizeof(out->addr));
    memcpy(&out->addr, res->ai_addr, sizeof(struct sockaddr_in));
    out->addr.sin_family = AF_INET;
    out->addr.sin_port   = htons((uint16_t)port);
    freeaddrinfo(res);

    snprintf(out->text, sizeof(out->text), "%s:%ld", host, port);
    return 0;
}

int config_add_route(server_config_t *cfg, const char *prefix,
                     const char *const *backend_specs, size_t n)
{
    if (prefix[0] != '/' || strlen(prefix) >= CONFIG_MAX_PREFIX_LEN) {
        fprintf(stderr, "route prefix must start with '/' and be shorter than %d: '%s'\n",
                CONFIG_MAX_PREFIX_LEN, prefix);
        return -1;
    }
    if (n == 0 || n > CONFIG_MAX_BACKENDS) {
        fprintf(stderr, "route '%s' needs 1..%d backends\n", prefix, CONFIG_MAX_BACKENDS);
        return -1;
    }

    route_config_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.prefix, sizeof(tmp.prefix), "%s", prefix);
    for (size_t i = 0; i < n; i++) {
        if (config_parse_backend(backend_specs[i], &tmp.backends[i]) < 0) {
            fprintf(stderr, "invalid backend '%s' (expected host:port)\n", backend_specs[i]);
            return -1;
        }
    }
    tmp.backend_count = n;

    /* 같은 prefix 는 나중 설정(CLI 등)이 덮어쓴다 */
    for (size_t i = 0; i < cfg->route_count; i++) {
        if (strcmp(cfg->routes[i].prefix, prefix) == 0) {
            cfg->routes[i] = tmp;
            return 0;
        }
    }
    if (cfg->route_count >= CONFIG_MAX_ROUTES) {
        fprintf(stderr, "too many routes (max %d)\n", CONFIG_MAX_ROUTES);
        return -1;
    }
    cfg->routes[cfg->route_count++] = tmp;
    return 0;
}

int config_add_route_spec(server_config_t *cfg, const char *spec)
{
    char buf[CONFIG_LINE_MAX];
    const char *list[CONFIG_MAX_BACKENDS];
    size_t n = 0;
    char *save = NULL;

    if (strlen(spec) >= sizeof(buf))
        return -1;
    snprintf(buf, sizeof(buf), "%s", spec);

    char *eq = strchr(buf, '=');
    if (eq == NULL) {
        fprintf(stderr, "route spec must look like /prefix/=host:port[,host:port]: '%s'\n", spec);
        return -1;
    }
    *eq = '\0';
    for (char *tok = strtok_r(eq + 1, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        if (n == CONFIG_MAX_BACKENDS) {
            fprintf(stderr, "too many backends in '%s'\n", spec);
            return -1;
        }
        list[n++] = tok;
    }
    return config_add_route(cfg, buf, list, n);
}

/* 한 줄 처리. 성공 0, 실패 -1 (메시지는 호출자가 줄 번호와 함께 출력) */
static int apply_line(server_config_t *cfg, char **tok, size_t ntok, const char **why)
{
    const char *key = tok[0];
    long v;

    *why = "invalid value";

    if (strcmp(key, "route") == 0) {
        if (ntok < 3) {
            *why = "usage: route <prefix> <host:port> [host:port ...]";
            return -1;
        }
        *why = "invalid route";
        return config_add_route(cfg, tok[1], (const char *const *)&tok[2], ntok - 2);
    }

    if (ntok != 2) {
        *why = "expected exactly one value";
        return -1;
    }
    const char *val = tok[1];

    if (strcmp(key, "port") == 0) {
        if (config_parse_long(val, 1, 65535, &v) < 0) return -1;
        cfg->port = (uint16_t)v;
    } else if (strcmp(key, "root") == 0) {
        if (strlen(val) >= sizeof(cfg->doc_root)) return -1;
        snprintf(cfg->doc_root, sizeof(cfg->doc_root), "%s", val);
    } else if (strcmp(key, "threads") == 0) {
        if (config_parse_long(val, 1, 1024, &v) < 0) return -1;
        cfg->threads = (size_t)v;
    } else if (strcmp(key, "queue_size") == 0) {
        if (config_parse_long(val, 1, 65536, &v) < 0) return -1;
        cfg->queue_size = (size_t)v;
    } else if (strcmp(key, "keepalive_timeout") == 0) {
        if (config_parse_long(val, 1, 3600, &v) < 0) return -1;
        cfg->keepalive_timeout_sec = (int)v;
    } else if (strcmp(key, "request_timeout") == 0) {
        if (config_parse_long(val, 1, 3600, &v) < 0) return -1;
        cfg->request_timeout_sec = (int)v;
    } else if (strcmp(key, "max_keepalive_requests") == 0) {
        if (config_parse_long(val, 1, 1000000, &v) < 0) return -1;
        cfg->max_keepalive_requests = (int)v;
    } else if (strcmp(key, "proxy_connect_timeout") == 0) {
        if (config_parse_long(val, 1, 3600, &v) < 0) return -1;
        cfg->proxy_connect_timeout_sec = (int)v;
    } else if (strcmp(key, "proxy_timeout") == 0) {
        if (config_parse_long(val, 1, 3600, &v) < 0) return -1;
        cfg->proxy_timeout_sec = (int)v;
    } else {
        *why = "unknown key";
        return -1;
    }
    return 0;
}

int config_load_file(server_config_t *cfg, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "cannot open config '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[CONFIG_LINE_MAX];
    int  lineno = 0;
    int  rc = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        lineno++;

        size_t len = strlen(line);
        if (len == sizeof(line) - 1 && line[len - 1] != '\n' && !feof(fp)) {
            fprintf(stderr, "%s:%d: line too long\n", path, lineno);
            rc = -1;
            break;
        }

        char *hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';

        char  *tok[CONFIG_MAX_TOKENS];
        size_t ntok = 0;
        char  *save = NULL;
        int    too_many = 0;
        for (char *t = strtok_r(line, " \t\r\n", &save); t != NULL; t = strtok_r(NULL, " \t\r\n", &save)) {
            if (ntok == CONFIG_MAX_TOKENS) {
                too_many = 1;
                break;
            }
            tok[ntok++] = t;
        }
        if (ntok == 0)
            continue;                       /* 빈 줄 / 주석 줄 */
        if (too_many) {
            fprintf(stderr, "%s:%d: too many values\n", path, lineno);
            rc = -1;
            break;
        }

        const char *why = NULL;
        if (apply_line(cfg, tok, ntok, &why) < 0) {
            fprintf(stderr, "%s:%d: %s (%s)\n", path, lineno, why, tok[0]);
            rc = -1;
            break;
        }
    }

    fclose(fp);
    return rc;
}

void config_print(const server_config_t *cfg)
{
    printf("[INFO] config: port=%u root=%s threads=%zu queue=%zu keepalive=%ds request_timeout=%ds\n",
           (unsigned)cfg->port, cfg->doc_root, cfg->threads, cfg->queue_size,
           cfg->keepalive_timeout_sec, cfg->request_timeout_sec);
    for (size_t i = 0; i < cfg->route_count; i++) {
        printf("[INFO] route %s ->", cfg->routes[i].prefix);
        for (size_t j = 0; j < cfg->routes[i].backend_count; j++)
            printf(" %s", cfg->routes[i].backends[j].text);
        printf("\n");
    }
    fflush(stdout);
}
