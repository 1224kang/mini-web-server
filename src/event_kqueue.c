#include "event.h"

#ifdef POLLER_HAVE_KQUEUE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * kqueue 백엔드 (macOS, BSD).
 *
 * 감시 목록은 커널(kq)이 들고 있다. kevent() 로 등록을 바꾸고, kevent() 로 준비된 것만 받는다.
 *
 * poll 과 다른 점:
 *   - 읽기와 쓰기가 따로따로 등록된다(필터). POLLIN → EVFILT_READ, POLLOUT → EVFILT_WRITE.
 *     그래서 한 fd 에 대해 결과가 두 개(읽기 하나, 쓰기 하나) 올 수 있다.
 *   - events 가 0 이면 등록할 필터가 없다 → 끊김도 알려 주지 않는다 (event.h 참고).
 *   - 상대가 끊으면 결과에 EV_EOF 가 붙는다 → POLLHUP 으로 바꿔 준다(오류가 있으면 POLLERR 도).
 *
 * 우리가 든 표(fd_table): fd 마다 지금 등록한 events 와 ptr.
 *   - mod 때 "어느 필터를 더하고 뺄지" 계산하려면 이전 events 를 알아야 한다.
 */

struct kq_poller {
    struct poller base;
    int kq;
    struct fd_table fds;
    struct kevent *buf; /* kevent 결과를 받을 칸 */
    int buf_cap;
};

static struct poller *kq_create(void)
{
    struct kq_poller *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->kq = kqueue();
    if (p->kq < 0) {
        free(p);
        return NULL;
    }
    fcntl(p->kq, F_SETFD, FD_CLOEXEC);
    return &p->base;
}

static void kq_destroy(struct poller *base)
{
    struct kq_poller *p = (struct kq_poller *)base;
    close(p->kq);
    fd_table_free(&p->fds);
    free(p->buf);
    free(p);
}

/* 필터 하나를 더하거나(EV_ADD: 이미 있으면 ptr 만 바뀜) 뺀다(EV_DELETE). */
static int kq_change(struct kq_poller *p, int fd, short filter, unsigned short flags, void *ptr)
{
    struct kevent ch;
    EV_SET(&ch, (uintptr_t)fd, filter, flags, 0, 0, ptr);
    if (kevent(p->kq, &ch, 1, NULL, 0, NULL) == 0)
        return 0;
    /* 빼려는데 이미 없음(ENOENT)이나 fd 가 이미 닫힘(EBADF): 목표 상태와 같으니 성공으로 본다 */
    if ((flags & EV_DELETE) && (errno == ENOENT || errno == EBADF))
        return 0;
    return -1;
}

/* 등록 상태를 old 에서 new 로 바꾼다. ptr 이 바뀌었으면 남는 필터도 다시 EV_ADD 해서 ptr 을 갱신한다. */
static int kq_apply(struct kq_poller *p, int fd, short old_ev, void *old_ptr, short new_ev, void *new_ptr)
{
    static const struct { short bit; short filter; } map[] = {
        { POLLIN, EVFILT_READ },
        { POLLOUT, EVFILT_WRITE },
    };
    for (int i = 0; i < 2; i++) {
        int had = (old_ev & map[i].bit) != 0;
        int want = (new_ev & map[i].bit) != 0;
        int rc = 0;
        if (want && (!had || old_ptr != new_ptr))
            rc = kq_change(p, fd, map[i].filter, EV_ADD, new_ptr);
        else if (!want && had)
            rc = kq_change(p, fd, map[i].filter, EV_DELETE, NULL);
        if (rc < 0)
            return -1;
    }
    return 0;
}

static int kq_add(struct poller *base, int fd, short events, void *ptr)
{
    struct kq_poller *p = (struct kq_poller *)base;
    if (fd_table_find(&p->fds, fd)) {
        errno = EEXIST;
        return -1;
    }
    struct fd_entry *e = fd_table_get(&p->fds, fd);
    if (!e) {
        errno = fd < 0 ? EBADF : ENOMEM;
        return -1;
    }
    if (kq_apply(p, fd, 0, NULL, events, ptr) < 0) {
        int err = errno;
        kq_apply(p, fd, events, ptr, 0, NULL); /* 일부만 붙었으면 떼어 낸다 */
        errno = err;
        return -1;
    }
    *e = (struct fd_entry){ .ptr = ptr, .events = events, .used = 1 };
    return 0;
}

static int kq_mod(struct poller *base, int fd, short events, void *ptr)
{
    struct kq_poller *p = (struct kq_poller *)base;
    struct fd_entry *e = fd_table_find(&p->fds, fd);
    if (!e) {
        errno = ENOENT;
        return -1;
    }
    if (kq_apply(p, fd, e->events, e->ptr, events, ptr) < 0)
        return -1;
    e->events = events;
    e->ptr = ptr;
    return 0;
}

static int kq_del(struct poller *base, int fd)
{
    struct kq_poller *p = (struct kq_poller *)base;
    struct fd_entry *e = fd_table_find(&p->fds, fd);
    if (!e) {
        errno = ENOENT;
        return -1;
    }
    int rc = kq_apply(p, fd, e->events, e->ptr, 0, NULL);
    *e = (struct fd_entry){ 0 }; /* 실패했어도 우리 쪽 등록은 지운다(fd 가 곧 닫힌다) */
    return rc;
}

static int kq_wait(struct poller *base, struct poller_event *out, int max, int timeout_ms)
{
    struct kq_poller *p = (struct kq_poller *)base;
    if (max > p->buf_cap) {
        struct kevent *b = realloc(p->buf, (size_t)max * sizeof(*b));
        if (!b) {
            errno = ENOMEM;
            return -1;
        }
        p->buf = b;
        p->buf_cap = max;
    }
    struct timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    /* 준비된 필터만 돌아온다 → 등록된 fd 가 몇 개든 O(준비된 수) */
    int n = kevent(p->kq, NULL, 0, p->buf, max, tsp);
    if (n <= 0)
        return n;

    for (int i = 0; i < n; i++) {
        const struct kevent *k = &p->buf[i];
        short rev = 0;
        if (k->filter == EVFILT_READ)
            rev |= POLLIN;
        else if (k->filter == EVFILT_WRITE)
            rev |= POLLOUT;
        if (k->flags & EV_EOF)
            rev |= POLLHUP;
        if ((k->flags & EV_ERROR) || ((k->flags & EV_EOF) && k->fflags))
            rev |= POLLERR;
        out[i] = (struct poller_event){ .fd = (int)k->ident, .revents = rev, .ptr = k->udata };
    }
    return n;
}

const struct poller_ops poller_kqueue_ops = {
    .name = "kqueue",
    .create = kq_create,
    .destroy = kq_destroy,
    .add = kq_add,
    .mod = kq_mod,
    .del = kq_del,
    .wait = kq_wait,
};

#else
/* 이 OS 에는 없는 백엔드: 빈 파일이 되지 않게(ISO C) 선언 하나만 둔다 */
typedef int event_kqueue_unavailable;
#endif /* POLLER_HAVE_KQUEUE */
