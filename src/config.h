#ifndef CONFIG_H
#define CONFIG_H

/*
 * 서버 설정. 포트/docroot/max_conns 는 명령줄로, CGI 설정는 설정 파일로 받는다.
 *
 * 설정 파일 형식 (한 줄에 하나, '#' 뒤는 주석):
 *
 *   cgi_dir                  cgi-bin
 *   cgi_timeout_ms           5000
 */

#include <limits.h> /* PATH_MAX */

struct server_config {
    int port;
    const char *docroot;          /* realpath 로 정규화된 절대 경로 */
    char cgi_dir[PATH_MAX];       /* realpath 로 정규화. 빈 문자열이면 CGI 끔 */
    int cgi_timeout_ms;

};

/* 기본값을 채운다. cgi_dir 은 "./cgi-bin" 이 있으면 그걸 쓴다. */
void config_defaults(struct server_config *cfg);

/* 설정 파일을 읽어 cfg 를 덮어쓴다. 실패하면 stderr 에 이유를 쓰고 -1. */
int config_load(struct server_config *cfg, const char *path);

#endif
