#ifndef CONFIG_H
#define CONFIG_H

/*
 * 서버 설정. 포트/docroot/max_conns 는 명령줄로, 타임아웃과 라우팅은 설정 파일로 받는다.
 *
 * 설정 파일 형식 (한 줄에 하나, '#' 뒤는 주석):
 *
 *   workers                  4          (이벤트 루프 스레드 수, 생략하면 CPU 코어 수)
 *   cgi_dir                  cgi-bin
 *   cgi_timeout_ms           5000
 *   keepalive_timeout_ms     5000
 *   header_timeout_ms       10000
 *   body_timeout_ms         30000
 *   proxy_connect_timeout_ms 1000
 *   proxy_timeout_ms         5000
 *   proxy_fail_timeout_ms   10000
 *   proxy  /api/  127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003
 *
 * proxy 줄은 "이 접두사로 시작하는 요청은 이 백엔드들에게 번갈아(Round-Robin) 넘긴다"는 뜻.
 * 여러 줄이 겹치면 가장 긴 접두사가 이긴다.
 *
 * 장애 조치: 백엔드 연결(connect)에 실패하면 그 백엔드를 proxy_fail_timeout_ms 동안 down 으로
 * 표시해 건너뛰고, 같은 요청을 아직 안 해 본 다음 백엔드로 다시 보낸다. 요청을 한 바이트도
 * 보내기 전에 실패한 것이므로 POST 도 안전하게 재시도할 수 있다. 시간이 지나면 다시 시도한다.
 */

#include <limits.h> /* PATH_MAX */
#include <netinet/in.h>
#include <stdatomic.h>

#define CONFIG_MAX_ROUTES   16
#define CONFIG_MAX_BACKENDS 8
#define CONFIG_MAX_PREFIX   128

struct backend {
    char name[64];             /* 로그와 헤더에 쓸 "127.0.0.1:8001" */
    struct sockaddr_in addr;   /* 시작할 때 한 번만 주소를 풀어 둔다(요청마다 DNS 조회 안 함) */
    /* 장애 조치: 연결에 실패하면 이 시각(now_ms 기준)까지 "down" 으로 보고 건너뛴다.
     * 0 이면 살아 있음. 여러 이벤트 루프 스레드가 함께 읽고 쓸 수 있도록 atomic. */
    atomic_llong down_until;
};

struct proxy_route {
    char prefix[CONFIG_MAX_PREFIX];
    struct backend backends[CONFIG_MAX_BACKENDS];
    int nbackends;
    /* 다음에 고를 백엔드 번호. 여러 이벤트 루프 스레드가 함께 쓰므로 atomic 으로 하나씩 올린다
     * (lock 없이도 두 스레드가 같은 번호를 받지 않는다). */
    atomic_ulong next;
};

#define CONFIG_MAX_WORKERS 64

struct server_config {
    int port;
    const char *docroot;          /* realpath 로 정규화된 절대 경로 */
    int workers;                  /* 이벤트 루프 스레드 수. 0 이면 CPU 코어 수(최대 8) */
    int keepalive_timeout_ms;    /* 요청 첫 바이트까지 기다리는 시간 */
    int header_timeout_ms;       /* 첫 바이트부터 헤더 전체까지의 마감 */
    int body_timeout_ms;         /* 헤더 파싱 후 본문 전체까지의 마감 */
    char cgi_dir[PATH_MAX];       /* realpath 로 정규화. 빈 문자열이면 CGI 끔 */
    int cgi_timeout_ms;
    int proxy_connect_timeout_ms;
    int proxy_timeout_ms;         /* 백엔드가 이 시간 동안 한 바이트도 안 보내면 504 */
    int proxy_fail_timeout_ms;    /* connect 에 실패한 백엔드를 건너뛰는 시간 */
    struct proxy_route routes[CONFIG_MAX_ROUTES];
    int nroutes;
};

/* 기본값을 채운다. cgi_dir 은 "./cgi-bin" 이 있으면 그걸 쓴다. */
void config_defaults(struct server_config *cfg);

/* 설정 파일을 읽어 cfg 를 덮어쓴다. 실패하면 stderr 에 이유를 쓰고 -1. */
int config_load(struct server_config *cfg, const char *path);

/* target 경로("/api/users?x=1")에 맞는 프록시 경로. 없으면 NULL. */
struct proxy_route *config_match_route(struct server_config *cfg, const char *target);

/* Round-Robin: 호출할 때마다 다음 백엔드를 돌려준다. */
const struct backend *config_pick_backend(struct proxy_route *route);

/* 장애 조치용 선택: Round-Robin 순서대로 보되,
 *   - *tried 에 비트가 켜진 백엔드(이번 요청에서 이미 실패)는 건너뛰고,
 *   - down 인 백엔드(now < down_until)는 살아 있는 백엔드가 없을 때만 고른다(복구 확인용).
 * 고른 백엔드의 비트를 *tried 에 켠다. 다 해 봤으면 NULL. */
struct backend *config_pick_alive(struct proxy_route *route, long long now, unsigned *tried);

/* 백엔드를 until 시각까지 down 으로 표시한다. */
void config_mark_down(struct backend *b, long long until);

/* 백엔드가 지금 down 인지 */
int config_is_down(const struct backend *b, long long now);

#endif
