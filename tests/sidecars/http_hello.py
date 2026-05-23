#!/usr/bin/env python3
"""Tiny HTTP server returning 'canboot-hello' for the net selftest GET."""

import http.server
import sys

BODY = b"canboot-hello"

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(BODY)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, *args, **kwargs):  # silence stderr
        return

def main() -> None:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    http.server.HTTPServer((host, port), Handler).serve_forever()

if __name__ == "__main__":
    main()
