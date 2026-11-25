/*
 * 헤더 가드 #ifndef MINI_WEB_SERVER_SERVER_H
 * 같은 헤더가 여러 번 #include되어도 중복 정의가 생기지 않게 막습니다.
*/
#ifndef MINI_WEB_SERVER_SERVER_H
#define MINI_WEB_SERVER_SERVER_H

#include <limits.h>
#include <stdint.h>

#include "config.h"
#include "proxy_handler.h"
#include "thread_pool.h"

#define SERVER_BACKLOG           128

/* 서버 인스턴스 상태. 서버 하나의 상태를 담는 그릇입니다.
 * 이후 Phase에서 설정(루트 디렉토리, 라우팅 규칙), 스레드 풀 등의 필드가 추가된다.
 * 나중에 루트 디렉토리, 라우팅 테이블,
 * 스레드 풀 포인터 같은 필드를 여기에 추가하면 main.c를 거의 건드리지 않고 기능을 늘릴 수 있습니다.
*/

typedef struct {
    int             listen_fd;
    server_config_t cfg;                 /* Phase 4: 포트, 타임아웃, 라우팅 규칙 등 전체 설정 */
    char            doc_root[PATH_MAX];  /* Phase 2: realpath()로 정규화된 정적 파일 루트 */
    proxy_table_t   proxy;               /* Phase 4: 라우팅 테이블 + Round-Robin 상태 */
    int             proxy_ready;
    thread_pool_t  *pool;                /* Phase 3: 워커 스레드 풀 */
    int             wake_pipe[2];        /* Phase 5: 시그널 -> 메인/워커를 깨우는 self-pipe */
} server_t;

/* 설정 적용 -> doc_root 정규화 -> 라우팅 테이블 -> 시그널 처리 등록 ->
 * socket -> setsockopt(SO_REUSEADDR) -> bind -> listen -> 스레드 풀 생성.
 * 성공 시 0, 실패 시 -1 (에러는 stderr로 출력, 만든 자원은 모두 정리됨). */
int server_init(server_t *srv, const server_config_t *cfg);

/* accept 루프. 연결을 작업 큐에 넣고 워커가 keep-alive 루프로 처리한다.
 * SIGINT/SIGTERM 을 받으면 새 연결을 그만 받고, 처리 중인 요청을 마친 뒤 0 을 반환한다. */
int server_run(server_t *srv);

/* 리스닝 소켓, 파이프, 스레드 풀, 라우팅 테이블 등 서버 자원 해제. 여러 번 호출해도 안전하다. */
void server_destroy(server_t *srv);

#endif /* MINI_WEB_SERVER_SERVER_H */
