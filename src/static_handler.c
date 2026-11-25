/*
 * static_handler.c - 정적 파일 서빙
 *
 * 처리 순서
 *   1) 메서드 검사 (GET 만 허용, 그 외 405)
 *   2) 디코딩된 경로에 ".." 세그먼트가 있으면 403         (1차 방어: 문자열 검사)
 *   3) doc_root + path (+ index.html) 를 realpath() 로 정규화
 *   4) 정규화된 경로가 doc_root 밖이면 403                (2차 방어: 심볼릭 링크까지 해석)
 *   5) open -> fstat 으로 일반 파일인지 확인 (디렉토리는 301 / 그 외 403)
 *   6) 헤더 전송 후 8KB 버퍼로 read -> send 반복
 */
#include "static_handler.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static const struct {
    const char *ext;
    const char *type;
} MIME_TABLE[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "text/javascript; charset=utf-8" },
    { "json", "application/json" },
    { "txt",  "text/plain; charset=utf-8" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "svg",  "image/svg+xml" },
    { "ico",  "image/x-icon" },
};

const char *static_mime_type(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    const char *dot   = strrchr(base, '.');

    if (dot != NULL && dot[1] != '\0') {
        for (size_t i = 0; i < sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]); i++) {
            if (strcasecmp(dot + 1, MIME_TABLE[i].ext) == 0)
                return MIME_TABLE[i].type;
        }
    }
    return "application/octet-stream";
}

/* resolved 가 root 자신이거나 root/ 하위인지 ("/www-evil" 같은 접두사 오탐 방지) */
static int is_inside_root(const char *resolved, const char *root)
{
    size_t root_len = strlen(root);

    if (root_len == 1 && root[0] == '/')
        return 1;
    return strncmp(resolved, root, root_len) == 0 &&
           (resolved[root_len] == '\0' || resolved[root_len] == '/');
}

//에러를 보내고 그 상태 코드를 반환하는 도우미입니다.
//Phase 3: 에러 응답이어도 요청은 끝까지 읽었으므로 연결은 유지할 수 있다.
static int send_error_status(int fd, int status, const char *extra, http_request_t *req)
{
    if (http_send_error(fd, status, extra, req->keep_alive) < 0)
        req->keep_alive = 0;
    return status;
}

/* 디렉토리를 "/" 없이 요청하면 "/" 를 붙인 주소로 보낸다 (상대 링크가 올바르게 동작하도록).
 * Location 에는 디코딩 전 원본 경로를 쓴다 — 원본은 제어문자가 없음이 파서에서 검증됨. */
static int redirect_to_dir(int fd, http_request_t *req)
{
    char extra[HTTP_MAX_URI_LEN * 2 + 32];
    const char *q = strchr(req->target, '?');
    int raw_len = q ? (int)(q - req->target) : (int)strlen(req->target);

    int n = snprintf(extra, sizeof(extra), "Location: %.*s/%s%s\r\n",
                     raw_len, req->target, q ? "?" : "", q ? q + 1 : "");
    if (n < 0 || (size_t)n >= sizeof(extra))
        return send_error_status(fd, 500, NULL, req);
    return send_error_status(fd, 301, extra, req);
}

/* 파일 내용을 8KB 고정 버퍼로 나눠 전송. 성공 0, 실패 -1. */
static int send_file_body(int client_fd, int file_fd, off_t size)
{
    char  chunk[HTTP_IO_CHUNK_SIZE];
    off_t sent = 0;

    while (sent < size) {
        ssize_t n = read(file_fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)                         /* 전송 도중 파일이 줄어듦 */
            return -1;
        if (http_send_all(client_fd, chunk, (size_t)n) < 0)
            return -1;
        sent += n;
    }
    return 0;
}

//메인 로직
int static_handle(int client_fd, http_request_t *req, const char *doc_root)
{
    if (req->method != HTTP_METHOD_GET)
        return send_error_status(client_fd, 405, "Allow: GET\r\n", req);

    /* 1차 방어: 디코딩 후 기준으로 검사하므로 "%2e%2e/" 도 여기서 걸린다 */
    if (http_path_has_dotdot(req->path))
        return send_error_status(client_fd, 403, NULL, req);

    size_t plen = strlen(req->path);
    int want_index = plen > 0 && req->path[plen - 1] == '/';

    char full[PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s%s%s", doc_root, req->path,
                     want_index ? "index.html" : "");
    if (n < 0 || (size_t)n >= sizeof(full))
        return send_error_status(client_fd, 404, NULL, req);

    /* 2차 방어: 심볼릭 링크, ".", 중복 "/" 까지 모두 해석한 실제 경로로 판단 */
    char resolved[PATH_MAX];
    if (realpath(full, resolved) == NULL) {
        if (errno == EACCES)
            return send_error_status(client_fd, 403, NULL, req);
        if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG || errno == ELOOP)
            return send_error_status(client_fd, 404, NULL, req);
        return send_error_status(client_fd, 500, NULL, req);
    }
    if (!is_inside_root(resolved, doc_root))
        return send_error_status(client_fd, 403, NULL, req);

    /* O_NONBLOCK: FIFO 같은 특수 파일을 열 때 open() 에서 멈추지 않도록.
     * (일반 파일 read 에는 영향 없음) */
    int file_fd = open(resolved, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file_fd < 0)
        return send_error_status(client_fd, errno == EACCES ? 403 : 404, NULL, req);

    /* stat 대신 이미 연 fd 로 fstat -> 검사와 사용 사이에 파일이 바뀌는 경쟁(TOCTOU) 방지 */
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return send_error_status(client_fd, 500, NULL, req);
    }
    if (S_ISDIR(st.st_mode)) {
        close(file_fd);
        if (want_index)                     /* "index.html" 이 디렉토리인 이상한 경우 */
            return send_error_status(client_fd, 403, NULL, req);
        return redirect_to_dir(client_fd, req);
    }
    if (!S_ISREG(st.st_mode)) {             /* 장치 파일, 소켓, FIFO 등 */
        close(file_fd);
        return send_error_status(client_fd, 403, NULL, req);
    }

    if (http_send_head(client_fd, 200, static_mime_type(resolved),
                       (long long)st.st_size, NULL, req->keep_alive) < 0) {
        req->keep_alive = 0;
    } else if (send_file_body(client_fd, file_fd, st.st_size) < 0) {
        /* 이미 Content-Length 를 약속했으므로 연결을 끊는 수밖에 없다 */
        fprintf(stderr, "[WARN] file transfer aborted: %s\n", resolved);
        req->keep_alive = 0;
    }
    close(file_fd);
    return 200;
}
