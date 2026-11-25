#!/bin/bash
# Phase 3~5 end-to-end 테스트 (keep-alive, 타임아웃, 동시성, 프록시, 종료). 사용법: tests/e2e_server.sh [binary]  (python3 필요)
cd "$(dirname "$0")/.." || exit 1
BIN=${1:-./mini_web_server_asan}
SCR=tests
P=18200
U=http://127.0.0.1:$P
LOG=/tmp/p5server.log
PASS=0; FAIL=0
export ASAN_OPTIONS=detect_leaks=1

check() {
  if [ "$2" == "$3" ]; then PASS=$((PASS+1)); printf "  PASS  %-52s %s\n" "$1" "$3";
  else FAIL=$((FAIL+1)); printf "  FAIL  %-52s expected [%s] got [%s]\n" "$1" "$2" "$3"; fi
}
code() { curl -s -o /dev/null -w "%{http_code}" "$@"; }
hdr()  { curl -s -D - -o /dev/null "${@:2}" | tr -d '\r' | grep -i "^$1:" | head -1 | cut -d' ' -f2-; }
json() { python3 -c "import sys,json; print(json.load(sys.stdin).get('$1'))"; }
ms_since() { echo $(( ($(date +%s%N) - $1) / 1000000 )); }

# ---------------------------------------------------------------- setup
python3 "$SCR/backend.py" 18001 & B1=$!
python3 "$SCR/backend.py" 18002 & B2=$!
head -c 20000000 /dev/urandom > www/big.bin
cat > /tmp/p5.conf <<EOF
# test config
port $P
root ./www
threads 4
queue_size 64
keepalive_timeout 2
request_timeout 3
max_keepalive_requests 100
proxy_connect_timeout 1
proxy_timeout 2
route /api/   127.0.0.1:18001 127.0.0.1:18002
route /dead/  127.0.0.1:18003
EOF
$BIN -c /tmp/p5.conf > $LOG 2>&1 & SRV=$!
sleep 1

echo "== config / startup"
check "routes printed at startup" 2 "$(grep -c '^\[INFO\] route' $LOG)"
check "static GET /"             200 "$(code $U/)"

echo "== Phase 3: keep-alive"
check "1.1 response says keep-alive"   keep-alive "$(hdr Connection $U/)"
check "Connection: close honored"      close "$(hdr Connection -H 'Connection: close' $U/)"
check "HTTP/1.0 defaults to close"     close "$(hdr Connection --http1.0 $U/)"
check "HTTP/1.0 + keep-alive"          keep-alive "$(hdr Connection --http1.0 -H 'Connection: keep-alive' $U/)"
REUSE=$(curl -sv -o /dev/null -o /dev/null -o /dev/null $U/ $U/css/style.css $U/js/app.js 2>&1 | grep -ci 're-using\|reusing')
check "3 URLs reuse 1 TCP connection"  2 "$REUSE"
PIPE=$( exec 3<>/dev/tcp/127.0.0.1/$P
        printf 'GET /data.json HTTP/1.1\r\nHost: x\r\n\r\nGET /css/style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' >&3
        cat <&3 | grep -c '^HTTP/1.1 200' )
check "pipelined 2 requests -> 2 responses" 2 "$PIPE"
START=$(date +%s%N)
IDLE=$( exec 3<>/dev/tcp/127.0.0.1/$P
        printf 'GET /data.json HTTP/1.1\r\nHost: x\r\n\r\n' >&3
        timeout 10 cat <&3 >/dev/null; echo closed )
T=$(ms_since $START)
check "idle keep-alive closed by server"   closed "$IDLE"
check "  ...after ~2s keepalive_timeout"   yes "$([ $T -ge 1800 ] && [ $T -lt 4000 ] && echo yes || echo "no(${T}ms)")"

echo "== Phase 3: timeouts (slowloris)"
START=$(date +%s%N)
SLOW=$( exec 3<>/dev/tcp/127.0.0.1/$P; printf 'GET / HTTP/1.1\r\nHost: x\r\n' >&3
        timeout 10 head -1 <&3 | tr -d '\r' | cut -d' ' -f2 )
T=$(ms_since $START)
check "half-sent headers -> 408"           408 "$SLOW"
check "  ...after ~3s request_timeout"     yes "$([ $T -ge 2800 ] && [ $T -lt 5000 ] && echo yes || echo "no(${T}ms)")"

echo "== Phase 3: concurrency"
exec 7<>/dev/tcp/127.0.0.1/$P      # idle client hogging one worker
exec 8<>/dev/tcp/127.0.0.1/$P      # another idle client
START=$(date +%s%N)
check "served while 2 idle clients hold workers" 200 "$(code $U/data.json)"
check "  ...without waiting for them"     yes "$([ $(ms_since $START) -lt 500 ] && echo yes || echo no)"
exec 7>&- 8>&-
OK=$(seq 1 400 | xargs -P 40 -I{} curl -s -o /dev/null -w "%{http_code}\n" $U/data.json | grep -c '^200$')
check "400 requests, 40 parallel clients"  400 "$OK"
PIDS=""
for i in 1 2 3 4; do curl -s $U/big.bin -o /tmp/par$i.bin & PIDS="$PIDS $!"; done
for p in $PIDS; do wait $p; done
EXPECT=$(sha256sum < www/big.bin)
N=0; for i in 1 2 3 4; do [ "$(sha256sum < /tmp/par$i.bin)" == "$EXPECT" ] && N=$((N+1)); done
rm -f /tmp/par?.bin
check "4 parallel 20MB downloads intact"   4 "$N"

echo "== Phase 4: reverse proxy"
SEQ=""; for i in 1 2 3 4 5 6; do SEQ="$SEQ$(curl -s $U/api/who | json backend) "; done
check "round-robin order"                  "18001 18002 18001 18002 18001 18002 " "$SEQ"
R=$(curl -s -H 'X-Forwarded-For: 10.0.0.1' "$U/api/users?id=7")
check "path+query forwarded unchanged"     "/api/users?id=7" "$(echo "$R" | json path)"
check "Host header forwarded"              "127.0.0.1:$P" "$(echo "$R" | json host)"
check "X-Forwarded-For appended"           "10.0.0.1, 127.0.0.1" "$(echo "$R" | json xff)"
check "X-Forwarded-Proto added"            http "$(echo "$R" | json xfp)"
check "backend sees Connection: close"     close "$(echo "$R" | json connection)"
check "backend status passed through"      404 "$(code $U/api/notfound)"
check "backend headers passed through"     yes "$([ -n "$(hdr X-Backend $U/api/who)" ] && echo yes)"
R=$(curl -s -d 'name=mini&x=1' $U/api/form)
check "POST body proxied (status)"         201 "$(code -d 'a=1' $U/api/form)"
check "POST body proxied (length)"         13 "$(echo "$R" | json body_len)"
head -c 2000000 /dev/zero | tr '\0' 'z' > /tmp/body2m
R=$(curl -s --data-binary @/tmp/body2m $U/api/upload)
check "2MB POST proxied (Expect handled)"  2000000 "$(echo "$R" | json body_len)"
check "Expect not forwarded to backend"    None "$(echo "$R" | json expect)"
check "keep-alive kept for CL responses"   keep-alive "$(hdr Connection $U/api/who)"
REUSE=$(curl -sv -o /dev/null -o /dev/null $U/api/a $U/api/b 2>&1 | grep -ci 're-using\|reusing')
check "2 proxied requests on 1 connection" 1 "$REUSE"
check "chunked backend body relayed"       "hello chunked world" "$(curl -s $U/api/chunked)"
check "  ...client conn closed after"      close "$(hdr Connection $U/api/chunked)"
check "no-length backend body (100KB)"     100000 "$(curl -s $U/api/nolen | wc -c)"
check "3MB backend body intact"            3000000 "$(curl -s $U/api/big | wc -c)"
check "backend down -> 502"                502 "$(code $U/dead/x)"
START=$(date +%s%N)
check "backend too slow -> 504"            504 "$(code $U/api/slow)"
check "  ...after ~2s proxy_timeout"       yes "$(T=$(ms_since $START); [ $T -ge 1800 ] && [ $T -lt 4500 ] && echo yes || echo "no(${T}ms)")"
check "/api/../ traversal blocked"         403 "$(code --path-as-is $U/api/../index.html)"
check "PUT to proxy path -> 405"           405 "$(code -X PUT $U/api/x)"
check "non-matching path stays static"     200 "$(code $U/data.json)"
kill $B2; wait $B2 2>/dev/null; sleep 0.3
CODES=""; for i in 1 2 3 4; do CODES="$CODES$(code $U/api/who) "; done
check "one backend killed -> 200/502 alternate" yes "$(echo "$CODES" | grep -q 200 && echo "$CODES" | grep -q 502 && echo yes || echo "no($CODES)")"
python3 "$SCR/backend.py" 18002 & B2=$!; sleep 0.8

echo "== Phase 5: robustness"
for i in $(seq 1 20); do (printf 'GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 0.05 nc -q0 127.0.0.1 $P >/dev/null 2>&1) ; done
check "clients aborting mid-download (SIGPIPE)" 200 "$(code $U/)"
check "server alive"                       yes "$(kill -0 $SRV 2>/dev/null && echo yes)"

echo "== Phase 5: graceful shutdown (SIGINT)"
exec 9<>/dev/tcp/127.0.0.1/$P; printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' >&9   # keep-alive conn left idle
curl -s --limit-rate 8M $U/big.bin -o /tmp/dl.bin & DL=$!
sleep 0.7
START=$(date +%s%N)
kill -INT $SRV
wait $DL; DLRC=$?
wait $SRV; SRVRC=$?
T=$(ms_since $START)
exec 9>&-
check "in-flight 20MB download completed"  0 "$DLRC"
check "  ...and is intact"                 "${EXPECT%% *}" "$(sha256sum < /tmp/dl.bin | cut -d' ' -f1)"
check "server exit code"                   0 "$SRVRC"
check "  ...did not wait for idle keep-alive" yes "$([ $T -lt 4000 ] && echo yes || echo "no(${T}ms)")"
check "shutdown complete logged"           1 "$(grep -c 'shutdown complete' $LOG)"
check "new connections refused after stop" 000 "$(code --max-time 2 $U/)"

echo "== Phase 5: SIGTERM + double SIGINT"
$BIN -c /tmp/p5.conf >> $LOG 2>&1 & SRV=$!; sleep 0.8
kill -TERM $SRV; wait $SRV
check "SIGTERM -> clean exit"              0 "$?"
$BIN -c /tmp/p5.conf >> $LOG 2>&1 & SRV=$!; sleep 0.8
curl -s --limit-rate 1M $U/big.bin -o /dev/null & DL=$!
sleep 0.5; kill -INT $SRV; sleep 0.3; kill -INT $SRV
wait $SRV; RC=$?; kill $DL 2>/dev/null; wait $DL 2>/dev/null
check "second SIGINT forces exit (130)"    130 "$RC"

echo "== Phase 5: queue full -> 503"
$BIN -p 18201 -t 1 -q 1 > /tmp/p5busy.log 2>&1 & BUSY=$!; sleep 0.8
exec 7<>/dev/tcp/127.0.0.1/18201; sleep 0.2   # worker busy with idle client
exec 8<>/dev/tcp/127.0.0.1/18201; sleep 0.2   # fills the queue
check "worker+queue full -> 503"           503 "$(code http://127.0.0.1:18201/)"
check "Retry-After header"                 1 "$(hdr Retry-After http://127.0.0.1:18201/)"
exec 7>&- 8>&-
kill -TERM $BUSY; wait $BUSY

# ---------------------------------------------------------------- teardown
kill $B1 $B2 2>/dev/null; wait $B1 $B2 2>/dev/null
rm -f www/big.bin /tmp/body2m /tmp/dl.bin

echo
echo "== server log excerpt"
grep -E 'shutdown|route|config|listening|WARN' $LOG | sort | uniq -c | sort -rn | head -14
echo
echo "AddressSanitizer/LeakSanitizer reports: $(grep -cE 'ERROR: (AddressSanitizer|LeakSanitizer)' $LOG /tmp/p5busy.log | awk -F: '{s+=$2} END {print s}')"
echo "RESULT: $PASS passed, $FAIL failed"
