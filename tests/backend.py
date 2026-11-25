#!/usr/bin/env python3
"""Test backend for the reverse proxy. usage: backend.py PORT"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _info(self, body=b""):
        return json.dumps({
            "backend": PORT,
            "method": self.command,
            "path": self.path,
            "host": self.headers.get("Host"),
            "xff": self.headers.get("X-Forwarded-For"),
            "xfp": self.headers.get("X-Forwarded-Proto"),
            "connection": self.headers.get("Connection"),
            "expect": self.headers.get("Expect"),
            "body_len": len(body),
            "body_head": body[:16].decode("latin-1"),
        }).encode()

    def _send(self, code, payload, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "keep-alive")   # proxy must rewrite this
        self.send_header("X-Backend", str(PORT))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path.startswith("/api/slow"):
            time.sleep(5)
            self._send(200, b"late")
        elif self.path.startswith("/api/chunked"):
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"hello ", b"chunked ", b"world"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                self.wfile.flush()
                time.sleep(0.05)
            self.wfile.write(b"0\r\n\r\n")
        elif self.path.startswith("/api/nolen"):
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"x" * 100000)
            self.close_connection = True
        elif self.path.startswith("/api/big"):
            self._send(200, b"B" * 3_000_000)
        elif self.path.startswith("/api/notfound"):
            self._send(404, b'{"error":"nope"}')
        else:
            self._send(200, self._info())

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        self._send(201, self._info(body))


ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
