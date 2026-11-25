#ifndef BUILTIN_H
#define BUILTIN_H

/*
 * 내장 핸들러: 프로세스를 띄우지 않고 서버 안에서 바로 계산하는 동적 응답.
 * CGI 와 같은 일을 하는 /builtin/add 를 두어 "fork+exec 비용"을 비교해 볼 수 있다.
 *
 *   GET  /builtin/add?a=1&b=2  → "3\n"
 *   POST /builtin/echo         → 받은 본문을 받은 Content-Type 그대로 돌려준다
 *
 * 소켓을 모르는 순수 함수라 단위 테스트가 가능하다.
 */

#include "http.h"

#include <stddef.h>

#define BUILTIN_PREFIX "/builtin/"

struct builtin_response {
    int status;
    const char *content_type;
    const char *extra_headers; /* NULL 또는 "Allow: GET, HEAD\r\n" */
    const char *body;          /* text 를 가리키거나, echo 면 요청 본문을 가리킨다 */
    size_t body_len;
    char text[256];
};

/* target 이 /builtin/ 으로 시작하면 처리하고 1, 아니면 0. */
int builtin_handle(const struct http_request *req, const char *body, size_t body_len,
                   struct builtin_response *out);

/* 쿼리 문자열에서 key 의 값을 URL 디코딩해 out 에 넣는다("+" 는 공백).
 * 찾으면 1, 없으면 0, 디코딩 실패나 out 이 모자라면 -1. */
int query_get(const char *target, const char *key, char *out, size_t cap);

#endif
