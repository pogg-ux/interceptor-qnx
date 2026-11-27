#!/bin/bash
# Build the Interceptor for QNX and copy it to the QNX VM.
# Usage: bash deploy.sh            (asks for the qnxuser password once)
set -e
cd "$(dirname "$0")"
QNX_IP="${QNX_IP:-192.168.122.158}"
[ -n "$QNX_HOST" ] || source ~/qnx800/qnxsdp-env.sh >/dev/null
qcc -Vgcc_ntox86_64 -O2 -Wall interceptor.c -o interceptor -lsocket
echo "Built. Copying to qnxuser@$QNX_IP ..."
cat interceptor | ssh qnxuser@"$QNX_IP" 'cat > interceptor && chmod +x interceptor'
echo "=== Deployed. In the QNX window run:  /data/home/qnxuser/interceptor ==="
