/*
 * thread_pool.h - 고정 크기 워커 스레드 풀 + 유한 작업 큐 (Producer-Consumer)
 *
 *   Producer : 메인 스레드 (accept 한 연결을 thread_pool_submit 으로 넣는다)
 *   Consumer : 워커 스레드 N 개 (큐에서 꺼내 task 함수를 실행한다)
 *
 * 큐는 고정 크기 원형 버퍼이며 mutex 1개 + 조건 변수 1개로 보호한다.
 * 큐가 가득 차면 submit 은 기다리지 않고 즉시 실패한다 -> 호출자가 503 으로 거절
 * (메인 스레드가 막히면 시그널/종료 처리도 함께 멈추기 때문).
 */
#ifndef MINI_WEB_SERVER_THREAD_POOL_H
#define MINI_WEB_SERVER_THREAD_POOL_H

#include <stddef.h>

typedef void (*thread_task_fn)(void *arg);

typedef struct thread_pool thread_pool_t;   /* 구현은 thread_pool.c 에 숨김 */

#define THREAD_POOL_OK        0
#define THREAD_POOL_FULL     (-1)
#define THREAD_POOL_STOPPED  (-2)

/* 워커 nthreads 개, 큐 용량 queue_capacity 로 생성. 실패 시 NULL.
 * 워커는 SIGINT/SIGTERM 등을 막은(block) 상태로 시작하므로 시그널은 생성한 스레드로만 간다. */
thread_pool_t *thread_pool_create(size_t nthreads, size_t queue_capacity);

/* 작업 추가. THREAD_POOL_OK / THREAD_POOL_FULL / THREAD_POOL_STOPPED.
 * 실패하면 arg 의 소유권은 호출자에게 남는다 (호출자가 정리해야 함). */
int thread_pool_submit(thread_pool_t *pool, thread_task_fn fn, void *arg);

/* Graceful 종료: 새 작업을 거부하고, 큐에 남은 작업까지 모두 실행한 뒤
 * 워커를 join 하고 메모리를 해제한다. NULL 이면 아무것도 하지 않는다. */
void thread_pool_destroy(thread_pool_t *pool);

#endif /* MINI_WEB_SERVER_THREAD_POOL_H */
