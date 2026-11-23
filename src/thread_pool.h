#ifndef THREAD_POOL_H
#define THREAD_POOL_H

/*
 * 고정 크기 스레드 풀 + "연결 fd 대기열(큐)".
 *
 *   main 스레드(accept) ──submit(fd)──▶ [ 큐: fd fd fd ... ] ──꺼내기──▶ 워커 N개
 *
 * 큐는 모든 스레드가 같이 쓰는 공유 자원이라 mutex 로 보호하고,
 * "큐가 비었으니 기다려" / "들어왔으니 일어나" 는 condition variable 로 주고받는다.
 */

#include <stddef.h>

struct thread_pool;

/* 워커가 큐에서 fd 를 하나 꺼낼 때마다 호출하는 함수. fd 를 닫는 건 이 함수의 몫이다.
 * worker_id 는 0..nthreads-1 (로그에서 "누가 처리했는지" 보려는 용도). */
typedef void (*pool_job_fn)(int fd, int worker_id, void *arg);

/* nthreads 개의 워커를 만들고, 최대 queue_cap 개의 fd 를 대기시킬 수 있는 풀을 만든다.
 * 실패하면 NULL. */
struct thread_pool *pool_create(int nthreads, size_t queue_cap, pool_job_fn fn, void *arg);

/* fd 를 큐에 넣는다. 성공 0.
 * 큐가 꽉 찼거나 종료 중이면 -1 (fd 는 호출한 쪽이 처리해야 한다 → 503).
 * 기다리지 않는다: accept 스레드가 막히면 서버 전체가 멈추기 때문. */
int pool_submit(struct thread_pool *p, int fd);

/* 지금 큐에서 워커를 기다리는 fd 개수. (keep-alive 를 계속할지 판단하는 데 쓴다) */
size_t pool_pending(struct thread_pool *p);

/* 종료: 새 일을 받지 않고, 아직 시작 못 한 fd 는 닫고, 일하던 워커가
 * 지금 연결을 끝낼 때까지 기다린(join) 뒤 메모리를 푼다. */
void pool_destroy(struct thread_pool *p);

#endif
