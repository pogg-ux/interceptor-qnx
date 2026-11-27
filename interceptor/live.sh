#!/bin/bash
# Show the Interceptor's live view, refreshed twice a second. Ctrl+C to stop.
# Usage: bash live.sh          (I/O + taps table)
#        bash live.sh events   (event timeline, newest at the bottom)
QNX_IP="${QNX_IP:-192.168.122.158}"
GET="ssh -o BatchMode=yes qnxuser@$QNX_IP"
if [ "$1" = "events" ]; then
  watch -n 0.5 -t "$GET 'cat /tmp/interceptor_events 2>/dev/null || cat interceptor_events' | tail -n \$((\$(tput lines) - 1))"
else
  watch -n 0.5 -t "$GET 'cat /tmp/interceptor_live 2>/dev/null || cat interceptor_live'"
fi
