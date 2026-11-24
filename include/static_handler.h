#ifndef MINI_WEB_SERVER_STATIC_HANDLER_H
#define MINI_WEB_SERVER_STATIC_HANDLER_H

#include "http.h"

/* doc_root(이미 realpath 로 정규화된 절대 경로) 아래의 정적 파일을 응답한다.
 *   - "/" 로 끝나는 경로는 index.html 을 붙여서 찾는다.
 *   - realpath() 결과가 doc_root 밖이면 403 (경로 탐색 공격 차단).
 *   - 파일은 HTTP_IO_CHUNK_SIZE(8KB) 고정 버퍼로 나눠 읽어 전송한다.
 * 반환: 클라이언트에 보낸 HTTP 상태 코드 (로그용). */
int static_handle(int client_fd, const http_request_t *req, const char *doc_root);

/* 확장자(대소문자 무시) -> Content-Type. 모르면 application/octet-stream. */
const char *static_mime_type(const char *path);

#endif /* MINI_WEB_SERVER_STATIC_HANDLER_H */
