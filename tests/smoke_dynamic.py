#!/usr/bin/env python3
"""5단계 스모크 테스트: CGI, 내장 핸들러.

실행: make smoke-dynamic   (CGI 타임아웃 포함)

서버를 띄우고 CGI 실행과 타임아웃, 내장 핸들러, keep-alive 를 확인한다.
"""
import concurrent.futures
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
BIN = os.path.join(REPO, "webserver")
PORT = int(os.environ.get("PORT", "18299"))
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
    st, _, _, _ = request("GET", "/cgi-bin/../www/index.html")
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


# ---------------------------------------------------------------- main

def main():
    if not os.path.exists(BIN):
        sys.exit("build first: make")
    conf = os.path.join(TMP, "test.conf")
    with open(conf, "w") as f:
        f.write(f"cgi_dir {os.path.join(REPO, 'cgi-bin')}\n"
                "cgi_timeout_ms 2000\n")
    log = open(os.path.join(TMP, "server.log"), "w")
    server = subprocess.Popen([BIN, "-c", conf, str(PORT), os.path.join(REPO, "www")],
                              stdout=log, stderr=subprocess.STDOUT)
    try:
        wait_port(PORT)
        test_cgi()
        test_cgi_concurrency()
        test_keepalive_mix()
        test_builtin()

        time.sleep(0.3)
        z = zombies(server.pid)
        check("좀비(<defunct>) CGI 자식 없음", z == [], z)

        server.send_signal(signal.SIGINT)
        rc = server.wait(timeout=5)
        check("SIGINT → exit 0", rc == 0, rc)
    finally:
        if server.poll() is None:
            server.kill()
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
