#!/bin/bash
# Step 1 of 2: download and build OpenPLC Runtime v3 inside WSL2 Ubuntu.
# Takes about 10 minutes. Run once.  Usage: bash install_openplc.sh
set -e
cd "$HOME"
if [ ! -d OpenPLC_v3 ]; then
  git clone --depth 1 https://github.com/thiagoralves/OpenPLC_v3.git
fi
cd OpenPLC_v3
./install.sh linux
# We start OpenPLC ourselves with run_all.sh, so turn off the auto-start service.
sudo systemctl disable --now openplc 2>/dev/null || true
echo
echo "=== OpenPLC installed. Next: bash configure_gipc.sh ==="
