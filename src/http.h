#ifndef HTTP_H
#define HTTP_H

/*
 * HTTP/1.x 요청 파서 + 응답 헤더 빌더.
 * 소켓을 전혀 모르는 "순수 함수"만 모아 두었다 → 단위 테스트를 바로 돌릴 수 있다.
 */

#include <stddef.h>

#define HTTP_MAX_HEAD    8192          /* 요청 라인 + 헤더 전체의 최대 크기 (넘으면 431) */
#define HTTP_MAX_HEADERS 32            /* 헤더 개수 최대치 (넘으면 431) */
#define HTTP_MAX_BODY    (1024 * 1024) /* 본문 최대 크기 1MB (넘으면 413) */

/* 헤더 한 줄 "Name: value".
 * 문자열을 복사하지 않고 요청 버퍼 안을 가리킨다(파서가 구분자 자리에 '\0' 을 써 넣는다). */
struct http_header {
    const char *name;
    const char *value;
};

struct http_request {
    const char *method;  /* "GET" */
    const char *target;  /* "/hello?x=1" (요청 라인에 적힌 그대로) */
    const char *version; /* "HTTP/1.1" */
    struct http_header headers[HTTP_MAX_HEADERS];
    size_t header_count;
};

enum http_parse_result {
    HTTP_PARSE_OK = 0,
    HTTP_PARSE_BAD_REQUEST,      /* 형식이 틀림          → 400 */
    HTTP_PARSE_TOO_MANY_HEADERS, /* 헤더 개수 초과        → 431 */
    HTTP_PARSE_BAD_VERSION,      /* HTTP/1.0, 1.1 이 아님 → 505 */
};

/* buf[0..len) 에서 헤더 끝 "\r\n\r\n" 을 찾는다.
 * 찾으면 "\r\n\r\n" 바로 다음 위치(= 헤더 길이, 본문 시작 위치)를, 아직 없으면 -1 을 반환.
 * recv 로 조각조각 받으면서 매번 호출해 "헤더를 다 받았는지" 판단하는 용도. */
long http_find_head_end(const char *buf, size_t len);

/* buf 의 앞 head_len 바이트(반드시 "\r\n\r\n" 으로 끝나야 함)를 파싱해 req 를 채운다.
 * buf 를 직접 수정한다. head_len 이후(본문)는 건드리지 않는다. */
enum http_parse_result http_parse_request(char *buf, size_t head_len,
                                          struct http_request *req);

/* 헤더 이름은 대소문자를 구분하지 않는다. 없으면 NULL. */
const char *http_get_header(const struct http_request *req, const char *name);

/* Content-Length 를 읽어 *out 에 넣는다. 헤더가 없으면 0.
 * 숫자가 아니거나, 오버플로거나, 여러 번 나오면 -1 (→ 400). 정상이면 0. */
int http_content_length(const struct http_request *req, size_t *out);

/* 상태 코드에 맞는 사유 문구. 예: 404 → "Not Found" */
const char *http_reason(int status);

/* 응답의 상태 라인 + 헤더 + 빈 줄을 out 에 쓴다. 본문은 호출한 쪽이 따로 보낸다.
 * extra_headers 는 NULL 이거나 "Name: value\r\n" 이 이어진 문자열.
 * 쓴 바이트 수를 반환하고, out 이 모자라면 -1. */
int http_build_response_head(char *out, size_t cap, int status,
                             const char *content_type, size_t body_len,
                             const char *extra_headers);

#endif
