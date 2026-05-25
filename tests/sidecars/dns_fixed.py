#!/usr/bin/env python3
"""Minimal fixed-answer DNS server for the CanBoot DNS smoke test.

Answers A queries for a single name (default "canboot.test") with a fixed
IPv4 address (default 10.0.2.2 - the SLIRP host gateway, where the http /
https sidecars are reachable from the guest). Everything else gets
NXDOMAIN. Bound on the host loopback; the guest reaches it via SLIRP at
10.0.2.2:53.

Usage: dns_fixed.py [bind_host] [bind_port] [answer_name] [answer_ip]
"""
import socket
import struct
import sys


def parse_qname(data, off):
    labels = []
    while True:
        ln = data[off]
        if ln == 0:
            off += 1
            break
        off += 1
        labels.append(data[off:off + ln].decode("ascii", "replace"))
        off += ln
    return ".".join(labels), off


def build_response(query, answer_name, answer_ip):
    tid = query[:2]
    qd_count = struct.unpack(">H", query[4:6])[0]
    if qd_count < 1:
        return None
    name, off = parse_qname(query, 12)
    qtype, qclass = struct.unpack(">HH", query[off:off + 4])
    off += 4
    question = query[12:off]

    name_ok = name.lower().rstrip(".") == answer_name.lower().rstrip(".")
    is_a = (qtype == 1 and qclass == 1)

    if name_ok and is_a:
        flags = 0x8180          # response, recursion available, NOERROR
        ancount = 1
        rdata = socket.inet_aton(answer_ip)
        answer = (b"\xc0\x0c"                       # name pointer to question
                  + struct.pack(">HHIH", 1, 1, 60, len(rdata))
                  + rdata)
    else:
        flags = 0x8183          # NXDOMAIN
        ancount = 0
        answer = b""

    header = tid + struct.pack(">HHHHH", flags, 1, ancount, 0, 0)
    return header + question + answer


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 53
    name = sys.argv[3] if len(sys.argv) > 3 else "canboot.test"
    ip = sys.argv[4] if len(sys.argv) > 4 else "10.0.2.2"

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    sys.stderr.write("dns_fixed: %s -> %s on %s:%d\n" % (name, ip, host, port))
    sys.stderr.flush()
    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except Exception:
            continue
        if len(data) < 12:
            continue
        try:
            resp = build_response(data, name, ip)
        except Exception:
            resp = None
        if resp:
            sock.sendto(resp, addr)


if __name__ == "__main__":
    main()
