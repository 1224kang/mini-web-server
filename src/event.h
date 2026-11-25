#ifndef EVENT_H
#define EVENT_H

/*
 * 이벤트 백엔드 추상화 (poller).
 *
 * 이벤트 루프가 "어떤 fd 에서 무엇을 기다리는지"를 커널에 알리는 방법은 OS 마다 다르다.
 *
 *   poll   : 기다릴 때마다 fd 목록 전체를 넘긴다. 어디서나 동작한다.     매번 O(등록된 fd 수)
 *   epoll  : (Linux)  목록을 커널에 한 번 등록해 두고, 준비된 fd 만 돌려받는다. O(준비된 fd 수)
 *   kqueue : (macOS, BSD) epoll 과 같은 방식.                             O(준비된 fd 수)
 *
 * 이 파일은 그 차이를 감춘다. 루프는 "등록 → 변경 → 해제 → 기다리기" 네 가지만 쓴다.
 *
 *   poller_add(p, fd, POLLIN, ptr)   fd 를 감시 목록에 넣는다. ptr 은 이벤트가 오면 그대로 돌려준다
 *   poller_mod(p, fd, POLLOUT, ptr)  기다리는 이벤트를 바꾼다
 *   poller_del(p, fd)                감시 목록에서 뺀다. fd 를 close 하기 "전에" 불러야 한다
 *   poller_wait(p, out, max, ms)     준비된 fd 들을 out 에 채운다 (최대 ms 밀리초 기다림)
 *
 * 이벤트 비트는 poll 의 것(POLLIN, POLLOUT / 결과에는 POLLERR, POLLHUP 도)을 그대로 쓴다.
 * 동작은 level-triggered: 준비된 상태가 계속되면 poller_wait 가 매번 다시 알려 준다(poll 과 같음).
 *
 * events 를 0 으로 등록하면 poll/epoll 은 오류와 끊김(POLLERR/POLLHUP)만 알려 준다.
 * kqueue 에는 그런 등록이 없어서 아무것도 알려 주지 않는다(연결은 마감이나 다음 쓰기에서 끊김을 안다).
 *
 * 백엔드는 실행할 때 고른다(설정 event_backend). 그래서 같은 실행 파일로 poll 과 epoll/kqueue 를
 * 나란히 측정할 수 있다.
 */

#include <poll.h>

struct poller;

struct poller_event {
    int fd;
    short revents; /* POLLIN, POLLOUT, POLLERR, POLLHUP 조합 */
    void *ptr;     /* poller_add/mod 에 넘긴 값 */
};

/* 백엔드 이름("auto", "poll", "epoll", "kqueue")을 이 OS 에서 쓸 실제 이름으로 바꾼다.
 * "auto" 는 epoll 또는 kqueue, 둘 다 없으면 poll. 이 OS 에 없는 백엔드면 NULL. */
const char *poller_resolve(const char *name);

/* name 은 poller_resolve 가 돌려준 이름. 실패하면 NULL. */
struct poller *poller_new(const char *name);
void poller_free(struct poller *p);
const char *poller_name(const struct poller *p);

/* 이미 등록된 fd 를 add 하거나, 등록되지 않은 fd 를 mod/del 하면 -1 (errno = EEXIST/ENOENT). */
int poller_add(struct poller *p, int fd, short events, void *ptr);
int poller_mod(struct poller *p, int fd, short events, void *ptr);
int poller_del(struct poller *p, int fd);

/* timeout_ms 가 -1 이면 무한히 기다린다. 준비된 fd 수(0 이면 시간 초과)를 돌려준다.
 * 같은 fd 가 두 번 나올 수 있다(kqueue: 읽기와 쓰기를 따로 알림) → 받는 쪽은 revents 를 합친다.
 * 준비된 fd 가 max 개보다 많으면 나머지는 다음 호출에서 돌려준다(level-triggered).
 * 실패하면 -1 (EINTR 포함, errno 유지). */
int poller_wait(struct poller *p, struct poller_event *out, int max, int timeout_ms);

/* ---- 백엔드 구현용 ---- */

struct poller_ops {
    const char *name;
    struct poller *(*create)(void);
    void (*destroy)(struct poller *p);
    int (*add)(struct poller *p, int fd, short events, void *ptr);
    int (*mod)(struct poller *p, int fd, short events, void *ptr);
    int (*del)(struct poller *p, int fd);
    int (*wait)(struct poller *p, struct poller_event *out, int max, int timeout_ms);
};

/* 모든 백엔드의 구조체는 이걸 맨 앞에 둔다 */
struct poller {
    const struct poller_ops *ops;
};

extern const struct poller_ops poller_poll_ops;
#ifdef __linux__
#define POLLER_HAVE_EPOLL 1
extern const struct poller_ops poller_epoll_ops;
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define POLLER_HAVE_KQUEUE 1
extern const struct poller_ops poller_kqueue_ops;
#endif

/* ---- fd → (이벤트, ptr) 표: 백엔드들이 같이 쓴다 ---- */

struct fd_entry {
    void *ptr;
    short events;
    char used;
};

struct fd_table {
    struct fd_entry *e;
    int cap;
};

/* fd 칸을 돌려준다(없으면 표를 키움). 메모리가 없으면 NULL. */
struct fd_entry *fd_table_get(struct fd_table *t, int fd);
/* 등록된 fd 의 칸, 없으면 NULL. */
struct fd_entry *fd_table_find(const struct fd_table *t, int fd);
void fd_table_free(struct fd_table *t);

#endif
