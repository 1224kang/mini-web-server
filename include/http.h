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
#define HTTP_READ_IO_ERROR (-2)   /* recv/send 실패 (RST, 타임아웃 등)         */

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
 * 반환: HTTP_READ_OK / HTTP_READ_EOF / HTTP_READ_IO_ERROR,
 *       또는 400, 413, 414, 500, 501, 505 같은 "응답해야 할 에러 상태 코드". */
int http_read_request(http_conn_t *conn, http_request_t *req);

/* 소켓과 무관한 순수 파서. data[0..len) 은 "\r\n\r\n" 으로 끝나는 헤더 블록.
 * 성공 0, 실패 시 에러 상태 코드. (단위 테스트에서 직접 호출 가능) */
int http_parse_head(http_request_t *req, const char *data, size_t len);

/* 대소문자 무시 헤더 조회. 없으면 NULL. */
const char *http_get_header(const http_request_t *req, const char *name);

/* ---------------------------------------------------------------- */
/* 응답 (http_response.c)                                            */
/* ---------------------------------------------------------------- */
const char *http_status_text(int status);

/* Partial write를 고려해 len 바이트를 모두 보낸다. 성공 0, 실패 -1. */
int http_send_all(int fd, const void *buf, size_t len);

/* 상태 줄 + 공통 헤더(Date, Server, Content-Type, Content-Length, Connection) 전송.
 * content_type 이 NULL 이면 Content-Type 생략.
 * extra_headers 는 "Name: value\r\n" 형태의 추가 헤더 묶음 (없으면 NULL). */
int http_send_head(int fd, int status, const char *content_type,
                   long long content_length, const char *extra_headers);

/* 기본 HTML 에러 페이지 전송 (400, 403, 404, 405, 413, 414, 500, 502, 504 ...). */
int http_send_error(int fd, int status, const char *extra_headers);

#endif /* MINI_WEB_SERVER_HTTP_H */
