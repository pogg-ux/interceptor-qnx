"""
simctl: send a command to the running GIPC simulator.

Examples:
  python simctl.py press PB_RESET
  python simctl.py press PB_START
  python simctl.py set ESTOP_A 0
  python simctl.py fault F1_rate 0.01
  python simctl.py status

License: MIT
"""
import socket
import sys


def send(cmd, host="127.0.0.1", port=6001, timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(cmd.encode(), (host, port))
    reply, _ = s.recvfrom(65535)
    s.close()
    return reply.decode()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    print(send(" ".join(sys.argv[1:])))
