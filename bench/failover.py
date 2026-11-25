#!/usr/bin/env python3
"""백엔드 장애 실험: 리버스 프록시 뒤의 백엔드 3대 중 1대가 고장 났을 때 클라이언트가 받는 응답.

단계마다 /api/ 요청을 N개(동시 10개) 보내고 상태 코드, 응답한 백엔드, 지연을 센다.

  1. normal   : 백엔드 3대 모두 정상
  2. killed   : 1대를 SIGKILL (프로세스 종료 → 연결 거부, ECONNREFUSED)
  3. recovered: 죽은 1대를 다시 띄우고 proxy_fail_timeout_ms(기본 10초)가 지난 뒤
  4. stopped  : 1대를 SIGSTOP (프로세스는 멈췄지만 커널은 연결을 받아 줌 → 응답이 오지 않음)

사용법 (저장소 루트에서):
    python3 bench/failover.py --series before=../before/webserver --series after=./webserver \\
        --csv bench/results/failover.csv
"""
import argparse
import concurrent.futures
import csv
import http.client
import json
import os
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = int(os.environ.get("BENCH_PORT", "18450"))
BACKENDS = [18451, 18452, 18453]
VICTIM = 1  # 고장 낼 백엔드: BACKENDS[1]


def wait_port(port, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"port {port} did not open")


def start_backend(port):
    p = subprocess.Popen([sys.executable, os.path.join(REPO, "tests", "backend.py"), str(port),
                          f"b{port}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_port(port)
    return p


def one_request(i):
    t = time.time()
    try:
        c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=15)
        c.request("GET", f"/api/order?n={i}", headers={"Connection": "close"})
        r = c.getresponse()
        body = r.read()
        name = ""
        if r.status == 200:
            name = json.loads(body).get("backend", "")
        c.close()
        return r.status, name, time.time() - t
    except Exception:
        return 0, "", time.time() - t  # 0 = 연결 자체가 실패


def burst(n):
    with concurrent.futures.ThreadPoolExecutor(10) as ex:
        return list(ex.map(one_request, range(n)))


def summarize(series, phase, res):
    codes = [r[0] for r in res]
    lat = sorted(r[2] * 1000 for r in res)
    by_backend = {f"b{p}": sum(1 for r in res if r[1] == f"b{p}") for p in BACKENDS}
    row = dict(series=series, phase=phase, requests=len(res),
               ok=codes.count(200), e502=codes.count(502), e504=codes.count(504),
               other=sum(1 for c in codes if c not in (200, 502, 504)),
               fail_pct=round(100 * (len(res) - codes.count(200)) / len(res), 1),
               p50_ms=round(statistics.median(lat)), p99_ms=round(lat[int(len(lat) * 0.99) - 1]),
               max_ms=round(lat[-1]), slow_over_1s=sum(1 for v in lat if v > 1000),
               dist=" ".join(f"{k}:{v}" for k, v in by_backend.items()))
    print(f"{series:>7} {phase:>9}  200={row['ok']:>4} 502={row['e502']:>4} 504={row['e504']:>4} "
          f"other={row['other']}  fail={row['fail_pct']:>5}%  p50={row['p50_ms']}ms "
          f"p99={row['p99_ms']}ms max={row['max_ms']}ms >1s={row['slow_over_1s']}  [{row['dist']}]", flush=True)
    return row


def run_series(name, binary, n, tmp):
    conf = os.path.join(tmp, "failover.conf")
    with open(conf, "w") as f:
        # 두 서버가 모두 아는 설정만 쓴다(장애 조치 설정은 기본값: 10초)
        f.write("proxy_connect_timeout_ms 1000\nproxy_timeout_ms 3000\n"
                f"proxy /api/ {' '.join(f'127.0.0.1:{p}' for p in BACKENDS)}\n")
    backends = [start_backend(p) for p in BACKENDS]
    log = open(os.path.join(tmp, f"{name}.log"), "w")
    server = subprocess.Popen([binary, "-c", conf, str(PORT), os.path.join(REPO, "www")],
                              stdout=log, stderr=subprocess.STDOUT)
    rows = []
    try:
        wait_port(PORT)
        burst(30)  # 데우기
        rows.append(summarize(name, "normal", burst(n)))

        backends[VICTIM].kill()
        backends[VICTIM].wait()
        rows.append(summarize(name, "killed", burst(n)))

        backends[VICTIM] = start_backend(BACKENDS[VICTIM])
        time.sleep(11)  # down 표시(10초)가 풀리기를 기다린다
        rows.append(summarize(name, "recovered", burst(n)))

        backends[VICTIM].send_signal(signal.SIGSTOP)
        rows.append(summarize(name, "stopped", burst(n)))
        backends[VICTIM].send_signal(signal.SIGCONT)
    finally:
        server.send_signal(signal.SIGINT)
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
        for b in backends:
            b.send_signal(signal.SIGCONT)
            b.kill()
            b.wait()
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--series", action="append", required=True, help="이름=실행파일")
    ap.add_argument("-n", type=int, default=300, help="단계마다 보낼 요청 수")
    ap.add_argument("--csv")
    args = ap.parse_args()
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for s in args.series:
            name, binary = s.split("=", 1)
            rows += run_series(name, os.path.abspath(binary), args.n, tmp)
            time.sleep(2)
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)


if __name__ == "__main__":
    sys.exit(main())
