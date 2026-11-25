# mini-web-server
정글 13기 13주차 웹서버 만들기 13팀 레포지토리입니다.

외부 HTTP 라이브러리 없이 **C(`-std=gnu11`) + POSIX API** 만으로 만든 멀티스레드 웹 서버 겸 리버스 프록시입니다.
Linux(Ubuntu/WSL2)와 macOS 에서 `-Wall -Wextra -pthread` 경고 없이 빌드됩니다.

## 빌드 & 실행

```bash
make                                  # ./mini_web_server
./mini_web_server -c config.conf      # 설정 파일로 실행
./mini_web_server 8080 ./www          # 위치 인자 (Phase 1~2 호환)
./mini_web_server -p 8080 -t 16 -R /api/=127.0.0.1:8001,127.0.0.1:8002
```

종료는 `Ctrl+C`(SIGINT) 또는 `kill`(SIGTERM)입니다. 진행 중인 요청을 마친 뒤 정상 종료하고,
`Ctrl+C` 를 한 번 더 누르면 기다리지 않고 바로 종료합니다.

| 옵션 | 의미 | 기본값 |
|---|---|---|
| `-c FILE` | 설정 파일 | 없음 |
| `-p PORT` | 포트 | 8080 |
| `-r DIR` | 정적 파일 루트 | `./www` |
| `-t N` | 워커 스레드 수 | 8 |
| `-q N` | 연결 대기 큐 크기 (가득 차면 503) | 256 |
| `-R SPEC` | 프록시 규칙 `/prefix/=host:port[,host:port]` (반복 가능) | 없음 |

우선순위: 기본값 < 설정 파일 < 명령줄 옵션. 설정 파일 형식은 [config.conf](config.conf) 참고.

## 테스트

```bash
make test         # 단위 테스트 (파서, 스레드 풀, Round-Robin, 설정)
make test-asan    # 단위 테스트를 AddressSanitizer + LeakSanitizer 로
make asan         # ./mini_web_server_asan
make e2e          # ASan 서버로 end-to-end 테스트 (curl, python3 필요)
make valgrind     # valgrind 설치 시
```

## 구조

```text
src/main.c            CLI/설정 파일 → server_config_t
src/config.c          config.conf 파서, 라우팅 규칙
src/server.c          accept 루프(메인), keep-alive 루프(워커), 시그널/Graceful Shutdown
src/thread_pool.c     pthread 워커 풀 + 유한 작업 큐 (mutex + cond)
src/http_parser.c     요청 수신(Partial Read) + 파싱 + Content-Length 본문
src/http_response.c   상태 줄/헤더/에러 페이지
src/static_handler.c  정적 파일 (MIME, realpath 경로 탐색 방어, 8KB 청크 전송)
src/proxy_handler.c   리버스 프록시 (최장 prefix 매칭, Round-Robin, 502/504)
tests/                단위 테스트 + end-to-end 스크립트
www/                  샘플 정적 파일
```

## 방어 한계값

| 항목 | 한계 | 초과 시 |
|---|---|---|
| 요청 줄 + 헤더 | 8KB, 헤더 64개 | 413 |
| URI | 1KB | 414 |
| 본문 (Content-Length) | 10MB | 413 |
| 헤더/본문 수신 중 멈춤 | `request_timeout` (10초) | 408 |
| keep-alive 유휴 | `keepalive_timeout` (5초) | 연결 종료 |
| 백엔드 connect / 응답 | 3초 / 10초 | 504 (연결 거부는 502) |
