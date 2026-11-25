#ifndef CGI_H
#define CGI_H

/*
 * CGI 프로세스 관리: fork + exec, 파이프, 자식 회수.
 *
 *            부모(서버)                               자식(CGI 스크립트)
 *   stdin_fd ──쓰기──▶ [파이프 1] ──▶ fd 0 (stdin)   요청 본문을 읽는다
 *   stdout_fd ◀──읽기── [파이프 2] ◀── fd 1 (stdout)  응답을 출력한다
 *                                    fd 2 (stderr)  서버 로그로 그대로 나온다
 *
 * 부모 쪽 두 fd 는 non-blocking 이라 이벤트 루프의 poll 목록에 연결 소켓과 나란히 들어간다.
 */

#include <sys/types.h>

/* script 를 실행한다. 자식은 dir 로 chdir 한 뒤 envp 환경으로 exec 된다.
 * 성공하면 0 과 함께 pid, 부모 쪽 파이프 두 개(non-blocking)를 돌려준다. 실패하면 -1. */
int cgi_spawn(const char *script, const char *dir, char *const envp[], pid_t *pid,
              int *stdin_fd, int *stdout_fd);

/* 좀비 회수 준비: SIGCHLD 가 오면 1바이트를 쓰는 파이프를 만든다(self-pipe).
 * 이벤트 루프는 cgi_reaper_fd() 를 poll 하다가 읽을 수 있게 되면 cgi_reap() 을 부른다. */
int cgi_reaper_init(void);
int cgi_reaper_fd(void);

/* 연결이 자식과의 볼일을 끝냈다(출력 EOF, 타임아웃, 연결 종료).
 * kill_it 이면 SIGKILL 을 먼저 보낸다. 이미 끝났으면 바로 거두고, 아직 실행 중이면
 * "고아 목록"에 넣어 두었다가 SIGCHLD 가 올 때 cgi_reap 이 거둔다. */
void cgi_release(pid_t pid, int kill_it);

/* 고아 목록에서 끝난 자식을 waitpid 로 거둔다. 거둔 수를 돌려준다. */
int cgi_reap(void);

#endif
