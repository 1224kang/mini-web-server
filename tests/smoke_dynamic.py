#!/usr/bin/env python3
"""5·6단계 스모크 테스트: CGI, 내장 핸들러, 리버스 프록시.

실행: make smoke-dynamic   (CGI/백엔드 타임아웃을 기다리느라 ~10초)

서버 1개 + 백엔드(tests/backend.py) 3개를 띄우고, 백엔드 하나를 죽였을 때 502 가
번갈아 나오는지, 느린 백엔드/CGI 가 504 가 되는지, 좀비가 남지 않는지까지 확인한다.
"""
import concurrent.futures
import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BIN = os.environ.get("WEBSERVER", os.path.join(REPO, "webserver"))
PORT = int(os.environ.get("PORT", "18299"))
BACKEND_PORTS = [18301, 18302, 18303]
TMP = tempfile.mkdtemp(prefix="mws-dyn-")

passed = failed = 0


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"PASS {name}")
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


# ---------------------------------------------------------------- helpers

def wait_port(port):
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"port {port} did not open")


def start_backend(port):
    log = open(os.path.join(TMP, f"backend-{port}.log"), "w")
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "backend.py"), str(port), f"b{port}"],
                         stdout=log, stderr=subprocess.STDOUT)
    wait_port(port)
    return p


def request(method, path, body=b"", headers=None, port=PORT, timeout=10):
    """요청 하나를 새 연결로 보내고 (status, headers, body, 걸린 시간) 을 돌려준다."""
    hdrs = {"Host": f"localhost:{port}", "Connection": "close"}
    hdrs.update(headers or {})
    if body or method == "POST":
        hdrs["Content-Length"] = str(len(body))
    raw = f"{method} {path} HTTP/1.1\r\n" + "".join(f"{k}: {v}\r\n" for k, v in hdrs.items()) + "\r\n"
    t0 = time.time()
    s = socket.create_connection(("127.0.0.1", port))
    s.settimeout(timeout)
    s.sendall(raw.encode() + body)
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    return parse(data, method) + (time.time() - t0,)


def parse(data, method="GET"):
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for line in lines[1:]:
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    n = 0 if method == "HEAD" else int(headers.get("content-length", "0"))
    return status, headers, rest[:n]


def read_one(s, buf, method="GET"):
    """keep-alive 연결에서 응답 하나 읽기. (status, headers, body, 남은 바이트)"""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(65536)
    head, _, rest = buf.partition(b"\r\n\r\n")
    status, headers, _ = parse(head + b"\r\n\r\n", method)
    n = 0 if method == "HEAD" else int(headers.get("content-length", "0"))
    while len(rest) < n:
        rest += s.recv(65536)
    return status, headers, rest[:n], rest[n:]


def zombies(pid):
    out = subprocess.run(["ps", "-A", "-o", "ppid=,stat="], capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if l.split()[0] == str(pid) and "Z" in l.split()[1]]


# ---------------------------------------------------------------- 5단계: CGI

def test_cgi():
    st, h, b, _ = request("GET", "/cgi-bin/add?a=1&b=2")
    check("CGI add?a=1&b=2 → 3", st == 200 and b == b"3\n", f"{st} {b!r}")
    check("CGI Content-Type 은 스크립트가 정한 값", h.get("content-type", "").startswith("text/plain"))
    st, _, b, _ = request("GET", "/cgi-bin/add?a=x&b=2")
    check("CGI Status: 400 헤더 → 400", st == 400, st)

    form = "name=%EC%A0%95%EA%B8%80&message=a+%26+b".encode()
    st, _, b, _ = request("POST", "/cgi-bin/form", form,
                          {"Content-Type": "application/x-www-form-urlencoded"})
    text = b.decode()
    check("POST 폼 → CGI 가 stdin 으로 받음", st == 200 and "정글" in text and "a &amp; b" in text,
          f"{st} {text[:200]!r}")

    st, _, b, _ = request("POST", "/cgi-bin/env/extra?q=1", b"hello",
                          {"X-Test": "yes", "Content-Type": "text/plain"})
    env = b.decode()
    for line in ["REQUEST_METHOD=POST", "QUERY_STRING=q=1", "PATH_INFO=/extra",
                 "SCRIPT_NAME=/cgi-bin/env", "CONTENT_LENGTH=5", "CONTENT_TYPE=text/plain",
                 "HTTP_X_TEST=yes", "REMOTE_ADDR=127.0.0.1", f"SERVER_PORT={PORT}",
                 "GATEWAY_INTERFACE=CGI/1.1"]:
        check(f"CGI 환경변수 {line}", line in env.splitlines(), "")
    check("CGI stdin 으로 본문 전달", env.rstrip().endswith("hello"))

    big = b"a" * 500_000  # 파이프 버퍼(64KB)보다 훨씬 큼: stdin 쓰기와 stdout 읽기가 동시에 돌아야 함
    st, _, b, _ = request("POST", "/cgi-bin/env", big)
    check("CGI 500KB 본문 왕복 (파이프 교착 없음)", st == 200 and b.count(b"a" * 1000) >= 499, f"{st} {len(b)}")

    st, h, _, _ = request("GET", "/cgi-bin/redirect")
    check("CGI Location 만 → 302", st == 302 and h.get("location") == "/form.html", f"{st} {h}")
    st, _, _, _ = request("GET", "/cgi-bin/crash")
    check("출력 없이 죽은 CGI → 502", st == 502, st)
    st, _, _, _ = request("GET", "/cgi-bin/noexec")
    check("실행 권한 없는 파일 → 403", st == 403, st)
    st, _, _, _ = request("GET", "/cgi-bin/nope")
    check("없는 스크립트 → 404", st == 404, st)
    st, _, _, _ = request("GET", "/cgi-bin/../conf/proxy.conf")
    check("/cgi-bin/../ → 404", st == 404, st)
    st, h, _, _ = request("DELETE", "/cgi-bin/add")
    check("CGI 에 DELETE → 405 + Allow", st == 405 and "POST" in h.get("allow", ""), f"{st} {h}")
    st, h, b, _ = request("HEAD", "/cgi-bin/add?a=1&b=2")
    check("HEAD CGI → 본문 없이 Content-Length 2", st == 200 and b == b"" and h.get("content-length") == "2",
          f"{st} {h} {b!r}")

    st, _, _, dt = request("GET", "/cgi-bin/sleep?37")
    check("CGI 타임아웃(2초) → 504", st == 504 and 1.5 < dt < 3.5, f"{st} {dt:.2f}s")
    time.sleep(0.2)
    left = subprocess.run(["pgrep", "-f", "^sleep 37$"], capture_output=True, text=True).stdout.strip()
    check("타임아웃 때 손자 프로세스(sleep)까지 kill", left == "", left)


def test_cgi_concurrency():
    # 느린 CGI 가 도는 동안에도 루프는 다른 요청을 처리해야 한다 (waitpid/read 로 기다리지 않음)
    with concurrent.futures.ThreadPoolExecutor(4) as ex:
        slow = ex.submit(request, "GET", "/cgi-bin/sleep?1")
        time.sleep(0.2)
        st, _, _, dt = request("GET", "/index.html")
        check("CGI 실행 중에도 정적 파일 즉시 응답", st == 200 and dt < 0.3, f"{st} {dt:.2f}s")
        check("느린 CGI 도 정상 완료", slow.result()[0] == 200)

    with concurrent.futures.ThreadPoolExecutor(20) as ex:
        futs = [ex.submit(request, "GET", f"/cgi-bin/add?a={i}&b={i}") for i in range(40)]
        ok = all(f.result()[0] == 200 and f.result()[2] == f"{2 * i}\n".encode()
                 for i, f in enumerate(futs))
    check("CGI 40개 동시 실행 → 결과 모두 정확", ok)


def test_keepalive_mix():
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(10)
    reqs = [("GET", "/cgi-bin/add?a=2&b=3"), ("GET", "/builtin/add?a=2&b=3"), ("GET", "/css/style.css")]
    s.sendall(b"".join(f"{m} {p} HTTP/1.1\r\nHost: t\r\n\r\n".encode() for m, p in reqs))
    buf = b""
    results = []
    for m, _ in reqs:
        st, h, b, buf = read_one(s, buf, m)
        results.append((st, b, h.get("connection")))
    s.close()
    check("한 연결에서 CGI → 내장 → 정적 (파이프라이닝)",
          [r[0] for r in results] == [200, 200, 200] and results[0][1] == b"5\n" and results[1][1] == b"5\n"
          and all(r[2] == "keep-alive" for r in results), results)


def test_builtin():
    st, _, b, _ = request("GET", "/builtin/add?a=40&b=2")
    check("내장 add → 42", st == 200 and b == b"42\n", f"{st} {b!r}")
    st, h, b, _ = request("POST", "/builtin/echo", b"ping", {"Content-Type": "text/x-test"})
    check("내장 echo", st == 200 and b == b"ping" and h.get("content-type") == "text/x-test")
    st, _, _, _ = request("POST", "/index.html", b"x")
    check("정적 경로에 POST → 405", st == 405, st)


# ---------------------------------------------------------------- 요청 본문 Transfer-Encoding

def raw(parts, delay=0.0, timeout=10):
    """원본 바이트를 (나눠서) 보내고 서버가 닫을 때까지 읽는다. (status, headers, body, 원본)"""
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(timeout)
    for p in parts:
        s.sendall(p)
        if delay:
            time.sleep(delay)
    data = b""
    while True:
        try:
            chunk = s.recv(65536)
        except ConnectionResetError:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    st, h, b = parse(data)
    return st, h, b, data


def chunked(body, size):
    out = b""
    for i in range(0, len(body), size):
        piece = body[i:i + size]
        out += f"{len(piece):x}\r\n".encode() + piece + b"\r\n"
    return out + b"0\r\n\r\n"


def test_request_chunked():
    head = (b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n"
            b"Content-Type: text/x-test\r\nConnection: close\r\n\r\n")
    st, h, b, _ = raw([head + b"5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-Trailer: 1\r\n\r\n"])
    check("chunked 요청 본문 → 내장 echo 가 디코딩된 본문 받음",
          st == 200 and b == b"hello world" and h.get("content-length") == "11", f"{st} {b!r}")

    # 청크 경계를 가로질러 1~3 바이트씩 나눠 보내기
    body = chunked(b"abcdefghij" * 3, 7)
    pieces = [head] + [body[i:i + 3] for i in range(0, len(body), 3)]
    st, _, b, _ = raw(pieces, delay=0.002)
    check("chunked 본문을 잘게 나눠 보내도 정확히 조립", st == 200 and b == b"abcdefghij" * 3, f"{st} {b!r}")

    big = os.urandom(300_000)
    st, _, b, _ = raw([head + chunked(big, 4000)])
    check("chunked 300KB(청크 75개) 왕복", st == 200 and b == big, f"{st} {len(b)}")

    st, h, b, _ = raw([head + b"0\r\n\r\n"])
    check("빈 chunked 본문 → Content-Length 0", st == 200 and b == b"" and h.get("content-length") == "0",
          f"{st} {h}")

    # keep-alive: chunked 요청 뒤에 이어 붙은 요청도 처리
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(10)
    s.sendall(b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n"
              b"3\r\nabc\r\n0\r\n\r\n"
              b"GET /builtin/add?a=2&b=3 HTTP/1.1\r\nHost: t\r\n\r\n")
    st1, h1, b1, buf = read_one(s, b"")
    st2, _, b2, _ = read_one(s, buf)
    s.close()
    check("chunked 요청 + 파이프라이닝된 다음 요청",
          (st1, b1, h1.get("connection"), st2, b2) == (200, b"abc", "keep-alive", 200, b"5\n"),
          (st1, b1, st2, b2))

    # 헤더가 버퍼를 거의 다 채운 경우: 임시 버퍼로 받고 연결을 닫는다
    pad = "x" * 7900
    big_head = (f"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nX-Pad: {pad}\r\n"
                "Transfer-Encoding: chunked\r\n\r\n").encode()
    st, h, b, _ = raw([big_head, chunked(b"z" * 5000, 1000)], delay=0.05)
    check("큰 헤더 + chunked 본문 → 정상 처리 후 연결 닫음",
          st == 200 and b == b"z" * 5000 and h.get("connection") == "close", f"{st} {len(b)} {h}")

    st, _, b, _ = raw([b"POST /cgi-bin/env HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n"
                       b"Connection: close\r\n\r\n" + chunked(b"hello cgi", 4)])
    env = b.decode()
    check("chunked 본문 → CGI 는 CONTENT_LENGTH 와 stdin 으로 받음",
          st == 200 and "CONTENT_LENGTH=9" in env.splitlines() and env.rstrip().endswith("hello cgi"),
          f"{st} {env[-200:]!r}")

    bad = [
        ("TE + Content-Length → 400", 400,
         b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n"
         b"Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"),
        ("TE: gzip, chunked → 501", 501,
         b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n"),
        ("TE: chunked, gzip (chunked 가 마지막 아님) → 400", 400,
         b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked, gzip\r\n\r\n"),
        ("HTTP/1.0 + TE → 400", 400,
         b"POST /builtin/echo HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
        ("잘못된 청크 길이 → 400", 400,
         b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"),
        ("청크 데이터 뒤 CRLF 없음 → 400", 400,
         b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX"),
    ]
    for name, want, data in bad:
        st, h, _, _ = raw([data])
        check(name + " (연결 닫음)", st == want and h.get("connection") == "close", f"{st} {h}")

    st, h, _, _ = raw([head + chunked(b"q" * (1024 * 1024 + 1), 65536)])
    check("chunked 본문 1MB 초과 → 413", st == 413 and h.get("connection") == "close", f"{st} {h}")

    # smuggling 시도: TE 를 무시하는 서버라면 뒤의 GET 을 별도 요청으로 처리한다
    st, _, _, data = raw([b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n"
                          b"Content-Length: 4\r\n\r\n0\r\n\r\nGET /builtin/add?a=1&b=1 HTTP/1.1\r\nHost: t\r\n\r\n"])
    check("smuggling 시도(TE+CL) → 응답은 400 하나뿐", st == 400 and data.count(b"HTTP/1.1 ") == 1,
          data[:200])


# ---------------------------------------------------------------- 6단계: 프록시

def test_proxy_stream():
    size = 20 * 1024 * 1024
    with socket.create_connection(("127.0.0.1", PORT), timeout=10) as s:
        start = time.monotonic()
        s.sendall(b"GET /api/stream HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
        with s.makefile("rb") as stream:
            head = b""
            while (line := stream.readline()) not in (b"\r\n", b""):
                head += line
            first = stream.read(4096)
            check("프록시 본문이 백엔드 전송 완료 전에 도착", first == b"x" * 4096
                  and time.monotonic() - start < 0.70)
            time.sleep(0.9)  # 읽지 않는 동안 프록시의 클라이언트 소켓 송신 버퍼가 찬다.
            st, _, _, elapsed = request("GET", "/index.html")
            check("프록시 클라이언트가 읽지 않아도 정적 요청 처리", st == 200 and elapsed < 0.5)
            status, headers, _ = parse(head + b"\r\n")
            total, valid = len(first), True
            while data := stream.read(65536):
                total += len(data)
                valid = valid and data == b"x" * len(data)
            check("20 MiB 프록시 응답의 길이와 내용 유지", status == 200 and valid
                  and int(headers["content-length"]) == total == size)
    st, h, b, _ = request("GET", "/api/truncated", headers={"Connection": "keep-alive"})
    check("본문 중간에 끊긴 프록시 응답은 연결 종료",
          st == 200 and h.get("content-length") == "10" and b == b"abc")


def test_proxy_framing():
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=4)
    try:
        c.request("GET", "/api/chunked")
        sock = c.sock
        r = c.getresponse()
        check("HTTP/1.1 chunked 본문 중계와 헤더 재작성",
              r.status == 200 and r.chunked and r.getheader("Trailer") is None
              and r.read() == b"hello world")
        c.request("GET", "/api/informational")
        r = c.getresponse()
        check("chunked 이후 같은 연결에서 최종 응답만 전송",
              c.sock is sock and r.status == 200 and r.read() == b"ok")
        c.request("GET", "/api/eof")
        r = c.getresponse()
        check("길이 없는 응답은 종료까지 중계",
              r.status == 200 and r.will_close and r.read() == b"until close")
    finally:
        c.close()

    with socket.create_connection(("127.0.0.1", PORT), timeout=4) as s:
        s.sendall(b"GET /api/chunked HTTP/1.0\r\nHost: t\r\n\r\n")
        with s.makefile("rb") as stream:
            head, _, body = stream.read().partition(b"\r\n\r\n")
        check("HTTP/1.0 클라이언트에게 chunked 디코딩 후 연결 종료",
              head.startswith(b"HTTP/1.0 200") and b"Transfer-Encoding:" not in head
              and body == b"hello world")
    with socket.create_connection(("127.0.0.1", PORT), timeout=4) as s:
        s.sendall(b"GET /api/chunk-cut HTTP/1.1\r\nHost: t\r\n\r\n"
                  b"GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n")
        with s.makefile("rb") as stream:
            head, _, body = stream.read().partition(b"\r\n\r\n")
        check("잘린 chunked 응답에 종료 청크나 다음 응답을 보내지 않음",
              head.startswith(b"HTTP/1.1 200") and body == b"3\r\nabc\r\n")


def test_configured_timeouts():
    with socket.create_connection(("127.0.0.1", PORT), timeout=2) as s:
        s.sendall(b"GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n")
        st, _, _, _ = read_one(s, b"")
        start = time.monotonic()
        check("설정한 600 ms 뒤 유휴 keep-alive 닫힘",
              st == 200 and s.recv(1) == b"" and 0.35 < time.monotonic() - start < 1.5)

    with socket.create_connection(("127.0.0.1", PORT), timeout=2) as s:
        s.sendall(b"GET /")
        start = time.monotonic()
        for _ in range(3):
            time.sleep(0.1)
            s.sendall(b"x")
        with s.makefile("rb") as stream:
            status = stream.readline()
        check("헤더 일부를 반복해서 보내도 350 ms 전체 마감 적용",
              b" 408 " in status and time.monotonic() - start < 0.6)

    with socket.create_connection(("127.0.0.1", PORT), timeout=2) as s:
        s.sendall(b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nContent-Length: 6\r\n\r\nabc")
        start = time.monotonic()
        with s.makefile("rb") as stream:
            status = stream.readline()
        check("설정한 본문 수신 마감에 408",
              b" 408 " in status and 0.2 < time.monotonic() - start < 1.5)


def test_proxy(backends):
    seq = []
    for _ in range(6):
        st, h, b, _ = request("GET", "/api/rr")
        seq.append(json.loads(b)["backend"] if st == 200 else st)
    names = [f"b{p}" for p in BACKEND_PORTS]
    check("Round-Robin: 3개에 번갈아 분배", seq == names * 2, seq)

    # 이벤트 루프 여러 개가 동시에 백엔드를 골라도 카운터(atomic)가 겹치지 않는다
    with concurrent.futures.ThreadPoolExecutor(30) as ex:
        got = list(ex.map(lambda _: request("GET", "/api/rr-par"), range(60)))
    picked = [json.loads(b)["backend"] for st, _, b, _ in got if st == 200]
    counts = {n: picked.count(n) for n in names}
    check("여러 루프에서 동시에 60개 → 백엔드마다 정확히 20개", len(picked) == 60 and
          all(v == 20 for v in counts.values()), counts)
    test_proxy_stream()
    test_proxy_framing()

    st, h, b, _ = request("GET", "/api/hdr?x=1", headers={"X-Forwarded-For": "1.2.3.4", "X-Hop": "1",
                                                           "Connection": "close, X-Hop"})
    j = json.loads(b)
    bh = {k.lower(): v for k, v in j["headers"].items()}
    check("백엔드가 받은 경로 그대로", j["path"] == "/api/hdr?x=1", j["path"])
    check("Host → 백엔드 주소로 재작성", bh.get("host", "").startswith("127.0.0.1:"), bh.get("host"))
    check("X-Forwarded-Host = 원래 Host", bh.get("x-forwarded-host") == f"localhost:{PORT}", bh)
    check("X-Forwarded-For 에 클라이언트 IP 덧붙임", bh.get("x-forwarded-for") == "1.2.3.4, 127.0.0.1",
          bh.get("x-forwarded-for"))
    check("X-Forwarded-Proto: http", bh.get("x-forwarded-proto") == "http")
    check("백엔드와는 HTTP/1.1 + Connection: close",
          j["request_version"] == "HTTP/1.1" and bh.get("connection") == "close", bh)
    check("hop-by-hop(Connection 에 적힌 X-Hop) 제거", "x-hop" not in bh, bh)
    check("응답: 백엔드 헤더(X-Backend) 유지 + Via 추가",
          h.get("x-backend", "").startswith("b") and h.get("via") == "1.1 mini-web-server", h)

    st, _, b, _ = request("POST", "/api/post", b'{"k": 1}', {"Content-Type": "application/json"})
    j = json.loads(b)
    check("POST 본문 전달", st == 200 and j["body"] == '{"k": 1}' and j["method"] == "POST", j)
    st, h, b, _ = request("HEAD", "/api/head")
    check("HEAD 프록시 → 본문 없이 Content-Length", st == 200 and b == b"" and int(h["content-length"]) > 0, h)

    # keep-alive: 클라이언트 연결 하나로 여러 백엔드 응답
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(10)
    s.sendall(b"GET /api/k1 HTTP/1.1\r\nHost: t\r\n\r\nGET /api/k2 HTTP/1.1\r\nHost: t\r\n\r\n")
    st1, h1, b1, buf = read_one(s, b"")
    st2, h2, b2, _ = read_one(s, buf)
    s.close()
    check("클라이언트와는 keep-alive 유지 (백엔드는 매번 새 연결)",
          st1 == st2 == 200 and h1.get("connection") == "keep-alive"
          and json.loads(b1)["backend"] != json.loads(b2)["backend"], (st1, st2, h1.get("connection")))

    st, _, b, _ = raw([b"POST /api/chunked-req HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n"
                       b"Connection: close\r\n\r\n" + chunked(b"via proxy", 3)])
    j = json.loads(b) if st == 200 else {}
    jh = {k.lower(): v for k, v in j.get("headers", {}).items()}
    check("chunked 요청 → 백엔드에는 Content-Length 로 전달",
          j.get("body") == "via proxy" and jh.get("content-length") == "9"
          and "transfer-encoding" not in jh, f"{st} {j}")

    # 백엔드 하나 죽이기
    backends[1].terminate()
    backends[1].wait()
    codes = [request("GET", "/api/dead")[0] for _ in range(6)]
    check("백엔드 하나 죽이면 3번에 1번 502", sorted(codes) == [200, 200, 200, 200, 502, 502], codes)

    st, _, _, dt = request("GET", "/api/slow?ms=3000")
    # 죽은 백엔드에 걸리면 502 가 먼저 나오므로 살아 있는 백엔드에 걸릴 때까지
    while st == 502:
        st, _, _, dt = request("GET", "/api/slow?ms=3000")
    check("느린 백엔드(3초) → proxy_timeout(1초) 후 504", st == 504 and 0.8 < dt < 2.0, f"{st} {dt:.2f}s")

    st, _, _, _ = request("GET", "/index.html")
    check("프록시 실패 뒤에도 정적 파일 정상", st == 200)


# ---------------------------------------------------------------- main

def main():
    if not os.path.exists(BIN):
        sys.exit("build first: make")
    conf = os.path.join(TMP, "test.conf")
    with open(conf, "w") as f:
        f.write("workers 4\n"
                f"cgi_dir {os.path.join(REPO, 'cgi-bin')}\n"
                "cgi_timeout_ms 2000\n"
                "keepalive_timeout_ms 600\n"
                "header_timeout_ms 350\n"
                "body_timeout_ms 350\n"
                "proxy_connect_timeout_ms 500\n"
                "proxy_timeout_ms 1000\n"
                "proxy /api/ " + " ".join(f"127.0.0.1:{p}" for p in BACKEND_PORTS) + "\n")

    backends = [start_backend(p) for p in BACKEND_PORTS]
    log = open(os.path.join(TMP, "server.log"), "w")
    server = subprocess.Popen([BIN, "-c", conf, str(PORT), os.path.join(REPO, "www")],
                              stdout=log, stderr=subprocess.STDOUT)
    try:
        wait_port(PORT)
        test_cgi()
        test_cgi_concurrency()
        test_keepalive_mix()
        test_builtin()
        test_request_chunked()
        test_configured_timeouts()
        test_proxy(backends)

        time.sleep(0.3)
        z = zombies(server.pid)
        check("좀비(<defunct>) CGI 자식 없음", z == [], z)

        server.send_signal(signal.SIGINT)
        rc = server.wait(timeout=5)
        check("SIGINT → exit 0", rc == 0, rc)
    finally:
        if server.poll() is None:
            server.kill()
        for b in backends:
            if b.poll() is None:
                b.terminate()
        print(f"\n{passed} passed, {failed} failed")
        if failed:
            log.flush()
            print("--- server log (tail) ---")
            with open(os.path.join(TMP, "server.log")) as f:
                print("".join(f.readlines()[-40:]))
        shutil.rmtree(TMP, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
