#include "event.h"

#ifdef POLLER_HAVE_EPOLL

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

/*
 * epoll 백엔드 (Linux).
 *
 * 감시 목록은 커널(ep)이 들고 있다. epoll_ctl 로 등록을 바꾸고, epoll_wait 로 준비된 것만 받는다.
 *
 * poll 과 다른 점:
 *   - epoll_event.data 는 union 이라 fd 와 ptr 을 함께 담을 수 없다. data.fd 에 fd 를 담고,
 *     ptr 은 우리 표(fd_table)에서 찾는다.
 *   - 등록은 (fd 번호, 그 fd 가 가리키는 열린 파일) 쌍에 붙는다. fork 로 자식이 같은 열린 파일을
 *     들고 있으면, 부모가 fd 를 close 해도 등록이 사라지지 않고 이벤트가 계속 온다.
 *     → close 하기 전에 반드시 poller_del (conn.c 의 close 훅이 해 준다).
 *   - events 가 0 이어도 EPOLLERR/EPOLLHUP 은 알려 준다(poll 과 같음).
 */

struct ep_poller {
    struct poller base;
    int ep;
    struct fd_table fds;
    struct epoll_event *buf;
    int buf_cap;
};

static struct poller *ep_create(void)
{
    struct ep_poller *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->ep = epoll_create1(EPOLL_CLOEXEC); /* CGI 자식에게 epoll fd 가 새지 않게 */
    if (p->ep < 0) {
        free(p);
        return NULL;
    }
    return &p->base;
}

static void ep_destroy(struct poller *base)
{
    struct ep_poller *p = (struct ep_poller *)base;
    close(p->ep);
    fd_table_free(&p->fds);
    free(p->buf);
    free(p);
}

static unsigned to_epoll(short events)
{
    unsigned ev = 0;
    if (events & POLLIN)
        ev |= EPOLLIN;
    if (events & POLLOUT)
        ev |= EPOLLOUT;
    return ev;
}

static int ep_ctl(struct ep_poller *p, int op, int fd, short events)
{
    struct epoll_event ev = { .events = to_epoll(events), .data.fd = fd };
    return epoll_ctl(p->ep, op, fd, &ev);
}

static int ep_add(struct poller *base, int fd, short events, void *ptr)
{
    struct ep_poller *p = (struct ep_poller *)base;
    if (fd_table_find(&p->fds, fd)) {
        errno = EEXIST;
        return -1;
    }
    struct fd_entry *e = fd_table_get(&p->fds, fd);
    if (!e) {
        errno = fd < 0 ? EBADF : ENOMEM;
        return -1;
    }
    if (ep_ctl(p, EPOLL_CTL_ADD, fd, events) < 0)
        return -1;
    *e = (struct fd_entry){ .ptr = ptr, .events = events, .used = 1 };
    return 0;
}

static int ep_mod(struct poller *base, int fd, short events, void *ptr)
{
    struct ep_poller *p = (struct ep_poller *)base;
    struct fd_entry *e = fd_table_find(&p->fds, fd);
    if (!e) {
        errno = ENOENT;
        return -1;
    }
    if (e->events != events && ep_ctl(p, EPOLL_CTL_MOD, fd, events) < 0)
        return -1;
    e->events = events;
    e->ptr = ptr; /* ptr 은 우리 표에만 있으므로 커널에 알릴 필요 없다 */
    return 0;
}

static int ep_del(struct poller *base, int fd)
{
    struct ep_poller *p = (struct ep_poller *)base;
    struct fd_entry *e = fd_table_find(&p->fds, fd);
    if (!e) {
        errno = ENOENT;
        return -1;
    }
    *e = (struct fd_entry){ 0 };
    int rc = ep_ctl(p, EPOLL_CTL_DEL, fd, 0);
    /* fd 가 이미 닫혔으면(EBADF) 커널 쪽 등록도 없는 것으로 본다 */
    return rc < 0 && errno != EBADF && errno != ENOENT ? -1 : 0;
}

static int ep_wait(struct poller *base, struct poller_event *out, int max, int timeout_ms)
{
    struct ep_poller *p = (struct ep_poller *)base;
    if (max > p->buf_cap) {
        struct epoll_event *b = realloc(p->buf, (size_t)max * sizeof(*b));
        if (!b) {
            errno = ENOMEM;
            return -1;
        }
        p->buf = b;
        p->buf_cap = max;
    }
    /* 준비된 fd 만 돌아온다 → 등록된 fd 가 몇 개든 O(준비된 수) */
    int n = epoll_wait(p->ep, p->buf, max, timeout_ms);
    if (n <= 0)
        return n;

    int got = 0;
    for (int i = 0; i < n; i++) {
        int fd = p->buf[i].data.fd;
        const struct fd_entry *e = fd_table_find(&p->fds, fd);
        if (!e) /* 이론상 없음: 등록을 지운 뒤에는 이벤트가 오지 않는다 */
            continue;
        unsigned ev = p->buf[i].events;
        short rev = 0;
        if (ev & EPOLLIN)
            rev |= POLLIN;
        if (ev & EPOLLOUT)
            rev |= POLLOUT;
        if (ev & EPOLLERR)
            rev |= POLLERR;
        if (ev & EPOLLHUP)
            rev |= POLLHUP;
        out[got++] = (struct poller_event){ .fd = fd, .revents = rev, .ptr = e->ptr };
    }
    return got;
}

const struct poller_ops poller_epoll_ops = {
    .name = "epoll",
    .create = ep_create,
    .destroy = ep_destroy,
    .add = ep_add,
    .mod = ep_mod,
    .del = ep_del,
    .wait = ep_wait,
};

#else
/* 이 OS 에는 없는 백엔드: 빈 파일이 되지 않게(ISO C) 선언 하나만 둔다 */
typedef int event_epoll_unavailable;
#endif /* POLLER_HAVE_EPOLL */
