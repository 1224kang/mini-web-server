/*
 * thread_pool.c - pthread 기반 워커 풀
 *
 *   tasks[] (원형 버퍼)        head: 다음에 꺼낼 위치, count: 들어 있는 개수
 *   ┌───┬───┬───┬───┬───┐
 *   │   │ A │ B │ C │   │     submit -> tasks[(head + count) % cap] 에 넣고 signal
 *   └───┴───┴───┴───┴───┘     worker -> tasks[head] 를 꺼내고 head = (head+1) % cap
 *         ^head  count=3
 */
#include "thread_pool.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define WORKER_STACK_SIZE (1024 * 1024)   /* macOS 기본 512KB 보다 넉넉하게 */

typedef struct {
    thread_task_fn fn;
    void          *arg;
} task_t;

struct thread_pool {
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;     /* "큐에 일이 생겼다" 또는 "종료하라" */

    task_t   *tasks;
    size_t    capacity;
    size_t    head;
    size_t    count;

    pthread_t *threads;
    size_t     nthreads;           /* 실제로 생성에 성공한 워커 수 */
    int        stopping;
};

static void *worker_main(void *arg)
{
    thread_pool_t *pool = arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);

        /* while 인 이유: 깨어났는데 다른 워커가 먼저 가져갔을 수 있음 (spurious wakeup 포함) */
        while (pool->count == 0 && !pool->stopping)
            pthread_cond_wait(&pool->not_empty, &pool->lock);

        /* 종료 요청이 와도 큐가 빌 때까지는 계속 처리한다 (graceful) */
        if (pool->count == 0 && pool->stopping) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        task_t task = pool->tasks[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;

        pthread_mutex_unlock(&pool->lock);

        task.fn(task.arg);          /* 락을 풀고 실행해야 다른 워커가 동시에 일한다 */
    }
    return NULL;
}

static void free_pool(thread_pool_t *pool)
{
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->lock);
    free(pool->threads);
    free(pool->tasks);
    free(pool);
}

thread_pool_t *thread_pool_create(size_t nthreads, size_t queue_capacity)
{
    if (nthreads == 0 || queue_capacity == 0)
        return NULL;

    thread_pool_t *pool = calloc(1, sizeof(*pool));
    if (pool == NULL)
        return NULL;

    pool->tasks   = calloc(queue_capacity, sizeof(task_t));
    pool->threads = calloc(nthreads, sizeof(pthread_t));
    if (pool->tasks == NULL || pool->threads == NULL ||
        pthread_mutex_init(&pool->lock, NULL) != 0) {
        free(pool->tasks);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        free(pool->tasks);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    pool->capacity = queue_capacity;

    /* 새 스레드는 만든 스레드의 시그널 마스크를 물려받는다.
     * 잠시 모든 시그널을 막은 채 워커를 만들면, 워커는 시그널을 받지 않고
     * SIGINT/SIGTERM 은 항상 메인 스레드로 전달된다. */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);

    pthread_attr_t attr;
    int have_attr = pthread_attr_init(&attr) == 0;
    if (have_attr)
        pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE);

    for (size_t i = 0; i < nthreads; i++) {
        if (pthread_create(&pool->threads[i], have_attr ? &attr : NULL, worker_main, pool) != 0) {
            fprintf(stderr, "pthread_create failed after %zu workers\n", i);
            break;
        }
        pool->nthreads++;
    }

    if (have_attr)
        pthread_attr_destroy(&attr);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    if (pool->nthreads == 0) {
        free_pool(pool);
        return NULL;
    }
    return pool;
}

int thread_pool_submit(thread_pool_t *pool, thread_task_fn fn, void *arg)
{
    int rc = THREAD_POOL_OK;

    pthread_mutex_lock(&pool->lock);
    if (pool->stopping) {
        rc = THREAD_POOL_STOPPED;
    } else if (pool->count == pool->capacity) {
        rc = THREAD_POOL_FULL;
    } else {
        size_t tail = (pool->head + pool->count) % pool->capacity;
        pool->tasks[tail].fn  = fn;
        pool->tasks[tail].arg = arg;
        pool->count++;
        pthread_cond_signal(&pool->not_empty);   /* 자고 있는 워커 하나만 깨운다 */
    }
    pthread_mutex_unlock(&pool->lock);
    return rc;
}

void thread_pool_destroy(thread_pool_t *pool)
{
    if (pool == NULL)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->not_empty);    /* 모든 워커를 깨워 종료 조건을 확인시킨다 */
    pthread_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < pool->nthreads; i++)
        pthread_join(pool->threads[i], NULL);

    free_pool(pool);
}
