#ifndef CONN_H
#define CONN_H

/*
 * 연결 하나의 상태 머신.
 *
 * 이벤트 루프(server.c)는 연결 내부를 모른다. 이 다섯 가지만 묻고 시킨다.
 *   conn_events()   : "지금 뭘 기다려?"       → POLLIN(읽을 것) / POLLOUT(쓸 자리)
 *   conn_deadline() : "언제까지 기다려 줄까?"  → 마감 시각(ms)
 *   conn_handle()   : "기다리던 게 왔어"       → 할 수 있는 만큼만 진행하고 바로 돌아온다
 *   conn_timeout()  : "마감 지났어"           → 408 을 보내거나 닫는다
 *   conn_done()     : "끝났어?"              → 1 이면 루프가 conn_free 로 정리
 *
 * 상태 흐름:
 *
 *   READ_HEAD ──헤더 완성──▶ (본문 있음) READ_BODY ──본문 완성──┐
 *       ▲          │                                         ▼
 *       │          └──────────(본문 없음)───────────────▶  WRITE
 *       │                                                    │ 응답 다 보냄
 *       └──────────────── keep-alive ────────────────────────┤
 *                                                            ▼ close
 *                                       LINGER(남은 입력 비우기) ──▶ DONE
 *
 * 핵심 규칙: 어떤 함수도 소켓을 "기다리지" 않는다(non-blocking).
 * 지금 못 하는 일은 상태와 위치를 struct conn 에 적어 두고 루프로 돌아간다.
 */

struct conn;

/* 단조 증가 시계(ms). 마감 시각 계산에 쓴다. */
long long now_ms(void);

/* non-blocking 소켓 fd 로 새 연결을 만든다. 실패하면 NULL (fd 는 호출한 쪽이 닫는다). */
struct conn *conn_new(int fd, unsigned long id, const char *docroot, long long now);

int conn_fd(const struct conn *c);
short conn_events(const struct conn *c);
long long conn_deadline(const struct conn *c);
void conn_handle(struct conn *c, short revents, long long now);
void conn_timeout(struct conn *c, long long now);
int conn_done(const struct conn *c);

/* 서버 종료 시작: 다음 요청을 기다리며 놀던 연결은 바로 닫고,
 * 요청을 처리 중인 연결은 응답을 끝까지 보낸 뒤 닫히게 한다. */
void conn_shutdown(struct conn *c, long long now);

/* fd 를 닫고 메모리를 푼다. */
void conn_free(struct conn *c);

/* 지금까지 파싱에 성공한 요청 수 (통계용). */
unsigned long conn_total_requests(void);

#endif
