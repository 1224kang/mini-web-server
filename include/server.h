/*
 * 헤더 가드 #ifndef MINI_WEB_SERVER_SERVER_H
 * 같은 헤더가 여러 번 #include되어도 중복 정의가 생기지 않게 막습니다.
*/
#ifndef MINI_WEB_SERVER_SERVER_H
#define MINI_WEB_SERVER_SERVER_H

#include <limits.h>
#include <stdint.h>

#define SERVER_DEFAULT_PORT      8080
#define SERVER_DEFAULT_DOC_ROOT  "./www"
#define SERVER_BACKLOG           128

/* 서버 인스턴스 상태. 서버 하나의 상태를 담는 그릇입니다.
 * 이후 Phase에서 설정(루트 디렉토리, 라우팅 규칙), 스레드 풀 등의 필드가 추가된다.
 * 나중에 루트 디렉토리, 라우팅 테이블,
 * 스레드 풀 포인터 같은 필드를 여기에 추가하면 main.c를 거의 건드리지 않고 기능을 늘릴 수 있습니다.
*/

typedef struct {
    int      listen_fd;
    uint16_t port;
    char     doc_root[PATH_MAX];   /* Phase 2: realpath()로 정규화된 정적 파일 루트 */
} server_t;

/* doc_root 정규화 -> socket -> setsockopt(SO_REUSEADDR) -> bind -> listen.
 * 성공 시 0, 실패 시 -1 (에러는 stderr로 출력). */
int server_init(server_t *srv, uint16_t port, const char *doc_root);

/* accept 루프. 연결마다 HTTP 요청 하나를 파싱해 정적 파일로 응답하고 연결을 닫는다.
 * 치명적 오류가 아니면 반환하지 않는다. 오류 시 -1. */
int server_run(server_t *srv);

/* 리스닝 소켓 등 서버 자원 해제. 여러 번 호출해도 안전하다. */
void server_destroy(server_t *srv);

#endif /* MINI_WEB_SERVER_SERVER_H */
