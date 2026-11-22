#!/bin/bash
# Start the GIPC simulator + OpenPLC, then show the PLC state live.
# Stop everything with Ctrl+C.
# Usage: bash run_all.sh        (gen 1 machine: twist-release E-stop)
#        bash run_all.sh 2      (gen 2 machine: spring-back E-stop + Panic button)
GEN="${1:-1}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OPLC="${OPENPLC_DIR:-$HOME/OpenPLC_v3}"
LOGS="$REPO/logs"; mkdir -p "$LOGS"

# OpenPLC serves Modbus on port 502, which needs admin rights.
sudo -v || exit 1

sudo pkill -f webserver.py 2>/dev/null; sudo pkill -f core/openplc 2>/dev/null
pkill -f gipc_sim.py 2>/dev/null
sleep 1

echo "Starting simulator (gen $GEN)..."
cd "$REPO/sim" && python3 gipc_sim.py --gen "$GEN" > "$LOGS/sim.log" 2>&1 &
SIM=$!
sleep 1
echo "Starting OpenPLC (web page: http://localhost:8080  login openplc / openplc)..."
cd "$OPLC" && sudo ./start_openplc.sh > "$LOGS/openplc.log" 2>&1 &
trap 'echo; echo Stopping...; kill $SIM 2>/dev/null; sudo pkill -f webserver.py; sudo pkill -f core/openplc; exit' INT TERM

printf "Waiting for the PLC on port 502"
for i in $(seq 1 60); do
  (echo > /dev/tcp/127.0.0.1/502) 2>/dev/null && break
  printf "."; sleep 1
done
echo
if ! (echo > /dev/tcp/127.0.0.1/502) 2>/dev/null; then
  echo "PLC did not open port 502 after 60 s. Last lines of the log:"
  tail -15 "$LOGS/openplc.log"
  kill $SIM 2>/dev/null; sudo pkill -f webserver.py; exit 1
fi

echo
echo "Ready. In a SECOND Ubuntu window:  cd ~/interceptor-qnx/sim"
echo "   python3 simctl.py press PB_RESET     (home the machine)"
echo "   python3 simctl.py press PB_START     (run auto cycles)"
echo "   python3 simctl.py estop              (E-stop: control power off, PLC goes dark)"
echo "   python3 simctl.py twist              (gen 1: release the E-stop)"
echo "   python3 simctl.py poweron            (selector OFF -> POWER -> START: power + pump)"
echo "   python3 tap_watch.py                 (see what the Interceptor will see)"
echo
cd "$REPO/sim" && python3 plc_watch.py 127.0.0.1 502
