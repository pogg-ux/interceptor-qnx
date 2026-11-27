#!/bin/bash
# Show one of the Interceptor's views, refreshed twice a second. Ctrl+C to stop.
# Usage: bash live.sh          (I/O + taps table)    /dev/interceptor/live
#        bash live.sh events   (event timeline)      /dev/interceptor/events
#        bash live.sh status   (link health)         /dev/interceptor/status
#        bash live.sh map      (learned chains)      /dev/interceptor/map
QNX_IP="${QNX_IP:-192.168.122.158}"
V="${1:-live}"
GET="ssh -o BatchMode=yes qnxuser@$QNX_IP 'cat /dev/interceptor/$V 2>/dev/null || cat /tmp/interceptor_$V 2>/dev/null || cat interceptor_$V'"
if [ "$V" = "events" ]; then
  watch -n 0.5 -t "$GET | tail -n \$((\$(tput lines) - 1))"
else
  watch -n 0.5 -t "$GET"
fi
