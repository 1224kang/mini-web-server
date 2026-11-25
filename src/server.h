#ifndef SERVER_H
#define SERVER_H

#include "config.h"

struct server_stats {
    unsigned long connections; /* 지금까지 받아들인 연결 수 */
    unsigned long requests;    /* 지금까지 파싱에 성공한 요청 수 */
    unsigned long rejected;    /* 연결 수 한도 때문에 503 으로 돌려보낸 수 */
};

/* port 에서 listen 하는 소켓을 만들어 fd 를 반환한다. 실패하면 -1. */
int server_listen(int port);

/* 메인 스레드는 accept 만 하고, 받은 연결을 이벤트 루프 스레드 workers 개 중
 * 연결이 가장 적은 곳에 넘긴다. 각 루프는 자기 연결만 poll 로 처리한다.
 * stop_fd 가 "읽을 수 있음"이 되면 우아한 종료를 시작하고, 모든 루프의 열린 연결이
 * 끝나면 돌아온다(listen_fd 는 여기서 닫는다).
 * 동시에 열어 둘 연결은 모든 루프를 합쳐 max_conns 개까지, 넘치면 503.
 * 요청은 프록시 경로 → /cgi-bin/ → /builtin/ → 정적 파일(GET/HEAD) 순으로 처리한다. */
int server_run(int listen_fd, int stop_fd, struct server_config *cfg, int max_conns, int workers,
               struct server_stats *stats);

/* 503 을 보내고 바로 닫는다. 막히지 않는다(non-blocking 소켓 전제). */
void server_reject_busy(int client_fd);

#endif
