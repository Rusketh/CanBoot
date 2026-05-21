#!/usr/bin/env python3
"""Tiny UDP echo server used by the milestone-6 smoke test."""

import socket
import sys

def main() -> None:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7777
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))
    while True:
        data, addr = sock.recvfrom(2048)
        sock.sendto(data, addr)

if __name__ == "__main__":
    main()
