#ifndef EVENT_H
#define EVENT_H

/*
 * 이벤트 백엔드 추상화 (poller).
 *
 * 이벤트 루프가 "어떤 fd 에서 무엇을 기다리는지"를 커널에 알리는 방법은 OS 마다 다르다.
 *
 *   poll   : 기다릴 때마다 fd 목록 전체를 넘긴다. 어디서나 동작한다.
 *   epoll  : (Linux)  목록을 커널에 한 번 등록해 두고, 준비된 fd 만 돌려받는다.
 *   kqueue : (macOS)  epoll 과 같은 방식.
 *
 * 이 파일은 그 차이를 감춘다. 루프는 "등록 → 변경 → 해제 → 기다리기" 네 가지만 쓴다.
 *
 *   poller_add(p, fd, POLLIN, ptr)   fd 를 감시 목록에 넣는다. ptr 은 이벤트가 오면 그대로 돌려준다
 *   poller_mod(p, fd, POLLOUT, ptr)  기다리는 이벤트를 바꾼다
 *   poller_del(p, fd)                감시 목록에서 뺀다. fd 를 close 하기 "전에" 불러야 한다
 *   poller_wait(p, out, max, ms)     준비된 fd 들을 out 에 채운다 (최대 ms 밀리초 기다림)
 *
 * 이벤트 비트는 poll 의 것(POLLIN, POLLOUT / 결과에는 POLLERR, POLLHUP, POLLNVAL 도)을 그대로 쓴다.
 * 동작은 level-triggered: 준비된 상태가 계속되면 poller_wait 가 매번 다시 알려 준다(poll 과 같음).
 *
 * 지금은 poll 백엔드만 있다. epoll/kqueue 는 같은 함수를 다른 파일에서 구현해 붙인다.
 */

#include <poll.h>

struct poller;

struct poller_event {
    int fd;
    short revents; /* POLLIN, POLLOUT, POLLERR, POLLHUP, POLLNVAL 조합 */
    void *ptr;     /* poller_add/mod 에 넘긴 값 */
};

/* 실패하면 NULL. */
struct poller *poller_new(void);
void poller_free(struct poller *p);

/* 이미 등록된 fd 를 add 하거나, 등록되지 않은 fd 를 mod/del 하면 -1 (errno = EEXIST/ENOENT). */
int poller_add(struct poller *p, int fd, short events, void *ptr);
int poller_mod(struct poller *p, int fd, short events, void *ptr);
int poller_del(struct poller *p, int fd);

/* timeout_ms 가 -1 이면 무한히 기다린다. 준비된 fd 수(0 이면 시간 초과)를 돌려준다.
 * 준비된 fd 가 max 개보다 많으면 나머지는 다음 호출에서 돌려준다(level-triggered).
 * 실패하면 -1 (EINTR 포함, errno 유지). */
int poller_wait(struct poller *p, struct poller_event *out, int max, int timeout_ms);

/* 로그용 백엔드 이름: "poll", "epoll", "kqueue" */
const char *poller_backend(void);

#endif
