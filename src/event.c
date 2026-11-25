#include "event.h"

#include <stdlib.h>
#include <string.h>

/* 백엔드 고르기와, 모든 백엔드에 공통인 함수 호출 전달. 실제 일은 event_*.c 가 한다. */

static const struct poller_ops *find_ops(const char *name)
{
    if (strcmp(name, "poll") == 0)
        return &poller_poll_ops;
#ifdef POLLER_HAVE_EPOLL
    if (strcmp(name, "epoll") == 0)
        return &poller_epoll_ops;
#endif
#ifdef POLLER_HAVE_KQUEUE
    if (strcmp(name, "kqueue") == 0)
        return &poller_kqueue_ops;
#endif
    return NULL;
}

const char *poller_resolve(const char *name)
{
    if (!name || strcmp(name, "auto") == 0) {
#if defined(POLLER_HAVE_EPOLL)
        return poller_epoll_ops.name;
#elif defined(POLLER_HAVE_KQUEUE)
        return poller_kqueue_ops.name;
#else
        return poller_poll_ops.name;
#endif
    }
    const struct poller_ops *ops = find_ops(name);
    return ops ? ops->name : NULL;
}

struct poller *poller_new(const char *name)
{
    const char *resolved = poller_resolve(name);
    const struct poller_ops *ops = resolved ? find_ops(resolved) : NULL;
    if (!ops)
        return NULL;
    struct poller *p = ops->create();
    if (p)
        p->ops = ops;
    return p;
}

void poller_free(struct poller *p)
{
    if (p)
        p->ops->destroy(p);
}

const char *poller_name(const struct poller *p) { return p->ops->name; }

int poller_add(struct poller *p, int fd, short events, void *ptr)
{
    return p->ops->add(p, fd, events, ptr);
}

int poller_mod(struct poller *p, int fd, short events, void *ptr)
{
    return p->ops->mod(p, fd, events, ptr);
}

int poller_del(struct poller *p, int fd) { return p->ops->del(p, fd); }

int poller_wait(struct poller *p, struct poller_event *out, int max, int timeout_ms)
{
    return p->ops->wait(p, out, max, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* fd 표                                                               */
/* ------------------------------------------------------------------ */

struct fd_entry *fd_table_get(struct fd_table *t, int fd)
{
    if (fd < 0)
        return NULL;
    if (fd >= t->cap) {
        int cap = t->cap ? t->cap : 64;
        while (cap <= fd)
            cap *= 2;
        struct fd_entry *e = realloc(t->e, (size_t)cap * sizeof(*e));
        if (!e)
            return NULL;
        memset(e + t->cap, 0, (size_t)(cap - t->cap) * sizeof(*e));
        t->e = e;
        t->cap = cap;
    }
    return &t->e[fd];
}

struct fd_entry *fd_table_find(const struct fd_table *t, int fd)
{
    if (fd < 0 || fd >= t->cap || !t->e[fd].used)
        return NULL;
    return &t->e[fd];
}

void fd_table_free(struct fd_table *t)
{
    free(t->e);
    t->e = NULL;
    t->cap = 0;
}
