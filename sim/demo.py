"""
demo: the repeatable demo run for the video. Needs start.sh running and a
taught map (start.sh teach once).

  0. Normal running: the Interceptor just watches
  1. F1 sticking clamp valve  -> timing drift alert (PLC sees nothing)
  2. F3 lazy guard channel    -> safety discrepancy alert
  3. F2 welded pump contactor -> E-stop pressed, pump keeps running: mismatch alert
  4. F8 dead eject solenoid   -> broken chain alert, shows which link broke
Each fault is cleared and the machine recovered before the next.

Usage: python3 demo.py [--only 1,3] [--web http://localhost:8090]
License: MIT
"""
import argparse
import os
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--web", default="http://localhost:8090")
ap.add_argument("--only", default="")
ap.add_argument("--normal-s", type=float, default=20)
args = ap.parse_args()
only = {int(x) for x in args.only.split(",") if x.strip()}

BOLD, RED, GRN, DIM, END = "\033[1m", "\033[91m", "\033[92m", "\033[2m", "\033[0m"


def sim(*a):
    subprocess.run([sys.executable, os.path.join(HERE, "simctl.py")] + [str(x) for x in a],
                   stdout=subprocess.DEVNULL, check=False)


def alerts():
    try:
        t = urllib.request.urlopen(args.web + "/api/alerts", timeout=2).read().decode()
    except OSError:
        return 0, []
    lines = t.split("\n")
    n = int(lines[0].split(":")[1].split()[0]) if ":" in lines[0] else 0
    items = []
    for l in lines[1:]:
        if not l.strip():
            break
        if not l.strip().startswith("("):
            items.append(l.strip())
    return n, items


def caption(n, title, text):
    print("\n" + BOLD + "=" * 72 + END)
    print(BOLD + "  %s  %s" % (n, title) + END)
    for line in text.strip().split("\n"):
        print("  " + line.strip())
    print(BOLD + "=" * 72 + END, flush=True)


def wait_alert(before, timeout, want):
    t0 = time.time()
    while time.time() - t0 < timeout:
        n, items = alerts()
        new = [i for i in items[-(n - before):]] if n > before else []
        hits = [i for i in new if want in i]
        if hits:
            print("\n  " + RED + BOLD + "INTERCEPTOR ALERT" + END + "  (%.1f s after the fault)" % (time.time() - t0))
            for h in hits[:2]:
                print("  " + RED + h + END)
            return True
        print("\r  " + DIM + "watching... %4.0f s" % (time.time() - t0) + END, end="", flush=True)
        time.sleep(0.5)
    print("\n  (no alert within %d s)" % timeout)
    return False


def recover(after_estop=False):
    if after_estop:
        sim("twist"); sim("poweron"); time.sleep(3.0)
    sim("press", "PB_RESET"); time.sleep(2.5)
    sim("press", "PB_START")


def step(k):
    return not only or k in only


if step(0):
    caption("0", "NORMAL RUNNING", """
    The machine runs its cycle. The PLC's I/O traffic passes through the
    Interceptor on QNX untouched; it watches, timestamps and compares every
    motion with what Teach Mode learned.""")
    sim("press", "PB_RESET"); time.sleep(2); sim("press", "PB_START")
    time.sleep(args.normal_s)

if step(1):
    caption("1", "STICKING CLAMP VALVE  (F1)", """
    The clamp valve starts to stick: each cycle the clamp closes a little slower.
    The PLC only faults if the clamp takes more than 5 seconds.""")
    n0, _ = alerts()
    sim("fault", "F1_rate", 0.06)
    wait_alert(n0, 90, "F1")
    sim("fault", "F1_rate", 0); sim("fault", "F1_clamp_drift", 0)
    print("  " + GRN + "cleared: valve replaced" + END)
    time.sleep(5)

if step(2):
    caption("2", "LAZY SAFETY CHANNEL  (F3)", """
    Guard switch channel B has gone lazy. The safety relay still trips on
    channel A, so the PLC sees a normal guard stop. The hidden fault: one
    channel is no longer protecting anyone.""")
    n0, _ = alerts()
    sim("fault", "F3_guard_b_lag", 0.6)
    sim("set", "GUARD_A", 0); sim("set", "GUARD_B", 0)
    wait_alert(n0, 10, "F3")
    time.sleep(1)
    sim("set", "GUARD_A", 1); sim("set", "GUARD_B", 1); time.sleep(1)
    sim("fault", "F3_guard_b_lag", 0)
    print("  " + GRN + "cleared: guard switch replaced" + END)
    recover()
    time.sleep(8)

if step(3):
    caption("3", "WELDED PUMP CONTACTOR  (F2)", """
    The pump contactor's main contact has welded shut. Someone hits the E-stop:
    the coil drops, but the pump keeps running. The PLC's aux contact looks normal.""")
    n0, _ = alerts()
    sim("fault", "F2_m1_welded", 1); time.sleep(1)
    sim("estop")
    wait_alert(n0, 10, "F2")
    time.sleep(2)
    sim("fault", "F2_m1_welded", 0)
    print("  " + GRN + "cleared: contactor replaced" + END)
    recover(after_estop=True)
    time.sleep(10)

if step(4):
    caption("4", "DEAD EJECT SOLENOID  (F8)", """
    The eject solenoid coil dies. The PLC turns the output on and, 5 seconds
    later, faults with a generic timeout. The Interceptor shows exactly where
    the chain broke: output ON, but the cylinder never left home.""")
    n0, _ = alerts()
    sim("fault", "F8_eject_dead", 1)
    wait_alert(n0, 40, "F8")
    sim("fault", "F8_eject_dead", 0)
    print("  " + GRN + "cleared: solenoid replaced" + END)
    recover()

print("\n" + BOLD + "Demo complete." + END + " Every alert above froze a black-box file on the QNX target.")
