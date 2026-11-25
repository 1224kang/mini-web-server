#!/usr/bin/env python3
"""4단계 스모크 테스트: 동시 접속, keep-alive, 타임아웃, 503, SIGPIPE, SIGINT 종료.
(서버 모델: acceptor 1개 + poll 이벤트 루프 스레드 N개. 기본은 CPU 코어 수)

실행: make smoke-concurrency
(bash 3.2 로는 소켓 여러 개를 정확한 타이밍으로 붙잡고 있기 어려워서 파이썬으로 썼다.
 idle/헤더 타임아웃(5초/10초)을 실제로 기다리므로 15초 정도 걸린다.)
"""
import concurrent.futures
import os
import re
import resource
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BIN = os.environ.get("WEBSERVER", os.path.join(REPO, "webserver"))
PORT = int(os.environ.get("PORT", "18199"))

passed = failed = 0
lock = threading.Lock()


def check(name, ok, detail=""):
    global passed, failed
    with lock:
        if ok:
            passed += 1
            print(f"PASS {name}")
        else:
            failed += 1
            print(f"FAIL {name} {detail}")


# ---------------------------------------------------------------- helpers

def start_server(port, root, max_conns=None, conf=None):
    log = open(os.path.join(TMP, f"server-{port}.log"), "w")
    args = [BIN] + (["-c", conf] if conf else []) + [str(port), root] + ([str(max_conns)] if max_conns else [])
    proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("server did not start")


def connect(port=PORT, timeout=15):
    s = socket.create_connection(("127.0.0.1", port))
    s.settimeout(timeout)
    return s


def read_response(s):
    """응답 하나(헤더 + Content-Length 만큼의 본문)를 읽어
    (status, headers, body, leftover) 반환. leftover 는 그 뒤에 딸려 온 바이트.
    연결이 먼저 닫히면 None."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            return None
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.decode().split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for line in lines[1:]:
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    while len(rest) < n:
        chunk = s.recv(65536)
        if not chunk:
            break
        rest += chunk
    return status, headers, rest[:n], rest[n:]


def get(path="/", port=PORT, extra=""):
    s = connect(port)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: t\r\nConnection: close\r\n{extra}\r\n".encode())
    r = read_response(s)
    s.close()
    return r


def wait_closed(s):
    """상대가 연결을 닫을 때까지 읽는다. 받은 바이트를 반환."""
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                return data
            data += chunk
    except (ConnectionResetError, socket.timeout):
        return data


# ---------------------------------------------------------------- tests

def test_many_concurrent():
    with concurrent.futures.ThreadPoolExecutor(max_workers=50) as ex:
        results = list(ex.map(lambda _: get("/")[0], range(50)))
    check("동시 요청 50개 모두 200", results.count(200) == 50, f"{results}")


def test_slow_client_does_not_block():
    slow = connect()
    slow.sendall(b"GET / HTTP/1.1\r\n")  # 헤더를 보내다 만 손님이 워커 하나를 잡고 있음
    time.sleep(0.2)
    t0 = time.time()
    r = get("/")
    dt = time.time() - t0
    check("느린 클라이언트가 있어도 다른 요청은 바로 처리", r[0] == 200 and dt < 1.0, f"{dt:.2f}s")
    slow.close()


def test_keep_alive():
    s = connect()
    s.sendall(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
    r1 = read_response(s)
    check("HTTP/1.1 기본: Connection: keep-alive", r1[2] and r1[1].get("connection") == "keep-alive",
          f"{r1[1]}")
    s.sendall(b"GET /css/style.css HTTP/1.1\r\nHost: t\r\n\r\n")
    r2 = read_response(s)
    check("같은 연결로 두 번째 요청", r2 is not None and r2[0] == 200)
    s.sendall(b"GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
    r3 = read_response(s)
    check("Connection: close 요청 → close 응답", r3[1].get("connection") == "close")
    check("close 응답 뒤 서버가 연결을 닫음", wait_closed(s) == b"")
    s.close()


def test_http10():
    s = connect()
    s.sendall(b"GET / HTTP/1.0\r\n\r\n")
    r = read_response(s)
    check("HTTP/1.0 기본: close", r[1].get("connection") == "close")
    s.close()
    s = connect()
    s.sendall(b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
    r = read_response(s)
    check("HTTP/1.0 + keep-alive 요청 → keep-alive", r[1].get("connection") == "keep-alive")
    s.close()


def test_pipelining():
    s = connect()
    # 요청 두 개를 send 한 번에 붙여서 보낸다.
    s.sendall(b"GET /css/style.css HTTP/1.1\r\nHost: t\r\n\r\n"
              b"POST /builtin/echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\nConnection: close\r\n\r\nping")
    r1 = read_response(s)
    leftover = r1[3]
    # 두 번째 응답이 이미 같이 왔을 수 있다
    rest = leftover + wait_closed(s)
    check("파이프라이닝: 첫 응답 200", r1[0] == 200)
    check("파이프라이닝: 두 번째(POST echo) 응답", rest.startswith(b"HTTP/1.1 200") and rest.endswith(b"ping"),
          repr(rest[:80]))
    s.close()


def test_pipelining_many():
    # 요청 5개를 send 한 번에. 첫 응답을 보낸 뒤엔 새 데이터가 안 오므로(POLLIN 없음)
    # 서버가 버퍼에 남은 요청을 스스로 이어서 처리해야 한다.
    s = connect()
    reqs = b"".join(b"GET /css/style.css HTTP/1.1\r\nHost: t\r\n\r\n" for _ in range(4))
    reqs += b"GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"
    s.sendall(reqs)
    data = wait_closed(s)
    check("파이프라이닝: 요청 5개 → 응답 5개", data.count(b"HTTP/1.1 200 OK") == 5,
          f"{data.count(b'HTTP/1.1 200 OK')}개")
    s.close()


def test_big_download_does_not_starve():
    # 20MB 를 최대 속도로 받는 중에도 작은 요청이 바로 처리되는지.
    # localhost 에선 송신 버퍼가 금방 차서 send 가 EAGAIN 으로 자연히 양보하므로,
    # 이 테스트는 conn.c 의 WRITE_BUDGET 을 없애도 통과한다(직접 확인함).
    # WRITE_BUDGET 은 송신 버퍼가 크고 클라이언트가 아주 빠른 환경을 위한 상한이다.
    dl = subprocess.Popen(["curl", "-s", "-o", "/dev/null", f"http://127.0.0.1:{PORT}/big.bin"])
    time.sleep(0.05)
    worst = 0.0
    for _ in range(10):
        t0 = time.time()
        get("/")
        worst = max(worst, time.time() - t0)
    dl.wait()
    check("큰 다운로드 중에도 작은 요청 지연 < 0.5초", worst < 0.5, f"worst {worst:.3f}s")


def test_idle_timeout():
    s = connect()
    t0 = time.time()
    data = wait_closed(s)
    dt = time.time() - t0
    check("요청 없는 연결은 ~5초 뒤 닫힘 (idle timeout)", data == b"" and 4.5 <= dt <= 7, f"{dt:.2f}s")


def test_header_timeout():
    s = connect(timeout=20)
    t0 = time.time()
    # slowloris: 2초마다 한 바이트씩. recv 마다 타이머를 새로 걸면 영원히 안 끊긴다.
    stop = threading.Event()

    def drip():
        for ch in b"GET / HTTP/1.1\r\nX-Slow: " + b"a" * 100:
            if stop.is_set():
                return
            try:
                s.send(bytes([ch]))
            except OSError:
                return
            time.sleep(2 if ch == ord("a") else 0)

    threading.Thread(target=drip, daemon=True).start()
    data = wait_closed(s)
    stop.set()
    dt = time.time() - t0
    check("slowloris: 헤더 마감(10초) 지나면 408", data.startswith(b"HTTP/1.1 408") and 9 <= dt <= 13,
          f"{dt:.2f}s {data[:40]!r}")


def test_sigpipe():
    # 20MB 를 요청하고 조금만 읽은 뒤 끊는다 → 서버의 send 가 EPIPE/ECONNRESET
    for _ in range(5):
        s = connect()
        s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n")
        s.recv(1000)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
        s.close()  # SO_LINGER 0 → RST
    time.sleep(0.3)
    check("클라이언트가 중간에 끊어도 서버 생존 (SIGPIPE 무시)", SERVER.poll() is None and get("/")[0] == 200)


def write_conf(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w") as f:
        f.write(text)
    return path


def test_workers_spread():
    """workers 4: 연결이 루프 4개에 고르게 나뉘고, 각 루프가 동시에 요청을 처리한다."""
    port = PORT + 5
    proc = start_server(port, ROOT, conf=write_conf("w4.conf", "workers 4\n"))
    time.sleep(0.2)
    holders = [connect(port) for _ in range(16)]  # 놀고 있는 연결 16개
    time.sleep(0.3)
    log = open(os.path.join(TMP, f"server-{port}.log")).read()
    per_loop = [len(re.findall(rf"\[loop{i}\] accepted", log)) for i in range(1, 5)]
    check("workers 4: 시작 로그에 4 event loops", "4 event loops" in log, log[:200])
    # 접속 확인용 연결 1개가 먼저 들어갔으므로 합계 17, 루프별 4~5개
    check("연결이 가장 적은 루프로 분배 (루프별 4~5개)", sum(per_loop) == 17 and min(per_loop) >= 4,
          per_loop)

    # 놀고 있는 연결이 모든 루프에 걸려 있어도 각 연결에서 요청이 바로 처리된다
    t0 = time.time()
    ok = 0
    for s in holders:
        s.sendall(b"GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n")
    for s in holders:
        r = read_response(s)
        ok += r is not None and r[0] == 200
        s.close()
    check("모든 루프의 keep-alive 연결이 응답", ok == 16 and time.time() - t0 < 2, f"{ok}/16")

    with concurrent.futures.ThreadPoolExecutor(64) as ex:
        codes = list(ex.map(lambda _: (get("/index.html", port=port) or (0,))[0], range(400)))
    check("workers 4: 동시 요청 400개 모두 200", codes.count(200) == 400, f"{codes.count(200)}/400")

    proc.send_signal(signal.SIGINT)
    proc.wait(timeout=10)
    log = open(os.path.join(TMP, f"server-{port}.log")).read()
    m = re.search(r"connections: (\d+), requests: (\d+)", log)
    check("workers 4: 종료 통계는 모든 루프 합계", m is not None and int(m.group(2)) >= 416,
          m.group(0) if m else log[-300:])
    check("workers 4: SIGINT 정상 종료 (exit 0)", proc.returncode == 0, f"rc={proc.returncode}")


def test_single_worker():
    """workers 1: 예전 단일 루프와 같은 구조로도 동작한다."""
    port = PORT + 6
    proc = start_server(port, ROOT, conf=write_conf("w1.conf", "workers 1\n"))
    r = get("/index.html", port=port)
    proc.send_signal(signal.SIGINT)
    proc.wait(timeout=10)
    log = open(os.path.join(TMP, f"server-{port}.log")).read()
    check("workers 1: 1 event loop 로 정상 응답",
          r is not None and r[0] == 200 and "1 event loop," in log and "[loop2]" not in log, log[:200])


def test_bad_workers():
    path = write_conf("w0.conf", "workers 0\n")
    r = subprocess.run([BIN, "-c", path, str(PORT + 7), ROOT], capture_output=True, text=True, timeout=5)
    check("workers 0 → 설정 오류로 시작 안 함", r.returncode != 0 and "invalid line" in r.stderr, r.stderr)


def test_busy_503():
    port = PORT + 1
    proc = start_server(port, ROOT, max_conns=16)  # 동시 연결 16개까지
    time.sleep(0.2)                                 # start_server 의 접속 확인용 연결이 정리되게
    holders = [connect(port) for _ in range(16)]    # 16개를 열어 둠 (아무것도 안 보냄)
    time.sleep(0.3)
    extra = connect(port, timeout=3)
    data = wait_closed(extra)
    check("연결 수 한도를 넘으면 503 + Retry-After", data.startswith(b"HTTP/1.1 503") and b"Retry-After: 1" in data,
          repr(data[:60]))
    for h in holders:
        h.close()
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=10)
    check("SIGTERM 으로도 정상 종료 (exit 0)", proc.returncode == 0, f"rc={proc.returncode}")


def test_many_idle_connections():
    """C10K 맛보기: 노는 연결 1000개가 열려 있어도 새 요청이 바로 처리되는지.
    스레드 풀(워커 8 + 큐 128)이었다면 136개째부터 503 이다."""
    log = open(os.path.join(TMP, f"server-{PORT}.log")).read()
    m = re.search(r"max (\d+) connections", log)
    if not m or int(m.group(1)) < 1100:
        print(f"SKIP 노는 연결 1000개 (서버 max_conns={m.group(1) if m else '?'}: fd 한도가 낮음)")
        return
    idle = []
    try:
        for _ in range(1000):
            idle.append(connect())
    except OSError as e:
        check("노는 연결 1000개 열기", False, str(e))
        for s in idle:
            s.close()
        return
    time.sleep(0.3)
    t0 = time.time()
    r = get("/")
    dt = time.time() - t0
    check("노는 연결 1000개 + 새 요청 즉시 200", r is not None and r[0] == 200 and dt < 1.0, f"{dt:.2f}s")
    for s in idle:
        s.close()


def test_graceful_shutdown():
    port = PORT + 2
    proc = start_server(port, ROOT)

    idle = connect(port)                         # keep-alive 로 노는 연결
    idle.sendall(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
    read_response(idle)

    dl_path = os.path.join(TMP, "big.dl")       # 5초쯤 걸리는 다운로드
    curl = subprocess.Popen(["curl", "-s", "--limit-rate", "4M", "-o", dl_path,
                             f"http://127.0.0.1:{port}/big.bin"])
    time.sleep(0.7)

    t0 = time.time()
    proc.send_signal(signal.SIGINT)
    idle_data = wait_closed(idle)
    check("SIGINT: 놀던 keep-alive 연결은 바로 닫힘", idle_data == b"" and time.time() - t0 < 1.5,
          f"{time.time() - t0:.2f}s")
    time.sleep(0.3)
    try:
        socket.create_connection(("127.0.0.1", port), timeout=1).close()
        refused = False
    except OSError:
        refused = True
    check("SIGINT: 새 연결은 거절", refused)
    check("SIGINT: 진행 중인 다운로드가 있으면 아직 안 끝남", proc.poll() is None)

    curl.wait(timeout=30)
    proc.wait(timeout=15)
    same = open(dl_path, "rb").read() == open(os.path.join(ROOT, "big.bin"), "rb").read()
    check("SIGINT: 진행 중이던 20MB 다운로드는 끝까지 완료", curl.returncode == 0 and same,
          f"curl rc={curl.returncode}")
    check("SIGINT: 열린 연결이 모두 끝난 뒤 exit 0", proc.returncode == 0, f"rc={proc.returncode}")
    log = open(os.path.join(TMP, f"server-{port}.log")).read()
    check("SIGINT: 종료 로그(bye) 출력", "] bye (" in log)


def test_double_sigint():
    port = PORT + 3
    proc = start_server(port, ROOT)
    curl = subprocess.Popen(["curl", "-s", "--limit-rate", "1M", "-o", "/dev/null",
                             f"http://127.0.0.1:{port}/big.bin"])
    time.sleep(0.5)
    proc.send_signal(signal.SIGINT)
    time.sleep(0.2)
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
    check("Ctrl+C 두 번 → 즉시 강제 종료 (exit 130)", proc.returncode == 130, f"rc={proc.returncode}")
    curl.kill()
    curl.wait()


# ---------------------------------------------------------------- main

if __name__ == "__main__":
    # 이 테스트 프로세스도 소켓 1000개를 열어야 하므로 fd 한도를 올린다.
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = 4096 if hard == resource.RLIM_INFINITY else min(4096, hard)
    if soft < want:
        resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))
    TMP = tempfile.mkdtemp()
    ROOT = os.path.join(TMP, "root")
    shutil.copytree(os.path.join(REPO, "www"), ROOT)
    with open(os.path.join(ROOT, "big.bin"), "wb") as f:
        f.write(os.urandom(20 * 1024 * 1024))

    SERVER = start_server(PORT, ROOT)
    try:
        # 오래 걸리는 타임아웃 테스트는 백그라운드에서 같이 돌린다.
        slow_tests = [threading.Thread(target=t) for t in (test_idle_timeout, test_header_timeout)]
        for t in slow_tests:
            t.start()

        test_many_concurrent()
        test_slow_client_does_not_block()
        test_keep_alive()
        test_http10()
        test_pipelining()
        test_pipelining_many()
        test_big_download_does_not_starve()
        test_sigpipe()
        test_many_idle_connections()
        test_busy_503()
        test_workers_spread()
        test_single_worker()
        test_bad_workers()
        test_graceful_shutdown()
        test_double_sigint()

        for t in slow_tests:
            t.join()
    finally:
        SERVER.send_signal(signal.SIGINT)
        try:
            SERVER.wait(timeout=5)
        except subprocess.TimeoutExpired:
            SERVER.kill()

    print(f"\n{passed} passed, {failed} failed")
    if failed:
        print("--- server log ---")
        print(open(os.path.join(TMP, f"server-{PORT}.log")).read()[-4000:])
    shutil.rmtree(TMP, ignore_errors=True)
    sys.exit(1 if failed else 0)
