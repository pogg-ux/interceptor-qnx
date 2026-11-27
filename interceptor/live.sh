#!/bin/bash
# Show the Interceptor's live I/O table, refreshed twice a second. Ctrl+C to stop.
QNX_IP="${QNX_IP:-192.168.122.158}"
watch -n 0.5 -t ssh -o BatchMode=yes qnxuser@"$QNX_IP" cat /tmp/interceptor_live
