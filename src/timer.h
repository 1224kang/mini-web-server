#ifndef TIMER_H
#define TIMER_H

/*
 * 타이머 힙: 마감 시각이 가장 이른 것이 항상 맨 위(a[0])에 있는 최소 힙.
 *
 * 이벤트 루프는 연결마다 마감 시각(conn_deadline)이 있다. 루프가 알아야 하는 건 두 가지뿐이다.
 *   1) 가장 가까운 마감은 언제인가    → 얼마나 잘지(poller_wait 의 timeout)
 *   2) 마감이 지난 연결은 누구인가    → conn_timeout 을 부를 대상
 * 모든 연결을 훑으면 O(연결 수). 힙이면 1) 은 맨 위 하나만 보면 되고(O(1)),
 * 2) 는 지난 것만 꺼내면 된다(꺼낼 때마다 O(log 연결 수)).
 *
 * 노드는 연결 쪽 구조체 안에 넣어 둔다(struct watch 의 timer). 노드가 자기 위치(pos)를 알고 있어서
 *   - 마감이 바뀌면 그 자리에서 위/아래로 옮기고(O(log n)),
 *   - 연결이 끝나면 바로 뺀다(O(log n)).
 * 그래서 힙 안에 "이미 지나간 옛 마감"이나 "이미 해제된 연결"이 남지 않는다.
 */

struct timer_node {
    long long when; /* 마감 시각(ms, now_ms 기준) */
    int pos;        /* 힙 배열 안 위치. -1 이면 힙에 없음 */
};

struct timer_heap {
    struct timer_node **a;
    int n, cap;
};

/* 노드를 cap 개까지 넣어도 timer_set 이 메모리를 새로 잡지 않게 미리 잡아 둔다. 실패하면 -1. */
int timer_heap_reserve(struct timer_heap *h, int cap);

/* 노드를 처음 쓰기 전에 부른다(pos = -1). */
void timer_node_init(struct timer_node *t);

/* 노드의 마감을 when 으로 정한다. 힙에 없으면 넣고, 있으면 자리를 옮긴다. 메모리가 없으면 -1. */
int timer_set(struct timer_heap *h, struct timer_node *t, long long when);

/* 힙에 있으면 뺀다. */
void timer_remove(struct timer_heap *h, struct timer_node *t);

/* 가장 이른 노드. 비었으면 NULL. */
struct timer_node *timer_top(const struct timer_heap *h);

void timer_heap_free(struct timer_heap *h);

#endif
