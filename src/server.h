#ifndef SERVER_H
#define SERVER_H

/* port 에서 listen 하는 소켓을 만들어 fd 를 반환한다. 실패하면 -1. */
int server_listen(int port);

/* 연결 하나를 처리한다: 요청(헤더+본문)을 읽고 파싱해 응답을 보낸 뒤 닫는다.
 * GET/HEAD → docroot 아래 정적 파일, POST → 본문 echo, 그 외 → 405.
 * docroot 는 realpath 로 정규화된 절대 경로여야 한다(끝에 '/' 없음). */
void server_handle_client(int client_fd, const char *docroot);

#endif
