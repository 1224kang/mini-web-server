#ifndef STATIC_FILES_H
#define STATIC_FILES_H

/*
 * 정적 파일 서빙에 필요한 "순수 함수" 모음.
 * 파일 시스템을 건드리지 않고 문자열만 다루므로 단위 테스트를 바로 돌릴 수 있다.
 * 실제 open/read/send 는 server.c 가 한다.
 */

#include <stddef.h>

#define STATIC_MAX_PATH 4096 /* 디코딩 전 URL 경로, 만들어진 파일 경로의 최대 길이 */

enum static_resolve_result {
    STATIC_OK = 0,
    STATIC_BAD_REQUEST, /* '/' 로 시작 안 함, 잘못된 %xx, %00     → 400 */
    STATIC_FORBIDDEN,   /* ".." 로 루트 밖으로 나가려 함           → 403 */
    STATIC_TOO_LONG,    /* 만든 경로가 out 버퍼보다 김             → 414 */
};

/* 요청 target("/css/a.css?v=1")을 docroot 아래의 파일 시스템 경로로 바꾼다.
 *
 *   1) '?', '#' 뒤(쿼리/프래그먼트)는 버린다.
 *   2) %xx 를 디코딩한다. ("%2e%2e" 도 ".." 으로 바뀐 뒤 검사받는다.)
 *   3) '/' 로 자른 조각을 하나씩 쌓는다. "" 와 "." 은 무시, ".." 은 하나 빼기.
 *      뺄 게 없는데 ".." 이 나오면 = 루트 밖으로 탈출 시도 → STATIC_FORBIDDEN.
 *
 * 성공하면 out 에 "docroot/정리된/경로" 를 쓴다.
 * *trailing_slash 는 target 경로가 '/' 로 끝났는지(디렉토리로 요청했는지)를 알려 준다.
 *
 * 주의: 이건 "글자만 보고" 하는 검사다. 심볼릭 링크로 밖을 가리키는 경우는
 * 못 잡으므로, server.c 에서 realpath 로 한 번 더 확인한다. */
enum static_resolve_result static_resolve_path(const char *docroot,
                                               const char *target,
                                               char *out, size_t cap,
                                               int *trailing_slash);

/* 확장자로 Content-Type 을 고른다. 모르는 확장자는 "application/octet-stream". */
const char *static_mime_type(const char *path);

/* realpath 결과 path 가 root 자기 자신이거나 그 아래에 있으면 1.
 * "/srv/www" 와 "/srv/www-secret" 처럼 앞부분만 같은 경우는 0. */
int static_is_under(const char *root, const char *path);

#endif
