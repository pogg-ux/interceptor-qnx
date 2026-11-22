"""
plc_watch: read the PLC's status words over Modbus and print them.

OpenPLC maps %MW0.. to Modbus holding registers starting at 1024.
  %MW10 state, %MW11 fault, %MW12 cycles, %MW13 last cycle ms

Usage: python plc_watch.py [host] [port] [seconds]
License: MIT
"""
import sys
import time

from modbus_tcp import ModbusTCPClient

STATE_NAMES = {1: "PLC_OFF", 0: "IDLE", 10: "SAFETY_CHECK", 20: "WAIT_PUMP", 30: "HOMING",
               40: "READY", 50: "MANUAL", 110: "WAIT_PART", 120: "CLAMP_CLOSE",
               130: "PRESS_DOWN", 140: "DWELL", 150: "PRESS_UP", 160: "CLAMP_OPEN",
               170: "UNLOCK", 180: "INDEX", 190: "LOCK", 200: "EJECT_EXT",
               210: "EJECT_RET", 800: "PANIC", 900: "FAULT", 999: "GUARD_STOP"}


def read_status(c):
    st, flt, cyc, cms = c.read_holding(1024 + 10, 4)
    return {"state": st, "name": STATE_NAMES.get(st, "?"), "fault": flt,
            "cycles": cyc, "cycle_ms": cms}


if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 502
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
    c = None
    last = None
    end = time.time() + secs
    while time.time() < end:
        try:
            if c is None:
                c = ModbusTCPClient(host, port)
            s = read_status(c)
        except (OSError, ConnectionError):
            c = None
            time.sleep(0.5)
            continue
        if s != last:
            print(time.strftime("%H:%M:%S"), s, flush=True)
            last = s
        time.sleep(0.05)
