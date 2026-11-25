#!/bin/bash
# Phase 2 정적 파일/파서 end-to-end 테스트. 사용법: tests/e2e_static.sh [binary]  (기본 ./mini_web_server_asan)
cd "$(dirname "$0")/.." || exit 1
BIN=${1:-./mini_web_server_asan}
P=18100
LOG=/tmp/p2server.log
PASS=0; FAIL=0

check() { # name expected actual
  if [ "$2" == "$3" ]; then PASS=$((PASS+1)); printf "  PASS  %-48s %s\n" "$1" "$3";
  else FAIL=$((FAIL+1)); printf "  FAIL  %-48s expected [%s] got [%s]\n" "$1" "$2" "$3"; fi
}
code()  { curl -s -o /dev/null -w "%{http_code}" "$@"; }
ctype() { curl -s -o /dev/null -w "%{content_type}" "$@"; }
hdr()   { curl -s -D - -o /dev/null "${@:2}" | tr -d '\r' | grep -i "^$1:" | cut -d' ' -f2-; }
raw()   { # send raw bytes (\r\n escapes) and print status line
  exec 3<>/dev/tcp/127.0.0.1/$P; printf '%b' "$1" >&3; head -1 <&3 | tr -d '\r' | cut -d' ' -f2; exec 3<&-; }

# fixtures (removed at the end)
head -c 20000000 /dev/urandom > www/big.bin
ln -sf /etc/passwd www/evil-link
ln -sfn /etc www/etc-link
HAVE_FIFO=0; mkfifo www/fifo 2>/dev/null && HAVE_FIFO=1   # NTFS(/mnt/c) 에서는 생성 불가
head -c 2000000 /dev/zero > /tmp/body2m
head -c 11000000 /dev/zero > /tmp/body11m

$BIN $P > $LOG 2>&1 & SRV=$!
sleep 0.8
U=http://127.0.0.1:$P

echo "== static files & MIME"
check "GET /"                       200 "$(code $U/)"
check "GET / content-type"          "text/html; charset=utf-8" "$(ctype $U/)"
check "GET /index.html == GET /"    "$(curl -s $U/index.html | sha1sum)" "$(curl -s $U/ | sha1sum)"
check "css"                         "text/css; charset=utf-8" "$(ctype $U/css/style.css)"
check "js"                          "text/javascript; charset=utf-8" "$(ctype $U/js/app.js)"
check "svg"                         "image/svg+xml" "$(ctype $U/images/logo.svg)"
check "json"                        "application/json" "$(ctype $U/data.json)"
check "unknown ext -> octet-stream" "application/octet-stream" "$(ctype $U/big.bin)"
check "Content-Length matches file" "$(stat -c %s www/data.json)" "$(hdr Content-Length $U/data.json)"
check "query string ignored"        200 "$(code "$U/data.json?v=1&x=2")"
check "percent-decoded name"        200 "$(code $U/data%2Ejson)"
check "/docs/ -> docs/index.html"   200 "$(code $U/docs/)"
check "/docs -> 301"                301 "$(code $U/docs)"
check "/docs Location"              "/docs/" "$(hdr Location $U/docs)"
check "/docs?x=1 Location"          "/docs/?x=1" "$(hdr Location "$U/docs?x=1")"
check "/images/ (no index) -> 404"  404 "$(code $U/images/)"
check "missing file -> 404"         404 "$(code $U/nope.html)"
check "error page is html"          "text/html; charset=utf-8" "$(ctype $U/nope.html)"

echo "== large file (20MB, 8KB chunks)"
check "big.bin sha256"              "$(sha256sum < www/big.bin)" "$(curl -s $U/big.bin | sha256sum)"

echo "== path traversal"
check "/../../etc/passwd (as-is)"   403 "$(code --path-as-is $U/../../etc/passwd)"
check "/%2e%2e/%2e%2e/etc/passwd"   403 "$(code --path-as-is $U/%2e%2e/%2e%2e/etc/passwd)"
check "/css/..%2F..%2Fetc/passwd"   403 "$(code --path-as-is $U/css/..%2F..%2Fetc/passwd)"
check "symlink to /etc/passwd"      403 "$(code $U/evil-link)"
check "symlink dir /etc-link/passwd" 403 "$(code $U/etc-link/passwd)"
if [ $HAVE_FIFO = 1 ]; then check "FIFO (non-regular file)" 403 "$(code $U/fifo)"; else echo "  SKIP  FIFO (cannot mkfifo on this filesystem)"; fi
check "%00 in path"                 400 "$(raw 'GET /index.html%00.png HTTP/1.1\r\nHost: x\r\n\r\n')"
check "%0d%0a in path"              400 "$(raw 'GET /a%0d%0aSet-Cookie:x HTTP/1.1\r\nHost: x\r\n\r\n')"

echo "== methods"
check "PUT -> 405"                  405 "$(code -X PUT $U/)"
check "PUT Allow header"            "GET, POST" "$(hdr Allow -X PUT $U/)"
check "DELETE -> 405"               405 "$(code -X DELETE $U/)"
check "HEAD -> 405"                 405 "$(raw 'HEAD / HTTP/1.1\r\nHost: x\r\n\r\n')"
check "lowercase 'get' -> 405"      405 "$(raw 'get / HTTP/1.1\r\nHost: x\r\n\r\n')"
check "POST to static -> 405"       405 "$(code -d 'a=1' $U/)"
check "POST static Allow"           "GET" "$(hdr Allow -d 'a=1' $U/)"

echo "== size limits"
LONG=$(head -c 2000 /dev/zero | tr '\0' 'a')
check "URI 2000B -> 414"            414 "$(code $U/$LONG)"
HUGE=$(head -c 9000 /dev/zero | tr '\0' 'a')
check "request line 9000B -> 414"   414 "$(code $U/$HUGE)"
check "header 9000B -> 413"         413 "$(code -H "X-Big: $HUGE" $U/)"
check "CL 11MB (curl, Expect) -> 413" 413 "$(code --data-binary @/tmp/body11m $U/)"
check "CL 99999999999999999999 -> 413" 413 "$(raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n')"
H=""; for i in $(seq 1 70); do H="$H -H X-H$i:v"; done
check "70 headers -> 413"           413 "$(code $H $U/)"

echo "== body reading"
START=$(date +%s%N)
check "POST 2MB (Expect 100-continue)" 405 "$(code --data-binary @/tmp/body2m $U/)"
MS=$(( ($(date +%s%N) - START) / 1000000 ))
check "  ...answered without 1s Expect stall" yes "$([ $MS -lt 900 ] && echo yes || echo "no(${MS}ms)")"
check "POST 2MB with Expect disabled" 405 "$(code -H 'Expect:' --data-binary @/tmp/body2m $U/)"

echo "== malformed requests"
check "garbage line"                400 "$(raw 'GARBAGE\r\n\r\n')"
check "missing Host (1.1)"          400 "$(raw 'GET / HTTP/1.1\r\n\r\n')"
check "HTTP/1.0 without Host"       200 "$(raw 'GET / HTTP/1.0\r\n\r\n')"
check "duplicate Host"              400 "$(raw 'GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n')"
check "HTTP/2.0 -> 505"             505 "$(raw 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')"
check "bad version string"          400 "$(raw 'GET / HTTX/1.1\r\nHost: x\r\n\r\n')"
check "absolute-form target"        400 "$(raw 'GET http://x/ HTTP/1.1\r\nHost: x\r\n\r\n')"
check "double space in req line"    400 "$(raw 'GET  / HTTP/1.1\r\nHost: x\r\n\r\n')"
check "bad percent %zz"             400 "$(raw 'GET /a%zz HTTP/1.1\r\nHost: x\r\n\r\n')"
check "truncated percent %2"        400 "$(raw 'GET /a%2 HTTP/1.1\r\nHost: x\r\n\r\n')"
check "header without colon"        400 "$(raw 'GET / HTTP/1.1\r\nHost: x\r\nBadHeader\r\n\r\n')"
check "space before colon"          400 "$(raw 'GET / HTTP/1.1\r\nHost : x\r\n\r\n')"
check "obs-fold header"             400 "$(raw 'GET / HTTP/1.1\r\nHost: x\r\nX-A: 1\r\n  folded\r\n\r\n')"
check "Content-Length: abc"         400 "$(raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n')"
check "Content-Length: -1"          400 "$(raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n')"
check "conflicting Content-Length"  400 "$(raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab')"
check "Transfer-Encoding chunked"   501 "$(raw 'POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n')"
check "TE + CL (smuggling)"         400 "$(raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n')"
check "leading CRLFs ignored"       200 "$(raw '\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n')"
check "OWS around header value"     200 "$(raw 'GET / HTTP/1.1\r\nHost:    x   \r\n\r\n')"

echo "== partial reads (TCP split)"
R=$( exec 3<>/dev/tcp/127.0.0.1/$P
     printf 'GE' >&3; sleep 0.2; printf 'T /data.js' >&3; sleep 0.2
     printf 'on HTTP/1.1\r\nHo' >&3; sleep 0.2; printf 'st: x\r\n' >&3; sleep 0.2; printf '\r\n' >&3
     head -1 <&3 | tr -d '\r' | cut -d' ' -f2 )
check "request split into 5 writes"  200 "$R"
R=$( exec 3<>/dev/tcp/127.0.0.1/$P
     printf 'PUT / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n12345' >&3; sleep 0.3; printf '67890' >&3
     head -1 <&3 | tr -d '\r' | cut -d' ' -f2 )
check "body split (5+5 bytes) fully read" 405 "$R"
R=$( exec 3<>/dev/tcp/127.0.0.1/$P; printf 'GET / HTTP/1.1\r\nHost: x\r\n' >&3; exec 3>&-; sleep 0.2; echo sent )
check "headers cut off mid-way"     sent "$R"
R=$( exec 3<>/dev/tcp/127.0.0.1/$P; printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nshort' >&3; exec 3>&-; sleep 0.2; echo sent )
check "body shorter than CL"        sent "$R"
R=$( exec 3<>/dev/tcp/127.0.0.1/$P; exec 3>&-; sleep 0.2; echo sent )
check "connect + close, no data"    sent "$R"

echo "== server health"
check "still serving after all tests" 200 "$(code $U/)"
check "process alive"               yes "$(kill -0 $SRV 2>/dev/null && echo yes)"

kill -TERM $SRV; wait $SRV 2>/dev/null
rm -f www/big.bin www/evil-link www/etc-link www/fifo /tmp/body2m /tmp/body11m

echo
echo "== server log (tail)"; tail -n 22 $LOG
echo
echo "AddressSanitizer reports: $(grep -c 'ERROR: AddressSanitizer' $LOG)"
echo "RESULT: $PASS passed, $FAIL failed"
