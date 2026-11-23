"""Bounded IPv4 TCP sender statistics from the caller's current net namespace."""

import argparse
import ipaddress
import json
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import time

PORT = 8070
NETLINK_SOCK_DIAG = 4
INFO = 2
ESTABLISHED = 1
BY_FAMILY = 20
DONE = 3
ERROR = 2
NLM_F_REQUEST_DUMP = 0x301
HEADER = struct.Struct("=IHHII")
ATTR = struct.Struct("=HH")
FIELDS = {
    "snd_mss": (16, "I"), "unacked": (24, "I"), "lost": (32, "I"),
    "rtt_us": (68, "I"), "snd_cwnd": (80, "I"), "total_retrans": (100, "I"),
    "bytes_acked": (120, "Q"), "notsent_bytes": (144, "I"),
    "delivery_rate_Bps": (160, "Q"), "busy_us": (168, "Q"),
    "rwnd_limited_us": (176, "Q"), "sndbuf_limited_us": (184, "Q"),
    "bytes_sent": (200, "Q"), "bytes_retrans": (208, "Q"),
    "snd_wnd": (228, "I"),
}


def tcp_info(data):
    return {name: struct.unpack_from("=" + kind, data, offset)[0]
            for name, (offset, kind) in FIELDS.items()
            if offset + struct.calcsize("=" + kind) <= len(data)}


def records(datagram, seq):
    offset = 0
    while offset < len(datagram):
        if len(datagram) - offset < HEADER.size:
            raise ValueError("short netlink header")
        length, kind, flags, actual_seq, _ = HEADER.unpack_from(datagram, offset)
        if length < HEADER.size or length > len(datagram) - offset or actual_seq != seq:
            raise ValueError("invalid netlink message or sequence")
        if flags & 0x10:
            raise ValueError("interrupted socket dump")
        yield kind, datagram[offset + HEADER.size:offset + length]
        offset += (length + 3) & ~3


def diag_row(data, uid, local, peer):
    if len(data) < 72 or data[:2] != bytes((socket.AF_INET, ESTABLISHED)):
        raise ValueError("invalid socket diag message")
    sport, dport = struct.unpack_from("!HH", data, 4)
    sk_uid = struct.unpack_from("=I", data, 64)[0]
    if sport != PORT or sk_uid != uid or data[8:12] != local or data[24:28] != peer:
        return None
    low, high = struct.unpack_from("=II", data, 44)
    if low == high == 0xFFFFFFFF or not dport:
        raise ValueError("missing socket cookie or peer port")
    info = None
    offset = 72
    while offset < len(data):
        if len(data) - offset < ATTR.size:
            raise ValueError("short netlink attribute")
        n, attr = ATTR.unpack_from(data, offset)
        if n < ATTR.size or n > len(data) - offset:
            raise ValueError("bad netlink attribute length")
        if attr == INFO:
            if info is not None:
                raise ValueError("duplicate TCP_INFO")
            info = tcp_info(data[offset + ATTR.size:offset + n])
        offset += (n + 3) & ~3
    if info is None or not {"bytes_acked", "bytes_sent", "total_retrans"} <= info.keys():
        raise ValueError("required TCP_INFO unavailable")
    return {"local_port": sport, "peer_port": dport, "cookie": (high << 32) | low, **info}


def query(sock, seq, uid, local, peer):
    # INET_DIAG_BC_S_EQ (11), true jump reaches end, false jump rejects.
    bytecode = struct.pack("=BBHBBH", 11, 8, 12, 0, 0, PORT)
    attr = ATTR.pack(4 + len(bytecode), 1) + bytecode
    req = struct.pack("=BBBBI", socket.AF_INET, socket.IPPROTO_TCP,
                      1 << (INFO - 1), 0, 1 << ESTABLISHED)
    req += bytes(40) + struct.pack("=II", 0xFFFFFFFF, 0xFFFFFFFF) + attr
    sock.sendto(HEADER.pack(HEADER.size + len(req), BY_FAMILY, NLM_F_REQUEST_DUMP,
                            seq, 0) + req, (0, 0))
    rows = {}
    for _ in range(32):
        packet, _, flags, sender = sock.recvmsg(65536)
        if sender != (0, 0) or flags & socket.MSG_TRUNC:
            raise ValueError("foreign or truncated netlink datagram")
        for kind, data in records(packet, seq):
            if kind == DONE:
                if len(data) < 4 or struct.unpack_from("=i", data)[0] != 0:
                    raise ValueError("failed netlink dump")
                return rows.values()
            if kind == ERROR and len(data) >= 4:
                raise OSError(-struct.unpack_from("=i", data)[0], "socket diag request failed")
            if kind != BY_FAMILY:
                raise ValueError("unexpected netlink message")
            row = diag_row(data, uid, local, peer)
            if row is not None:
                cookie = row["cookie"]
                if cookie in rows or len(rows) >= 32:
                    raise ValueError("duplicate cookie or too many matching sockets")
                rows[cookie] = row
    raise ValueError("socket dump exceeded datagram limit")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--local-ip", type=ipaddress.IPv4Address, required=True)
    parser.add_argument("--peer-ip", type=ipaddress.IPv4Address, required=True)
    parser.add_argument("--stop-file", type=Path)
    parser.add_argument("--seconds", type=int, choices=range(1, 241), default=180)
    args = parser.parse_args()
    if args.local_ip.is_unspecified or args.peer_ip.is_unspecified:
        parser.error("exact non-wildcard scope required")
    stop = False

    def stopped(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, stopped)
    uid = os.geteuid()
    start = time.monotonic_ns()
    interval = 50_000_000
    emitted = 0
    previous = {}
    with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, NETLINK_SOCK_DIAG) as sock:
        sock.bind((0, 0))
        sock.settimeout(0.15)
        for seq in range(1, args.seconds * 20 + 1):
            if stop or (args.stop_file and args.stop_file.exists()):
                break
            before = time.monotonic_ns()
            rows = query(sock, seq, uid, args.local_ip.packed, args.peer_ip.packed)
            after = time.monotonic_ns()
            active = {}
            for row in rows:
                cookie = row["cookie"]
                active[cookie] = row
                # Keep a one-second idle heartbeat, plus every TCP state change.
                if previous.get(cookie) == row and seq % 20:
                    continue
                if emitted >= 12_000:
                    raise ValueError("socket sample budget exhausted")
                print(json.dumps({"seq": seq, "sample_start_ns": before, "sample_end_ns": after,
                                  **row}, separators=(",", ":")))
                emitted += 1
            previous = active
            sys.stdout.flush()
            deadline = start + seq * interval
            time.sleep(max(0, (deadline - time.monotonic_ns()) / 1e9))
    print(f"socket_diag_samples={emitted}", file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        sys.exit(f"sender TCP_INFO: {error}")
