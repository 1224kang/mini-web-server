#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void config_defaults(struct server_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->cgi_timeout_ms = 5000;

    struct stat st;
    if (stat("cgi-bin", &st) == 0 && S_ISDIR(st.st_mode) && !realpath("cgi-bin", cfg->cgi_dir))
        cfg->cgi_dir[0] = '\0';
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
        } else if (strcmp(key, "cgi_timeout_ms") == 0) {
            rc = parse_ms(val, &cfg->cgi_timeout_ms);
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
