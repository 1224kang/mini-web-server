#ifndef SERVER_H
#define SERVER_H

struct thread_pool;

/* 모든 워커가 같이 보는 서버 설정. 워커는 읽기만 한다. */
struct server_ctx {
    const char *docroot;      /* realpath 로 정규화된 절대 경로(끝에 '/' 없음) */
    int stop_fd;              /* 이 fd 가 "읽을 수 있음"이 되면 = 종료 신호가 왔다 */
    struct thread_pool *pool; /* 대기 중인 연결 수를 보고 keep-alive 여부를 정한다 */
};

/* 워커들이 함께 갱신하는 통계 (mutex 로 보호). */
struct server_stats {
    unsigned long connections; /* 지금까지 처리를 시작한 연결 수 */
    unsigned long requests;    /* 지금까지 파싱에 성공한 요청 수 */
    unsigned long active;      /* 지금 처리 중인 연결 수 */
};

/* port 에서 listen 하는 소켓을 만들어 fd 를 반환한다. 실패하면 -1. */
int server_listen(int port);

/* 워커 스레드가 호출: 연결 하나를 끝까지 처리하고 닫는다.
 * keep-alive 면 같은 연결에서 요청을 여러 개 받는다.
 * GET/HEAD → docroot 아래 정적 파일, POST → 본문 echo, 그 외 → 405.
 * arg 는 struct server_ctx *. (thread_pool 의 pool_job_fn 모양에 맞춤) */
void server_handle_client(int client_fd, int worker_id, void *arg);

/* 큐가 꽉 찼을 때 accept 스레드가 호출: 503 을 보내고 바로 닫는다. 막히지 않는다. */
void server_reject_busy(int client_fd);

void server_stats_get(struct server_stats *out);

#endif
