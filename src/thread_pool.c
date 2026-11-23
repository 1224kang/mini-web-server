#include "thread_pool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct thread_pool {
    /* ---- mu 로 보호되는 것들 (반드시 lock 을 잡고 읽고 쓴다) ---- */
    pthread_mutex_t mu;
    pthread_cond_t not_empty; /* "큐에 뭔가 들어왔다" 또는 "종료한다" 신호 */
    int *queue;               /* 원형 버퍼: head 에서 꺼내고 (head+count)%cap 에 넣는다 */
    size_t cap, head, count;
    int stopping;

    /* ---- 만든 뒤엔 읽기만 하므로 lock 이 필요 없는 것들 ---- */
    pthread_t *threads;
    int nthreads;
    pool_job_fn fn;
    void *arg;
};

struct worker_start {
    struct thread_pool *pool;
    int id;
};

static void *worker_main(void *raw)
{
    struct worker_start ws = *(struct worker_start *)raw;
    free(raw);
    struct thread_pool *p = ws.pool;

    for (;;) {
        pthread_mutex_lock(&p->mu);
        /* while 이어야 한다(if 가 아니라):
         *  - 깨어났는데 다른 워커가 먼저 가져가 큐가 또 비었을 수 있고
         *  - 아무 이유 없이 깨어나는 spurious wakeup 도 허용되어 있다. */
        while (p->count == 0 && !p->stopping)
            pthread_cond_wait(&p->not_empty, &p->mu); /* 잠들면서 mu 를 놓고, 깨면 다시 잡는다 */

        if (p->stopping) {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        int fd = p->queue[p->head];
        p->head = (p->head + 1) % p->cap;
        p->count--;
        pthread_mutex_unlock(&p->mu);

        /* 실제 처리는 lock 밖에서! lock 을 쥔 채 처리하면 다른 워커가 전부 멈춘다. */
        p->fn(fd, ws.id, p->arg);
    }
    return NULL;
}

struct thread_pool *pool_create(int nthreads, size_t queue_cap, pool_job_fn fn, void *arg)
{
    if (nthreads < 1 || queue_cap < 1)
        return NULL;

    struct thread_pool *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->queue = calloc(queue_cap, sizeof(int));
    p->threads = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!p->queue || !p->threads) {
        free(p->queue);
        free(p->threads);
        free(p);
        return NULL;
    }
    p->cap = queue_cap;
    p->fn = fn;
    p->arg = arg;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->not_empty, NULL);

    for (int i = 0; i < nthreads; i++) {
        struct worker_start *ws = malloc(sizeof(*ws));
        int err = ws ? 0 : -1;
        if (ws) {
            ws->pool = p;
            ws->id = i;
            err = pthread_create(&p->threads[i], NULL, worker_main, ws);
        }
        if (err != 0) {
            fprintf(stderr, "pthread_create failed (worker %d)\n", i);
            free(ws);
            p->nthreads = i; /* 만들어진 것까지만 정리 */
            pool_destroy(p);
            return NULL;
        }
    }
    p->nthreads = nthreads;
    return p;
}

int pool_submit(struct thread_pool *p, int fd)
{
    pthread_mutex_lock(&p->mu);
    if (p->stopping || p->count == p->cap) {
        pthread_mutex_unlock(&p->mu);
        return -1;
    }
    p->queue[(p->head + p->count) % p->cap] = fd;
    p->count++;
    pthread_cond_signal(&p->not_empty); /* 자고 있는 워커 하나만 깨우면 충분 */
    pthread_mutex_unlock(&p->mu);
    return 0;
}

size_t pool_pending(struct thread_pool *p)
{
    pthread_mutex_lock(&p->mu);
    size_t n = p->count;
    pthread_mutex_unlock(&p->mu);
    return n;
}

void pool_destroy(struct thread_pool *p)
{
    if (!p)
        return;

    pthread_mutex_lock(&p->mu);
    p->stopping = 1;
    /* 아직 아무 워커도 맡지 않은 연결은 처리하지 않고 닫는다. */
    while (p->count > 0) {
        close(p->queue[p->head]);
        p->head = (p->head + 1) % p->cap;
        p->count--;
    }
    pthread_cond_broadcast(&p->not_empty); /* 자는 워커 "모두" 깨워 종료 확인시키기 */
    pthread_mutex_unlock(&p->mu);

    /* 일하던 워커는 지금 연결을 마친 뒤 루프 맨 위에서 stopping 을 보고 나온다. */
    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);

    pthread_cond_destroy(&p->not_empty);
    pthread_mutex_destroy(&p->mu);
    free(p->threads);
    free(p->queue);
    free(p);
}
