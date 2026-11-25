#!/usr/bin/env python3
"""이벤트 백엔드 성능 측정.

실험 1 (idle): 노는 keep-alive 연결을 N개 붙여 둔 채로, 다른 연결들의 처리량과 지연을 잰다.
    poll 은 매 바퀴 등록된 fd 전체를 훑으므로 N 이 늘수록 느려지고,
    epoll/kqueue 는 준비된 fd 만 다루므로 거의 그대로여야 한다.
    서버는 workers 1 (루프 하나가 모든 연결을 맡게 해서 차이를 그대로 보이게).

실험 2 (workers): 노는 연결 없이 workers 수를 바꿔 가며 처리량을 잰다.
    부하 도구(ab)는 스레드 하나라 여러 개를 동시에 띄워 합친다.

사용법 (저장소 루트에서):
    python3 bench/bench.py idle    --series kqueue=./webserver:kqueue --series poll=./webserver:poll
    python3 bench/bench.py workers --series kqueue=./webserver:kqueue
  --series 이름=실행파일:백엔드  (백엔드는 WEBSERVER_EVENT_BACKEND 로 넘김. 비우면 서버 기본값)

결과는 표준 출력에 표로, --csv 로 준 파일에 CSV 로 남는다.
부하 도구는 ab(Apache Bench, macOS 기본 포함 / Ubuntu: apache2-utils)를 쓴다.
"""
import argparse
import csv
import errno
import os
import re
import resource
import select
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time

PORT = int(os.environ.get("BENCH_PORT", "18400"))
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URL_PATH = "/index.html"


def raise_nofile():
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = 30000 if hard == resource.RLIM_INFINITY else min(30000, hard)
    if soft < want:
        resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))


def start_server(binary, backend, workers, tmp):
    conf = os.path.join(tmp, f"bench-{workers}.conf")
    with open(conf, "w") as f:
        # 노는 연결이 측정 도중 닫히지 않게 keep-alive 마감을 길게
        f.write(f"workers {workers}\nkeepalive_timeout_ms 600000\n")
    env = dict(os.environ)
    if backend:
        env["WEBSERVER_EVENT_BACKEND"] = backend
    else:
        env.pop("WEBSERVER_EVENT_BACKEND", None)
    log = open(os.path.join(tmp, "server.log"), "w")
    proc = subprocess.Popen([binary, "-c", conf, str(PORT), os.path.join(REPO, "www")],
                            stdout=log, stderr=subprocess.STDOUT, env=env)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.05)
    proc.kill()
    raise RuntimeError(f"server did not start: {binary}")


def stop_server(proc):
    proc.send_signal(2)  # SIGINT: 우아한 종료
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def open_idle(n):
    """노는 연결 n 개. listen backlog 가 작아서(macOS 128) 조금씩 나눠 연다.
    임시 포트가 잠깐 모자라면(EADDRNOTAVAIL) 이전 측정의 TIME_WAIT 가 풀리기를 기다렸다 다시 연다."""
    socks = []
    deadline = time.time() + 90
    while len(socks) < n:
        try:
            for _ in range(min(100, n - len(socks))):
                socks.append(socket.create_connection(("127.0.0.1", PORT)))
        except OSError as e:
            if e.errno not in (errno.EADDRNOTAVAIL, errno.ECONNRESET) or time.time() > deadline:
                raise
            time.sleep(1)
            continue
        time.sleep(0.01)
    return socks


def close_rst(s):
    """RST 로 바로 끊는다. 보통의 close 는 먼저 닫은 쪽(여기선 클라이언트)에 TIME_WAIT 를 남겨
    임시 포트를 30초쯤 묶는다 → 수천 개를 닫고 다시 열면 포트가 모자란다."""
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    except OSError:
        pass
    s.close()


def idle_still_open(socks):
    """서버가 닫았거나(503 포함) 무언가 보낸 연결 수를 센다. 0 이어야 정상."""
    p = select.poll()
    for s in socks:
        p.register(s.fileno(), select.POLLIN)
    return len(socks) - len(p.poll(0))


def run_ab(duration, concurrency, procs):
    """ab 를 procs 개 동시에 돌려 결과를 합친다."""
    cmd = ["ab", "-q", "-k", "-c", str(concurrency), "-t", str(duration), "-n", "100000000",
           f"http://127.0.0.1:{PORT}{URL_PATH}"]
    ps = [subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
          for _ in range(procs)]
    rps, p50, p99, failed = 0.0, [], [], 0
    for p in ps:
        out, _ = p.communicate()
        m = re.search(r"Requests per second:\s+([\d.]+)", out)
        if not m:
            raise RuntimeError("ab failed:\n" + out[-800:])
        rps += float(m.group(1))
        p50.append(int(re.search(r"^\s+50%\s+(\d+)", out, re.M).group(1)))
        p99.append(int(re.search(r"^\s+99%\s+(\d+)", out, re.M).group(1)))
        failed += int(re.search(r"Failed requests:\s+(\d+)", out).group(1))
        m = re.search(r"Non-2xx responses:\s+(\d+)", out)
        failed += int(m.group(1)) if m else 0
    return rps, max(p50), max(p99), failed


def measure(binary, backend, workers, idle, args, tmp):
    proc = start_server(binary, backend, workers, tmp)
    socks = []
    try:
        socks = open_idle(idle)
        run_ab(1, args.concurrency, args.procs)  # 데우기
        alive = idle_still_open(socks)
        if alive != idle:
            raise RuntimeError(f"only {alive}/{idle} idle connections are still open")
        return run_ab(args.duration, args.concurrency, args.procs)
    finally:
        for s in socks:
            close_rst(s)
        stop_server(proc)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("experiment", choices=["idle", "workers"])
    ap.add_argument("--series", action="append", required=True, help="이름=실행파일:백엔드")
    ap.add_argument("--idle", default="0,1000,2500,5000,9000", help="실험 idle 의 노는 연결 수들")
    ap.add_argument("--workers", default="1,2,4,8", help="실험 workers 의 루프 수들")
    ap.add_argument("--duration", type=int, default=8, help="측정 1회 길이(초)")
    ap.add_argument("--repeat", type=int, default=3, help="반복 횟수 (중앙값을 쓴다)")
    ap.add_argument("--concurrency", type=int, default=50, help="ab 하나의 동시 연결 수")
    ap.add_argument("--procs", type=int, default=0, help="동시에 띄울 ab 수 (0: idle=1, workers=4)")
    ap.add_argument("--csv", help="결과 CSV 파일")
    args = ap.parse_args()
    if args.procs == 0:
        args.procs = 1 if args.experiment == "idle" else 4
    raise_nofile()

    series = []
    for s in args.series:
        name, rest = s.split("=", 1)
        binary, _, backend = rest.partition(":")
        series.append((name, os.path.abspath(binary), backend))

    if args.experiment == "idle":
        points = [("idle", int(x), 1) for x in args.idle.split(",")]
    else:
        points = [("workers", 0, int(x)) for x in args.workers.split(",")]

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, binary, backend in series:
            for _, idle, workers in points:
                runs = []
                for _ in range(args.repeat):
                    runs.append(measure(binary, backend, workers, idle, args, tmp))
                rps = statistics.median(r[0] for r in runs)
                p50 = statistics.median(r[1] for r in runs)
                p99 = statistics.median(r[2] for r in runs)
                failed = sum(r[3] for r in runs)
                row = dict(series=name, workers=workers, idle=idle, rps=round(rps),
                           p50_ms=p50, p99_ms=p99, failed=failed,
                           rps_runs=" ".join(str(round(r[0])) for r in runs))
                rows.append(row)
                print(f"{name:>10}  workers={workers}  idle={idle:>5}  "
                      f"{row['rps']:>7} req/s  p50={p50}ms  p99={p99}ms  failed={failed}  "
                      f"(runs: {row['rps_runs']})", flush=True)

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)


if __name__ == "__main__":
    sys.exit(main())
