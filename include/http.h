/*
 * http.h - HTTP/1.1 요청 파서 + 응답 빌더 (RFC 7230/7231 최소 구현)
 *
 *   http_parser.c   : 소켓에서 요청을 읽고(Partial Read 대응) 파싱한다.
 *   http_response.c : 상태 줄/헤더/에러 페이지를 만들어 전송한다.
 */
#ifndef MINI_WEB_SERVER_HTTP_H
#define MINI_WEB_SERVER_HTTP_H

#include <stddef.h>
#include <sys/types.h>

/* ---------------------------------------------------------------- */
/* 방어용 한계값                                                     */
/* ---------------------------------------------------------------- */
#define HTTP_MAX_HEADER_SIZE  8192                 /* 요청 줄 + 헤더 + 빈 줄 (초과 시 413) */
#define HTTP_MAX_URI_LEN      1024                 /* request-target 길이 (초과 시 414)    */
#define HTTP_MAX_BODY_SIZE    (10L * 1024 * 1024)  /* Content-Length 상한 (초과 시 413)    */
#define HTTP_MAX_HEADERS      64                   /* 헤더 줄 개수 (초과 시 413)           */
#define HTTP_MAX_METHOD_LEN   15
#define HTTP_IO_CHUNK_SIZE    8192                 /* 파일 전송 시 고정 버퍼 크기           */

/* http_read_request() 의 특수 반환값 (양수는 클라이언트에 보낼 HTTP 에러 상태 코드) */
#define HTTP_READ_OK        0
#define HTTP_READ_EOF      (-1)   /* 요청을 한 바이트도 보내지 않고 연결이 닫힘 */
#define HTTP_READ_IO_ERROR (-2)   /* recv/send 실패 (RST 등)                    */
#define HTTP_READ_TIMEOUT  (-3)   /* Phase 3: 아무것도 오지 않은 채 SO_RCVTIMEO 만료
                                     (요청을 보내다 멈춘 경우는 408 을 반환)      */

typedef enum {
    HTTP_METHOD_UNKNOWN = 0,      /* 문법상 올바르지만 지원하지 않는 메서드 -> 405 */
    HTTP_METHOD_GET,
    HTTP_METHOD_POST
} http_method_t;

typedef struct {
    const char *name;             /* http_request_t.head 내부를 가리킨다 */
    const char *value;
} http_header_t;

typedef struct {
    http_method_t method;
    char   method_str[HTTP_MAX_METHOD_LEN + 1];
    char   target[HTTP_MAX_URI_LEN + 1];   /* 원본 request-target  예) "/a%20b.html?x=1" */
    char   path[HTTP_MAX_URI_LEN + 1];     /* 퍼센트 디코딩된 경로 예) "/a b.html"        */
    char   query[HTTP_MAX_URI_LEN + 1];    /* '?' 뒤 문자열 (없으면 "")                   */
    int    version_major;
    int    version_minor;

    http_header_t headers[HTTP_MAX_HEADERS];
    size_t        header_count;

    char   head[HTTP_MAX_HEADER_SIZE + 1]; /* 헤더 원문 사본. 파싱하며 제자리에서 잘라 쓴다 */
    size_t head_len;

    char  *body;                           /* malloc. Content-Length 만큼, NUL 종료 */
    size_t body_len;

    /* Phase 3: 응답 후 연결을 유지할지.
     * 파서가 버전/Connection 헤더로 초기값을 정하고, 서버(종료 중, 요청 수 한도)와
     * 핸들러(전송 실패, 길이를 모르는 프록시 응답)가 0 으로 내릴 수 있다. */
    int    keep_alive;
} http_request_t; //파싱 결과를 담는 구조체

/* 연결 단위 수신 버퍼.
 * recv()는 요청 경계와 무관하게 데이터를 돌려주므로, 헤더 끝(\r\n\r\n)을 찾을 때까지
 * 여기에 쌓아두고, 요청을 다 쓰고 남은 바이트는 다음 요청(Phase 3 keep-alive)용으로 보존한다. */
typedef struct {
    int    fd;
    size_t len;                            /* buf 안에 아직 소비하지 않은 바이트 수 */
    char   buf[HTTP_MAX_HEADER_SIZE];
} http_conn_t;

/* ---------------------------------------------------------------- */
/* 파서 (http_parser.c)                                              */
/* ---------------------------------------------------------------- */
void http_conn_init(http_conn_t *conn, int fd);
void http_request_init(http_request_t *req);
void http_request_free(http_request_t *req);

/* 요청 하나를 끝까지(헤더 + Content-Length 본문) 읽어 req를 채운다.
 * 반환: HTTP_READ_OK / HTTP_READ_EOF / HTTP_READ_IO_ERROR / HTTP_READ_TIMEOUT,
 *       또는 400, 408, 413, 414, 500, 501, 505 같은 "응답해야 할 에러 상태 코드".
 * 요청 뒤에 이어 온 바이트(파이프라이닝)는 conn->buf 에 남겨 다음 호출에서 사용한다. */
int http_read_request(http_conn_t *conn, http_request_t *req);

/* 소켓과 무관한 순수 파서. data[0..len) 은 "\r\n\r\n" 으로 끝나는 헤더 블록.
 * 성공 0, 실패 시 에러 상태 코드. (단위 테스트에서 직접 호출 가능) */
int http_parse_head(http_request_t *req, const char *data, size_t len);

/* 대소문자 무시 헤더 조회. 없으면 NULL. */
const char *http_get_header(const http_request_t *req, const char *name);

/* "keep-alive, Upgrade" 처럼 쉼표로 구분된 헤더 값에 token 이 있는지 (대소문자 무시) */
int http_header_has_token(const char *value, const char *token);

/* "/a/../b" 처럼 정확히 ".." 인 경로 세그먼트가 있는지 (경로 탐색 1차 방어) */
int http_path_has_dotdot(const char *path);

/* ---------------------------------------------------------------- */
/* 응답 (http_response.c)                                            */
/* ---------------------------------------------------------------- */
const char *http_status_text(int status);

/* Partial write를 고려해 len 바이트를 모두 보낸다. 성공 0, 실패 -1. */
int http_send_all(int fd, const void *buf, size_t len);

/* 상태 줄 + 공통 헤더(Date, Server, Content-Type, Content-Length, Connection) 전송.
 * content_type 이 NULL 이면 Content-Type 생략.
 * extra_headers 는 "Name: value\r\n" 형태의 추가 헤더 묶음 (없으면 NULL).
 * keep_alive 가 참이면 "Connection: keep-alive", 아니면 "Connection: close". */
int http_send_head(int fd, int status, const char *content_type,
                   long long content_length, const char *extra_headers, int keep_alive);

/* 기본 HTML 에러 페이지 전송 (400, 403, 404, 405, 408, 413, 414, 500, 502, 503, 504 ...). */
int http_send_error(int fd, int status, const char *extra_headers, int keep_alive);

#endif /* MINI_WEB_SERVER_HTTP_H */
