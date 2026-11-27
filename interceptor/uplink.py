"""
uplink: sends the Interceptor's alerts and usage meters to the Forge app
(Interceptor for Jira), which turns them into Jira work orders.

The Interceptor on QNX publishes its views read-only over HTTP (:8081). This
gateway, running in WSL, watches them and posts to the Forge web trigger over
HTTPS. In a real install it is the box's network uplink; it never sends
anything back to the machine.

Settings (never commit these): ~/.config/interceptor/uplink.env
    FORGE_URL=https://....webtrigger.atlassian.app/public/....
    INTERCEPTOR_TOKEN=<same value as: forge variables set --encrypt INTERCEPTOR_TOKEN ...>
    MACHINE=GIPC-1

Usage: python3 uplink.py [--send-existing] [--once-test]
License: MIT
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.request
from datetime import datetime, timezone

ap = argparse.ArgumentParser()
ap.add_argument("--qnx", default=os.environ.get("QNX_IP", "192.168.122.158"))
ap.add_argument("--qnx-port", type=int, default=8081)
ap.add_argument("--config", default=os.path.expanduser("~/.config/interceptor/uplink.env"))
ap.add_argument("--meters-every", type=float, default=900, help="seconds between meter snapshots")
ap.add_argument("--send-existing", action="store_true", help="also send alerts that were already there at start")
ap.add_argument("--once-test", action="store_true", help="send one test alert to check the connection, then exit")
args = ap.parse_args()

cfg = {}
if os.path.exists(args.config):
    for line in open(args.config):
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            cfg[k.strip()] = v.strip().strip('"')
FORGE_URL = os.environ.get("FORGE_URL", cfg.get("FORGE_URL", ""))
TOKEN = os.environ.get("INTERCEPTOR_TOKEN", cfg.get("INTERCEPTOR_TOKEN", ""))
MACHINE = os.environ.get("MACHINE", cfg.get("MACHINE", "GIPC-1"))
if not FORGE_URL or not TOKEN:
    sys.exit("uplink: set FORGE_URL and INTERCEPTOR_TOKEN in %s (see the top of this file)" % args.config)

BASE = "http://%s:%d/api/" % (args.qnx, args.qnx_port)
ALERT_RE = re.compile(r"^\s*([\d.]+)  (.{16}) (.*)$")


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def view(name):
    return urllib.request.urlopen(BASE + name, timeout=3).read().decode()


def post(msg):
    data = json.dumps(msg).encode()
    req = urllib.request.Request(FORGE_URL, data=data, method="POST", headers={
        "Content-Type": "application/json", "x-interceptor-token": TOKEN})
    with urllib.request.urlopen(req, timeout=15) as r:
        return r.status


def parse_alerts(text):
    lines = text.split("\n")
    m = re.search(r"(\d+) total", lines[0])
    total = int(m.group(1)) if m else 0
    items = []
    for l in lines[1:]:
        if not l.strip():
            break
        a = ALERT_RE.match(l)
        if a:
            items.append({"t": float(a.group(1)), "code": a.group(2).strip(), "text": a.group(3).strip()})
    return total, items


def timeline_before(t, events_text, n=40):
    out = []
    for l in events_text.split("\n")[1:]:
        m = re.match(r"^\s*([\d.]+)\s", l)
        if m and float(m.group(1)) <= t + 0.001:
            out.append(l.rstrip())
    return out[-n:]


def parse_meters(text):
    meters = {}
    for l in text.split("\n"):
        m = re.match(r"^\s+([a-z_]+)\s+([\d.]+)\s", l)
        if m:
            meters[m.group(1)] = float(m.group(2))
    return meters


queue = []          # messages waiting to be delivered (retried on failure)


def send_queue():
    while queue:
        msg = queue[0]
        try:
            status = post(msg)
        except Exception as e:
            print("  ! Forge not reachable (%s); will retry, %d waiting" % (e, len(queue)), flush=True)
            return
        print("  -> Forge %s %s %s" % (status, msg["kind"], msg.get("alert", {}).get("code", "")), flush=True)
        queue.pop(0)


if args.once_test:
    queue.append({"v": 1, "machine": MACHINE, "kind": "alert", "sent_at": now_iso(),
                  "alert": {"code": "TEST", "text": "Uplink connection test from the Interceptor gateway", "t": 0},
                  "timeline": []})
    send_queue()
    sys.exit(0 if not queue else 1)

print("uplink: %s -> Forge (machine %s)" % (BASE, MACHINE), flush=True)
last_total = None
last_meters = 0.0
while True:
    try:
        total, items = parse_alerts(view("alerts"))
        if last_total is None:
            last_total = 0 if args.send_existing else total
            print("  %d alert(s) already there; %s" % (total, "sending them" if args.send_existing else "watching for new ones"), flush=True)
        if total < last_total:                       # Interceptor restarted
            last_total = 0
        if total > last_total:
            new = items[-(total - last_total):] if total - last_total <= len(items) else items
            ev = view("events")
            for a in new:
                queue.append({"v": 1, "machine": MACHINE, "kind": "alert", "sent_at": now_iso(),
                              "alert": a, "timeline": timeline_before(a["t"], ev)})
                print("  alert %s: %s" % (a["code"], a["text"][:80]), flush=True)
            last_total = total
        if time.time() - last_meters >= args.meters_every:
            queue.append({"v": 1, "machine": MACHINE, "kind": "meters", "sent_at": now_iso(),
                          "meters": parse_meters(view("meters"))})
            last_meters = time.time()
    except Exception as e:
        print("  ! Interceptor not reachable (%s)" % e, flush=True)
    send_queue()
    time.sleep(2)
