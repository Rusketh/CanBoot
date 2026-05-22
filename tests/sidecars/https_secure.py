#!/usr/bin/env python3
"""HTTPS server for the milestone-7 TLS smoke test.

Serves 'canboot-secure' over TLS on 127.0.0.1:8443 (reachable from the
QEMU guest as 10.0.2.2:8443 via SLIRP). Uses the pinned self-signed
cert at tests/sidecars/tls/canboot-test.{pem,key}. Session resumption is
enabled (Python's ssl module advertises tickets by default).
"""

import http.server
import os
import ssl
import sys

BODY = b"canboot-secure"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(BODY)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, format, *args):
        import sys
        sys.stderr.write("https-sidecar: " + (format % args) + "\n")
        sys.stderr.flush()


def main() -> None:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8443
    here = os.path.dirname(os.path.abspath(__file__))
    cert = os.path.join(here, "tls", "canboot-test.pem")
    key  = os.path.join(here, "tls", "canboot-test.key")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    # No client auth.
    ctx.verify_mode = ssl.CERT_NONE

    httpd = http.server.HTTPServer((host, port), Handler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
