#!/bin/bash
# Build the Interceptor for QNX and copy it to the QNX VM.
# Usage: bash deploy.sh            (asks for the qnxuser password once)
set -e
cd "$(dirname "$0")"
QNX_IP="${QNX_IP:-192.168.122.158}"
[ -n "$QNX_HOST" ] || source ~/qnx800/qnxsdp-env.sh >/dev/null
qcc -Vgcc_ntox86_64 -O2 -Wall interceptor.c modbus.c events.c taps.c views.c resmgr.c mapper.c -o interceptor -lsocket -lm
echo "Built. Copying to qnxuser@$QNX_IP ..."
# Write to a new name, then swap, so this works even while the old one is running.
cat interceptor | ssh qnxuser@"$QNX_IP" 'cat > interceptor.new && chmod +x interceptor.new && mv -f interceptor.new interceptor'
echo "=== Deployed. In the QNX window run:  /data/home/qnxuser/interceptor ==="
