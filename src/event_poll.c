#include "event.h"

#include <errno.h>
#include <stdlib.h>

/*
 * poll 백엔드.
 *
 * poll 은 커널에 "등록"하는 개념이 없다. 그래서 등록 목록을 우리가 배열로 들고 있다가
 * poller_wait 때마다 배열 전체를 poll 에 넘긴다. 매번 O(등록된 fd 수).
 *
 *   pfds[0..n)  : poll 에 넘길 배열 (순서 없음)
 *   ptrs[0..n)  : pfds[i] 에 딸린 ptr
 *   slot[fd]    : fd 가 pfds 의 몇 번째 칸인지 + 1 (0 이면 등록 안 됨)
 *                 → mod/del 이 배열을 훑지 않고 O(1)
 */

struct poller {
    struct pollfd *pfds;
    void **ptrs;
    int n, cap;
    int *slot;
    int slot_cap;
    int start; /* 준비된 fd 가 max 보다 많을 때 다음 호출은 여기부터 (뒤쪽 fd 가 밀리지 않게) */
};

struct poller *poller_new(void)
{
    return calloc(1, sizeof(struct poller));
}

void poller_free(struct poller *p)
{
    if (!p)
        return;
    free(p->pfds);
    free(p->ptrs);
    free(p->slot);
    free(p);
}

const char *poller_backend(void)
{
    return "poll";
}

static int slot_of(const struct poller *p, int fd)
{
    return fd < p->slot_cap ? p->slot[fd] - 1 : -1;
}

/* slot[fd] 를 쓸 수 있게 배열을 키운다. */
static int grow_slot(struct poller *p, int fd)
{
    if (fd < p->slot_cap)
        return 0;
    int cap = p->slot_cap ? p->slot_cap : 64;
    while (cap <= fd)
        cap *= 2;
    int *s = realloc(p->slot, (size_t)cap * sizeof(*s));
    if (!s)
        return -1;
    for (int i = p->slot_cap; i < cap; i++)
        s[i] = 0;
    p->slot = s;
    p->slot_cap = cap;
    return 0;
}

static int grow_list(struct poller *p)
{
    if (p->n < p->cap)
        return 0;
    int cap = p->cap ? p->cap * 2 : 64;
    struct pollfd *f = realloc(p->pfds, (size_t)cap * sizeof(*f));
    if (!f)
        return -1;
    p->pfds = f;
    void **q = realloc(p->ptrs, (size_t)cap * sizeof(*q));
    if (!q)
        return -1;
    p->ptrs = q;
    p->cap = cap;
    return 0;
}

int poller_add(struct poller *p, int fd, short events, void *ptr)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (slot_of(p, fd) >= 0) {
        errno = EEXIST;
        return -1;
    }
    if (grow_slot(p, fd) < 0 || grow_list(p) < 0) {
        errno = ENOMEM;
        return -1;
    }
    p->pfds[p->n] = (struct pollfd){ .fd = fd, .events = events };
    p->ptrs[p->n] = ptr;
    p->slot[fd] = ++p->n;
    return 0;
}

int poller_mod(struct poller *p, int fd, short events, void *ptr)
{
    int i = fd >= 0 ? slot_of(p, fd) : -1;
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    p->pfds[i].events = events;
    p->ptrs[i] = ptr;
    return 0;
}

int poller_del(struct poller *p, int fd)
{
    int i = fd >= 0 ? slot_of(p, fd) : -1;
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    /* 맨 뒤 칸으로 빈자리를 메운다(순서는 상관없음) */
    int last = --p->n;
    if (i != last) {
        p->pfds[i] = p->pfds[last];
        p->ptrs[i] = p->ptrs[last];
        p->slot[p->pfds[i].fd] = i + 1;
    }
    p->slot[fd] = 0;
    return 0;
}

int poller_wait(struct poller *p, struct poller_event *out, int max, int timeout_ms)
{
    int r = poll(p->pfds, (nfds_t)p->n, timeout_ms);
    if (r <= 0)
        return r;

    /* poll 은 "몇 개가 준비됐는지"만 알려 준다. 어느 것인지는 배열 전체를 훑어야 안다. */
    int got = 0;
    if (p->start >= p->n)
        p->start = 0;
    for (int k = 0; k < p->n; k++) {
        int i = (p->start + k) % p->n;
        if (!p->pfds[i].revents)
            continue;
        if (got == max) {
            p->start = i; /* 다음 호출은 못 돌려준 이 칸부터 */
            return got;
        }
        out[got++] = (struct poller_event){
            .fd = p->pfds[i].fd,
            .revents = p->pfds[i].revents,
            .ptr = p->ptrs[i],
        };
    }
    return got;
}
