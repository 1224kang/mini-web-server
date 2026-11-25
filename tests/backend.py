#!/usr/bin/env python3
"""리버스 프록시 테스트용 백엔드.

    python3 tests/backend.py 8001 [이름]

어떤 경로로 요청하든 "누가 받았고, 무엇을 받았는지"를 JSON 으로 돌려준다.
프록시가 헤더를 어떻게 다시 썼는지(Host, X-Forwarded-For ...) 눈으로 확인하는 용도.

    GET /api/slow?ms=4000   → 4초 뒤 응답 (프록시의 504 확인용)
"""
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8001
NAME = sys.argv[2] if len(sys.argv) > 2 else f"backend-{PORT}"


class Handler(BaseHTTPRequestHandler):
    server_version = f"{NAME}/1.0"

    def _reply(self):
        url = urlsplit(self.path)
        if url.path == "/api/stream":
            size = 20 * 1024 * 1024
            self.send_response(200)
            self.send_header("Content-Length", str(size))
            self.end_headers()
            self.wfile.write(b"x" * 4096)
            time.sleep(0.75)
            for _ in range((size - 4096) // 65536):
                self.wfile.write(b"x" * 65536)
            self.wfile.write(b"x" * ((size - 4096) % 65536))
            return
        if url.path == "/api/truncated":
            self.send_response(200)
            self.send_header("Content-Length", "10")
            self.end_headers()
            self.wfile.write(b"abc")
            return
        if url.path.endswith("/slow"):
            ms = int(parse_qs(url.query).get("ms", ["3000"])[0])
            time.sleep(ms / 1000)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        data = json.dumps({
            "backend": NAME,
            "port": PORT,
            "pid": os.getpid(),
            "method": self.command,
            "path": self.path,
            "request_version": self.request_version,
            "headers": dict(self.headers.items()),
            "body": body,
        }, ensure_ascii=False, indent=2).encode("utf-8") + b"\n"
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Backend", NAME)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = _reply

    def log_message(self, fmt, *args):
        sys.stderr.write(f"[{NAME}] {self.address_string()} {fmt % args}\n")


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"[{NAME}] listening on 127.0.0.1:{PORT}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
