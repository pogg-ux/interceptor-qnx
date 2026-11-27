"""
teach: plays the technician's Teach Mode walkthrough on the simulated machine.

  1. start Teach Mode on the Interceptor
  2. label every point (from setup/labels.txt)
  3. run the machine for N auto cycles (each motion 5+ times)
  4. trip the E-stop 3 times and open the guard once (safety baselines)
  5. finish: the Interceptor freezes its baselines and saves the map
  6. restart the machine in auto

The Interceptor only listens; this script presses the simulated buttons
(simctl) the way a tech would, and sends Teach commands to the Interceptor's
command port. Nothing here writes to the PLC.

Usage: python3 teach.py [--cycles 6] [--qnx 192.168.122.158] [--no-restart]
License: MIT
"""
import argparse
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--qnx", default=os.environ.get("QNX_IP", "192.168.122.158"))
ap.add_argument("--port", type=int, default=6002)
ap.add_argument("--cycles", type=int, default=6)
ap.add_argument("--cycle-s", type=float, default=10.5)
ap.add_argument("--no-restart", action="store_true")
args = ap.parse_args()

cmd_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def teach(line):
    cmd_sock.sendto(line.encode(), (args.qnx, args.port))
    time.sleep(0.02)


def sim(*a):
    subprocess.run([sys.executable, os.path.join(HERE, "simctl.py")] + [str(x) for x in a],
                   stdout=subprocess.DEVNULL, check=False)


def step(msg):
    print("\n==> " + msg, flush=True)


def wait(seconds, label):
    end = time.time() + seconds
    while time.time() < end:
        left = int(end - time.time())
        print("\r    %s ... %3ds " % (label, left), end="", flush=True)
        time.sleep(1)
    print("\r    %s ... done   " % label, flush=True)


step("1/6 Start Teach Mode")
teach("start")

step("2/6 Label every point")
n = 0
with open(os.path.join(HERE, "..", "setup", "labels.txt")) as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        point, _, text = line.partition(" ")
        teach("label %s %s" % (point, text.strip()))
        n += 1
print("    %d labels sent" % n)

step("3/6 Run %d auto cycles (each motion %d times)" % (args.cycles, args.cycles))
sim("press", "PB_RESET")
time.sleep(2)
sim("press", "PB_START")
wait(args.cycles * args.cycle_s + 2, "running")
sim("press", "PB_STOP")
wait(args.cycle_s + 1, "finishing the last cycle")

step("4/6 Trip each safety (E-stop x3, guard x1)")
for i in range(3):
    sim("estop")
    time.sleep(1.0)
    sim("twist")
    sim("poweron")
    time.sleep(3.0)
    sim("press", "PB_RESET")
    time.sleep(1.5)
    print("    E-stop trip %d done" % (i + 1), flush=True)
sim("set", "GUARD_A", 0)
sim("set", "GUARD_B", 0)
time.sleep(1.0)
sim("set", "GUARD_A", 1)
sim("set", "GUARD_B", 1)
time.sleep(1.0)
sim("press", "PB_RESET")
time.sleep(1.5)
print("    guard open/close done", flush=True)

step("5/6 Finish: Interceptor freezes baselines and saves the map")
teach("finish")
time.sleep(1)

if not args.no_restart:
    step("6/6 Restart the machine in auto")
    sim("press", "PB_RESET")
    time.sleep(2)
    sim("press", "PB_START")
print("\nTeach complete. See:  bash ~/interceptor-qnx/interceptor/live.sh teach")
