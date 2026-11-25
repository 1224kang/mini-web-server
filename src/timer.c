#include "timer.h"

#include <stdlib.h>

/*
 * 배열로 만든 이진 힙. a[i] 의 자식은 a[2i+1], a[2i+2], 부모는 a[(i-1)/2].
 * 규칙: 부모의 when <= 자식의 when  →  a[0] 이 가장 이르다.
 */

void timer_node_init(struct timer_node *t)
{
    t->when = 0;
    t->pos = -1;
}

static void place(struct timer_heap *h, int i, struct timer_node *t)
{
    h->a[i] = t;
    t->pos = i;
}

/* a[i] 가 부모보다 이르면 위로 올린다. */
static void sift_up(struct timer_heap *h, int i)
{
    struct timer_node *t = h->a[i];
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (h->a[parent]->when <= t->when)
            break;
        place(h, i, h->a[parent]);
        i = parent;
    }
    place(h, i, t);
}

/* a[i] 가 자식보다 늦으면 아래로 내린다. */
static void sift_down(struct timer_heap *h, int i)
{
    struct timer_node *t = h->a[i];
    for (;;) {
        int child = 2 * i + 1;
        if (child >= h->n)
            break;
        if (child + 1 < h->n && h->a[child + 1]->when < h->a[child]->when)
            child++;
        if (t->when <= h->a[child]->when)
            break;
        place(h, i, h->a[child]);
        i = child;
    }
    place(h, i, t);
}

int timer_heap_reserve(struct timer_heap *h, int cap)
{
    if (cap <= h->cap)
        return 0;
    struct timer_node **a = realloc(h->a, (size_t)cap * sizeof(*a));
    if (!a)
        return -1;
    h->a = a;
    h->cap = cap;
    return 0;
}

int timer_set(struct timer_heap *h, struct timer_node *t, long long when)
{
    if (t->pos >= 0) { /* 이미 힙에 있음: 마감만 바꾸고 자리 옮기기 */
        long long old = t->when;
        t->when = when;
        if (when < old)
            sift_up(h, t->pos);
        else if (when > old)
            sift_down(h, t->pos);
        return 0;
    }
    if (h->n == h->cap) {
        int cap = h->cap ? h->cap * 2 : 64;
        struct timer_node **a = realloc(h->a, (size_t)cap * sizeof(*a));
        if (!a)
            return -1;
        h->a = a;
        h->cap = cap;
    }
    t->when = when;
    place(h, h->n++, t);
    sift_up(h, t->pos);
    return 0;
}

void timer_remove(struct timer_heap *h, struct timer_node *t)
{
    int i = t->pos;
    if (i < 0)
        return;
    t->pos = -1;
    int last = --h->n;
    if (i == last)
        return;
    /* 맨 뒤 노드를 빈자리에 넣고, 그 노드가 올라가야 할지 내려가야 할지 본다 */
    place(h, i, h->a[last]);
    if (i > 0 && h->a[(i - 1) / 2]->when > h->a[i]->when)
        sift_up(h, i);
    else
        sift_down(h, i);
}

struct timer_node *timer_top(const struct timer_heap *h)
{
    return h->n > 0 ? h->a[0] : NULL;
}

void timer_heap_free(struct timer_heap *h)
{
    free(h->a);
    h->a = NULL;
    h->n = h->cap = 0;
}
