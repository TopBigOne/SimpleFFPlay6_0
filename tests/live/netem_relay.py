#!/usr/bin/env python3
"""UDP network impairment relay for live-stream tests (a tiny userspace "netem").

Forwards UDP datagrams from 127.0.0.1:<in_port> to 127.0.0.1:<out_port> while applying:
  --loss P            drop each datagram with probability P (0..1)
  --blackout T:D      drop everything between T and T+D seconds (burst loss)
  --jitter MS         add a random 0..MS ms delay to each datagram (order preserved)
  --reorder P         with --jitter, let a datagram skip the ordering constraint with probability P
  --stall T:D         hold everything between T and T+D seconds, then release it as a burst
Times are measured from the first datagram received. Runs for --duration seconds.
Prints a one-line JSON summary on exit.
"""
import argparse
import heapq
import json
import random
import signal
import socket
import sys
import time


def window(spec):
    if not spec:
        return None
    t, d = spec.split(":")
    return float(t), float(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("in_port", type=int)
    ap.add_argument("out_port", type=int)
    ap.add_argument("--duration", type=float, required=True)
    ap.add_argument("--loss", type=float, default=0.0)
    ap.add_argument("--blackout", type=window)
    ap.add_argument("--jitter", type=float, default=0.0)
    ap.add_argument("--reorder", type=float, default=0.0)
    ap.add_argument("--stall", type=window)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    rx.bind(("127.0.0.1", a.in_port))
    rx.settimeout(0.001)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = ("127.0.0.1", a.out_port)

    stats = {"received": 0, "dropped": 0, "sent": 0, "reordered": 0}
    queue = []          # (send_time, seq, data)
    seq = 0
    t0 = None
    last_send = 0.0
    end = time.time() + a.duration

    def in_window(w, rel):
        return w is not None and w[0] <= rel < w[0] + w[1]

    def on_term(signum, frame):     # the test runner stops us with SIGTERM: still print the summary
        print(json.dumps(stats), flush=True)
        sys.exit(0)
    signal.signal(signal.SIGTERM, on_term)

    while time.time() < end:
        try:
            data = rx.recv(65536)
        except socket.timeout:
            data = None
        now = time.time()
        if data is not None:
            if t0 is None:
                t0 = now
            rel = now - t0
            stats["received"] += 1
            if in_window(a.blackout, rel) or (a.loss and rng.random() < a.loss):
                stats["dropped"] += 1
            else:
                send_at = now + (rng.uniform(0, a.jitter) / 1000.0 if a.jitter else 0.0)
                if a.reorder and rng.random() < a.reorder:
                    stats["reordered"] += 1
                else:
                    send_at = max(send_at, last_send)   # keep order
                    last_send = send_at
                if in_window(a.stall, rel):
                    send_at = max(send_at, t0 + a.stall[0] + a.stall[1])
                    last_send = max(last_send, send_at)
                heapq.heappush(queue, (send_at, seq, data))
                seq += 1
        while queue and queue[0][0] <= now:
            _, _, d = heapq.heappop(queue)
            tx.sendto(d, dst)
            stats["sent"] += 1
            if len(queue) > 50:          # releasing a burst: don't overflow the receiver's socket buffer
                time.sleep(0.0002)
    print(json.dumps(stats), flush=True)


if __name__ == "__main__":
    main()
