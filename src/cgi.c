#include "cgi.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void set_cloexec_nonblock(int fd, int nonblock)
{
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (nonblock)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

int cgi_spawn(const char *script, const char *dir, char *const envp[], pid_t *pid,
              int *stdin_fd, int *stdout_fd)
{
    int in[2], out[2]; /* [0] 읽는 쪽, [1] 쓰는 쪽 */
    if (pipe(in) < 0)
        return -1;
    if (pipe(out) < 0) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    /* FD_CLOEXEC: exec 하는 순간 자동으로 닫힌다. 이게 없으면 다른 CGI 자식들이 이 파이프를
     * (그리고 클라이언트 소켓까지) 물려받아, 우리가 닫아도 상대는 EOF 를 영영 못 본다. */
    set_cloexec_nonblock(in[0], 0);
    set_cloexec_nonblock(in[1], 1);
    set_cloexec_nonblock(out[0], 1);
    set_cloexec_nonblock(out[1], 0);

    pid_t p = fork();
    if (p < 0) {
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        return -1;
    }
    if (p == 0) {
        /* ---- 자식 ---- fork 직후엔 부모 메모리의 복사본이다. 여기서는 exec 전까지
         * async-signal-safe 한 일만 하고, 실패해도 exit 대신 _exit (부모의 stdio 버퍼를
         * 두 번 flush 하지 않도록). */
        dup2(in[0], STDIN_FILENO);   /* 파이프 1 의 읽는 쪽 → stdin  */
        dup2(out[1], STDOUT_FILENO); /* 파이프 2 의 쓰는 쪽 → stdout */
        /* 원본 네 개는 FD_CLOEXEC 라 exec 때 닫힌다. dup2 로 만든 0, 1 은 CLOEXEC 가 없다. */

        /* SIG_IGN 은 exec 해도 그대로 물려진다. 서버가 무시하던 SIGPIPE 를 기본값으로 돌려 둬야
         * "head -1" 같은 평범한 프로그램이 정상적으로 동작한다. */
        signal(SIGPIPE, SIG_DFL);
        /* 자기만의 프로세스 그룹을 만든다(그룹 번호 = 자기 pid).
         *  - 타임아웃 때 kill(-pid) 로 스크립트가 띄운 손자(예: sh 안의 sleep)까지 한 번에 죽일 수 있다.
         *  - 터미널의 Ctrl+C(SIGINT)는 "포그라운드 그룹 전체"에 가는데, 여기서 빠져나와야
         *    서버가 우아하게 종료하는 동안 진행 중인 CGI 가 같이 죽지 않는다. */
        setpgid(0, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);

        if (chdir(dir) < 0)
            _exit(127);
        const char *slash = strrchr(script, '/');
        char *const argv[] = { (char *)(slash ? slash + 1 : script), NULL };
        execve(script, argv, envp);

        /* 여기 왔다면 exec 실패 (#! 줄이 가리키는 인터프리터가 없다 등) */
        static const char msg[] = "[cgi] exec failed\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(127);
    }

    /* ---- 부모 ---- 자식보다 부모가 먼저 실행될 수도 있으니 여기서도 그룹을 정해 둔다
     * (kill(-pid) 가 그룹이 생기기 전에 불리는 경우 대비. 둘 중 늦은 쪽은 무해하게 실패). */
    setpgid(p, p);

    /* 자식에게 준 쪽은 닫는다. 특히 out[1] 을 안 닫으면 자식이 끝나도
     * "쓰는 쪽이 아직 열려 있다(부모 자신)"이라 stdout 파이프의 EOF 가 오지 않는다. */
    close(in[0]);
    close(out[1]);
    *pid = p;
    *stdin_fd = in[1];
    *stdout_fd = out[0];
    return 0;
}

/* ------------------------------------------------------------------ */
/* 좀비 회수                                                             */
/* ------------------------------------------------------------------ */

/* 자식이 끝나면 커널은 부모가 waitpid 로 종료 상태를 읽어 갈 때까지 프로세스 표의 한 칸을
 * 남겨 둔다(좀비, ps 에서 <defunct>). 안 거두면 쌓여서 결국 fork 가 실패한다. */
static int reap_pipe[2] = { -1, -1 };

static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;
    (void)!write(reap_pipe[1], "c", 1); /* non-blocking: 꽉 차 있으면 그냥 버린다(이미 깨울 예정) */
    errno = saved;
}

int cgi_reaper_init(void)
{
    if (pipe(reap_pipe) < 0) { //파이프 만들기
        perror("pipe");
        return -1;
    }
    set_cloexec_nonblock(reap_pipe[0], 1); //양쪽 다 non-blocking +CLOEXEC
    set_cloexec_nonblock(reap_pipe[1], 1);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART: read/write 도중 SIGCHLD 가 와도 EINTR 로 끊기지 않게.
     * SA_NOCLDSTOP: 자식이 멈출(SIGSTOP) 때는 알리지 않음, 끝날 때만. */
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction(SIGCHLD)");
        return -1;
    }
    return 0;
}

int cgi_reaper_fd(void)
{
    return reap_pipe[0];
}

/* 연결이 손을 뗀 자식들. 아직 안 끝났으면 SIGCHLD 가 올 때 여기서 거둔다. */
static pid_t *orphans;
static size_t norphans, orphans_cap;

static int try_wait(pid_t pid)
{
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0)
        return 0; /* 아직 실행 중 */
    if (r == pid) {
        if (WIFEXITED(status))
            printf("[cgi] reaped pid %d (exit %d)\n", (int)pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            printf("[cgi] reaped pid %d (killed by signal %d)\n", (int)pid, WTERMSIG(status));
    }
    return 1; /* 거뒀거나(r == pid), 이미 없음(ECHILD) */
}

void cgi_release(pid_t pid, int kill_it)
{
    if (pid <= 0)
        return;
    /* 아직 우리가 waitpid 하지 않았으므로 이 pid 는 (좀비로라도) 살아 있고, 다른 프로세스에게
     * 재사용될 수 없다 → kill 이 엉뚱한 프로세스를 죽일 위험이 없다. */
    if (kill_it)
        kill(-pid, SIGKILL); /* 음수 = 프로세스 그룹 전체 (자식 + 손자들) */
    if (try_wait(pid))
        return;
    if (norphans == orphans_cap) {
        size_t cap = orphans_cap ? orphans_cap * 2 : 64;
        pid_t *np = realloc(orphans, cap * sizeof(*np));
        if (!np) {
            fprintf(stderr, "[cgi] cannot track pid %d (it will stay a zombie)\n", (int)pid);
            return;
        }
        orphans = np;
        orphans_cap = cap;
    }
    orphans[norphans++] = pid;
}

int cgi_reap(void)
{
    char drain[64];
    while (read(reap_pipe[0], drain, sizeof(drain)) > 0)
        ;
    /* 시그널은 겹치면 하나로 합쳐질 수 있다(자식 3개가 동시에 끝나도 SIGCHLD 는 1번).
     * 그래서 "하나 거두기"가 아니라 목록 전체를 훑는다.
     * waitpid(-1) 을 쓰지 않는 이유: 연결이 아직 쥐고 있는 자식까지 거둬 버리면, 그 pid 가
     * 재사용된 뒤 연결이 타임아웃으로 kill(pid) 할 때 남의 프로세스를 죽일 수 있다. */
    int n = 0;
    for (size_t i = 0; i < norphans;) {
        if (try_wait(orphans[i])) {
            orphans[i] = orphans[--norphans];
            n++;
        } else {
            i++;
        }
    }
    return n;
}
