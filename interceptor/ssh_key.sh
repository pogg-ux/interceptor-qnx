#!/bin/bash
# One time: let WSL log in to the QNX VM without a password (for deploy.sh and live.sh).
# Asks for the qnxuser password once.
QNX_IP="${QNX_IP:-192.168.122.158}"
[ -f ~/.ssh/id_ed25519 ] || ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519 -q
ssh-copy-id -i ~/.ssh/id_ed25519.pub qnxuser@"$QNX_IP" && \
ssh -o BatchMode=yes qnxuser@"$QNX_IP" 'echo "=== Key works: no more passwords ==="'
