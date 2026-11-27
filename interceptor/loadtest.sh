#!/bin/bash
# Passthrough-under-load test. While the machine runs, hammer every reader
# of the Interceptor (the /dev files over ssh and the web dashboard) and
# check the PLC's traffic still flows at the same speed.
# Usage: bash loadtest.sh [seconds]      (start.sh must be running)
QNX_IP="${QNX_IP:-192.168.122.158}"
SECS="${1:-60}"
SSH="ssh -o BatchMode=yes qnxuser@$QNX_IP"
status() { $SSH cat /dev/interceptor/status; }
echo "== Before:"; status | grep -E "forwarding|<10us|modbus frames"
B=$(status | sed -n 's/.*modbus frames *\([0-9]*\) requests.*/\1/p')
echo "== Loading for $SECS s: 4 ssh readers looping over /dev/interceptor/*, 8 web clients at full speed"
END=$((SECONDS + SECS))
for i in 1 2 3 4; do
  # -tt: the remote loop gets a terminal, so it ends when we hang up
  ssh -tt -o BatchMode=yes qnxuser@$QNX_IP "while :; do for v in live events status map alerts meters teach map.json; do cat /dev/interceptor/\$v >/dev/null; done; done" >/dev/null 2>&1 &
done
for i in $(seq 1 8); do
  ( while [ $SECONDS -lt $END ]; do for v in live events status alerts; do curl -s -o /dev/null http://localhost:8090/api/$v; done; done ) &
done
sleep "$SECS"
kill $(jobs -p) 2>/dev/null; wait 2>/dev/null
A=$(status | sed -n 's/.*modbus frames *\([0-9]*\) requests.*/\1/p')
echo "== After:"; status | grep -E "forwarding|<10us|modbus frames"
echo "== PLC polls during the test: $((A - B)) in ${SECS}s = $(( (A - B) / SECS )) per second (normal is about 150)"
