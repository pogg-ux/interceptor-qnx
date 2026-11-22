"""
tap_watch: print the passive tap stream the Interceptor will receive.

Digital taps print on every change. Analog frames are summarized once per
second (use --all to print every 20 ms frame).

Usage: python tap_watch.py [--port 6000] [--all] [--digital-only]
License: MIT
"""
import argparse
import json
import socket
import time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=6000)
ap.add_argument("--all", action="store_true")
ap.add_argument("--digital-only", action="store_true")
args = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", args.port))
last_analog = 0.0
while True:
    data, _ = s.recvfrom(65535)
    for line in data.decode().splitlines():
        msg = json.loads(line)
        if "tap" in msg:
            print("%10.4f  %-16s %d" % (msg["t"], msg["tap"], msg["v"]), flush=True)
        elif not args.digital_only and (args.all or time.time() - last_analog >= 1.0):
            last_analog = time.time()
            a = msg["a"]
            print("%10.4f  ANALOG  " % msg["t"] + "  ".join("%s=%s" % kv for kv in a.items()), flush=True)
