/* src/timer.c 단위 테스트: 타이머 힙이 항상 가장 이른 마감을 맨 위에 두는지.
 * 실행: make test */
#include "timer.h"

#include <stdio.h>
#include <stdlib.h>

static int g_failed, g_total;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_total++;                                                        \
        if (!(cond)) {                                                    \
            g_failed++;                                                   \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

/* 힙 규칙(부모 <= 자식)과 pos 가 실제 위치와 같은지 */
static int heap_ok(const struct timer_heap *h)
{
    for (int i = 0; i < h->n; i++) {
        if (h->a[i]->pos != i)
            return 0;
        if (i > 0 && h->a[(i - 1) / 2]->when > h->a[i]->when)
            return 0;
    }
    return 1;
}

static void test_basic(void)
{
    struct timer_heap h = { 0 };
    struct timer_node a, b, c;
    timer_node_init(&a);
    timer_node_init(&b);
    timer_node_init(&c);
    CHECK(timer_top(&h) == NULL);

    timer_set(&h, &a, 300);
    timer_set(&h, &b, 100);
    timer_set(&h, &c, 200);
    CHECK(timer_top(&h) == &b);

    timer_set(&h, &b, 400); /* 마감이 늦춰짐 → 아래로 */
    CHECK(timer_top(&h) == &c);
    timer_set(&h, &a, 50); /* 마감이 당겨짐 → 위로 */
    CHECK(timer_top(&h) == &a);

    timer_remove(&h, &a);
    CHECK(a.pos == -1);
    CHECK(timer_top(&h) == &c);
    timer_remove(&h, &a); /* 없는 걸 또 빼도 괜찮다 */
    CHECK(h.n == 2);

    timer_remove(&h, &c);
    timer_remove(&h, &b);
    CHECK(timer_top(&h) == NULL);
    CHECK(heap_ok(&h));
    timer_heap_free(&h);
}

/* 무작위로 넣고/바꾸고/빼면서, 맨 위가 항상 전체에서 가장 이른 것인지 단순 비교로 확인 */
static void test_random(void)
{
    enum { N = 500, STEPS = 20000 };
    static struct timer_node nodes[N];
    struct timer_heap h = { 0 };
    for (int i = 0; i < N; i++)
        timer_node_init(&nodes[i]);
    srand(12345);

    int bad_top = 0, bad_heap = 0;
    for (int step = 0; step < STEPS; step++) {
        struct timer_node *t = &nodes[rand() % N];
        if (rand() % 4 == 0)
            timer_remove(&h, t);
        else
            timer_set(&h, t, rand() % 10000);

        long long min = -1;
        int count = 0;
        for (int i = 0; i < N; i++) {
            if (nodes[i].pos < 0)
                continue;
            count++;
            if (min < 0 || nodes[i].when < min)
                min = nodes[i].when;
        }
        struct timer_node *top = timer_top(&h);
        if (count != h.n || (count == 0 ? top != NULL : top->when != min))
            bad_top++;
        if (step % 100 == 0 && !heap_ok(&h))
            bad_heap++;
    }
    CHECK(bad_top == 0);
    CHECK(bad_heap == 0);
    CHECK(heap_ok(&h));

    /* 맨 위부터 하나씩 꺼내면 오름차순이어야 한다 */
    long long prev = -1;
    int sorted = 1;
    while (timer_top(&h)) {
        struct timer_node *t = timer_top(&h);
        if (t->when < prev)
            sorted = 0;
        prev = t->when;
        timer_remove(&h, t);
    }
    CHECK(sorted);
    timer_heap_free(&h);
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "basic",  test_basic },
        { "random", test_random },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failed;
        tests[i].fn();
        printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
