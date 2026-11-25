/*
 * http_response.c - HTTP 응답 빌더
 *   상태 코드 -> 사유 문구, 공통 헤더 조립, 기본 에러 페이지 생성, 안전한 전송
 */
#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

/* 끊긴 소켓에 write 할 때 SIGPIPE로 프로세스가 죽지 않도록 한다.
 * Linux: send()의 MSG_NOSIGNAL 플래그 / macOS: 소켓 옵션 SO_NOSIGPIPE (server.c 에서 설정).
 * 이렇게 하면 두 OS 모두 send()가 시그널 대신 -1과 errno = EPIPE를 반환합니다.
 * (Phase 5 에서 프로세스 전역 SIGPIPE 무시도 추가했지만, 라이브러리처럼 단독으로
 *  쓰여도 안전하도록 소켓 단위 방어는 그대로 둔다) */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define SERVER_SOFTWARE "mini-web-server/1.0"

//상태 코드를 사유 문구로 바꿉니다
const char *http_status_text(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* Partial write를 고려해 len 바이트를 모두 보낸다. 성공 0, 실패 -1. */
//부분 전송(partial write)이 일어나도 끝까지 반복해서 보냅니다.
int http_send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n = send(fd, p, len, SEND_FLAGS);
        if (n < 0) {
            //EINTR: 시그널 때문에 시스템 콜이 중단된 것이므로 에러가 아닙니다. 다시 시도합니다.
            if (errno == EINTR)
                continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* RFC 7231 7.1.1.1 IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT" */
//형식의 날짜를 만듭니다.
static void format_http_date(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm;

    if (gmtime_r(&now, &tm) == NULL || strftime(out, cap, "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0)
        snprintf(out, cap, "Thu, 01 Jan 1970 00:00:00 GMT");
}

//상태 줄과 공통 헤더를 한 번에 조립해서 보냅니다.
int http_send_head(int fd, int status, const char *content_type,
                   long long content_length, const char *extra_headers, int keep_alive)
{
    char date[64];
    char head[4096];

    format_http_date(date, sizeof(date));

    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: " SERVER_SOFTWARE "\r\n"
                     "%s%s%s"
                     "Content-Length: %lld\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "\r\n",
                     status, http_status_text(status),
                     date,
                     content_type ? "Content-Type: " : "",
                     content_type ? content_type : "",
                     content_type ? "\r\n" : "",
                     content_length,
                     keep_alive ? "keep-alive" : "close",
                     extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= sizeof(head))
        return -1;
    return http_send_all(fd, head, (size_t)n);
}

//에러 상태 코드로 작은 HTML 페이지를 만들어 보냅니다.
int http_send_error(int fd, int status, const char *extra_headers, int keep_alive)
{
    char body[512];
    const char *text = http_status_text(status);

    int n = snprintf(body, sizeof(body),
                     "<!DOCTYPE html>\n"
                     "<html>\n"
                     "<head><meta charset=\"utf-8\"><title>%d %s</title></head>\n"
                     "<body>\n"
                     "<h1>%d %s</h1>\n"
                     "<hr>\n"
                     "<p>" SERVER_SOFTWARE "</p>\n"
                     "</body>\n"
                     "</html>\n",
                     status, text, status, text);
    if (n < 0 || (size_t)n >= sizeof(body))
        return -1;

    if (http_send_head(fd, status, "text/html; charset=utf-8", n, extra_headers, keep_alive) < 0)
        return -1;
    return http_send_all(fd, body, (size_t)n);
}
