#!/usr/bin/env bash
# 3단계 정적 파일 서빙 스모크 테스트: 서버를 실제로 띄우고 curl 로 두드려 본다.
# 실행: make smoke   (www/ 를 임시 디렉토리에 복사해서 쓰므로 www/ 는 건드리지 않는다)
set -u

PORT=${PORT:-18099}
BIN=./webserver
TMP=$(mktemp -d)
ROOT="$TMP/root"
BASE="http://127.0.0.1:$PORT"
pass=0; fail=0

cleanup() {
    [ -n "${SERVER_PID:-}" ] && { kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; }
    chmod -R u+rwx "$TMP" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

check() { # 설명, 기대값, 실제값
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$1"
    else
        fail=$((fail + 1)); printf 'FAIL %s (expected %s, got %s)\n' "$1" "$2" "$3"
    fi
}
status() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
header() { # URL, 헤더 이름
    curl -s -D - -o /dev/null "$1" | tr -d '\r' | awk -v h="$2" 'tolower($0) ~ "^"tolower(h)":" { sub(/^[^:]*: */, ""); print }'
}

# --- 테스트용 문서 루트 준비 ---------------------------------------------
cp -R www "$ROOT"
mkdir -p "$ROOT/empty_dir"                          # index.html 없는 디렉토리
echo "top secret" > "$TMP/secret.txt"               # 루트 "밖"의 파일
ln -s "$TMP/secret.txt" "$ROOT/escape_link"         # 밖을 가리키는 심볼릭 링크
ln -s "css/style.css" "$ROOT/inside_link.css"       # 안을 가리키는 심볼릭 링크 (허용)
echo "hi" > "$ROOT/hello world.txt"
echo "no" > "$ROOT/noperm.txt"; chmod 000 "$ROOT/noperm.txt"
dd if=/dev/urandom of="$ROOT/big.bin" bs=1048576 count=20 2>/dev/null   # 20MB

$BIN "$PORT" "$ROOT" > "$TMP/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 50); do curl -s -o /dev/null "$BASE/" && break; sleep 0.1; done

# --- 정상 응답 -----------------------------------------------------------
check "GET / → 200"                      200 "$(status "$BASE/")"
check "/ 는 text/html"                   "text/html; charset=utf-8" "$(header "$BASE/" Content-Type)"
check "CSS MIME"                         "text/css; charset=utf-8" "$(header "$BASE/css/style.css" Content-Type)"
check "JS MIME"                          "text/javascript; charset=utf-8" "$(header "$BASE/js/app.js" Content-Type)"
check "PNG MIME"                         "image/png" "$(header "$BASE/img/logo.png" Content-Type)"
check "쿼리스트링 무시"                    200 "$(status "$BASE/js/app.js?v=123")"
check "%20 디코딩"                        200 "$(status "$BASE/hello%20world.txt")"
check "안쪽 심볼릭 링크 허용"               200 "$(status "$BASE/inside_link.css")"

curl -s "$BASE/img/logo.png" -o "$TMP/logo.png"
check "PNG 바이트 그대로"                  same "$(cmp -s "$TMP/logo.png" www/img/logo.png && echo same || echo diff)"

curl -s "$BASE/big.bin" -o "$TMP/big.dl"
check "20MB 파일 바이트 그대로"            same "$(cmp -s "$TMP/big.dl" "$ROOT/big.bin" && echo same || echo diff)"
check "20MB Content-Length"              "$(wc -c < "$ROOT/big.bin" | tr -d ' ')" "$(header "$BASE/big.bin" Content-Length)"

# HEAD: 헤더는 GET 과 같고 본문은 0바이트
check "HEAD Content-Length 는 GET 과 같음" "$(header "$BASE/" Content-Length)" \
      "$(curl -s -I "$BASE/" | tr -d '\r' | awk 'tolower($0) ~ /^content-length:/ {print $2}')"
check "HEAD 본문 없음"                     0 "$(curl -s --head "$BASE/" -o /dev/null -w '%{size_download}')"

# --- 디렉토리 -----------------------------------------------------------
check "/docs/ → 200"                     200 "$(status "$BASE/docs/")"
check "/docs → 301"                      301 "$(status "$BASE/docs")"
check "301 Location"                     "/docs/" "$(header "$BASE/docs?x=1" Location)"
check "index 없는 디렉토리 → 403"          403 "$(status "$BASE/empty_dir/")"

# --- 에러 ---------------------------------------------------------------
check "없는 파일 → 404"                    404 "$(status "$BASE/nope.html")"
check "파일 밑의 경로 → 404 (ENOTDIR)"     404 "$(status "$BASE/index.html/x")"
if [ "$(id -u)" -ne 0 ]; then
    check "읽기 권한 없음 → 403"            403 "$(status "$BASE/noperm.txt")"
fi
check "DELETE → 405"                      405 "$(status -X DELETE "$BASE/")"
check "405 Allow 헤더" "GET, HEAD, POST" "$(curl -s -D - -o /dev/null -X DELETE "$BASE/" | tr -d '\r' | awk -F': ' 'tolower($1)=="allow"{print $2}')"
check "잘못된 %xx → 400"                  400 "$(status "$BASE/bad%zz")"
check "%00 → 400"                        400 "$(status "$BASE/index.html%00.png")"

# --- 경로 탈출(path traversal) ------------------------------------------
# curl 은 기본적으로 "../" 를 미리 정리해 버리므로 --path-as-is 로 그대로 보낸다.
check "../ 탈출 → 403"                    403 "$(status --path-as-is "$BASE/../secret.txt")"
check "깊은 ../ 탈출 → 403"               403 "$(status --path-as-is "$BASE/css/../../../etc/passwd")"
check "%2e%2e 탈출 → 403"                 403 "$(status --path-as-is "$BASE/%2e%2e/secret.txt")"
check "%2e%2e%2f 탈출 → 403"              403 "$(status --path-as-is "$BASE/%2e%2e%2fsecret.txt")"
check "심볼릭 링크 탈출 → 403"              403 "$(status "$BASE/escape_link")"
check "secret 내용 유출 없음"              "" "$(curl -s --path-as-is "$BASE/../secret.txt" | grep secret)"

# --- 2단계 기능 유지 -----------------------------------------------------
check "POST echo 유지"                    "ping" "$(curl -s -d ping "$BASE/echo")"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || { echo "--- server log ---"; cat "$TMP/server.log"; exit 1; }
