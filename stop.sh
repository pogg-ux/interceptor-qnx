#!/bin/bash
# Stop everything start.sh started: Interceptor, OpenPLC, simulator, QNX VM.
S=icpt
QNX_IP="${QNX_IP:-192.168.122.158}"
if ! tmux has-session -t $S 2>/dev/null; then
  echo "No launcher window open. Stopping any PLC / simulator left over..."
  pkill -f gipc_sim.py 2>/dev/null; sudo pkill -f webserver.py 2>/dev/null; sudo pkill -f core/openplc 2>/dev/null
  exit 0
fi
echo "Stopping live view, Interceptor, PLC + simulator..."
tmux send-keys -t $S:0.3 C-c
tmux send-keys -t $S:0.2 C-c
tmux send-keys -t $S:0.1 C-c
pkill -f gipc_sim.py 2>/dev/null
sudo pkill -f webserver.py 2>/dev/null; sudo pkill -f core/openplc 2>/dev/null
sleep 2
echo "Stopping the Interceptor and shutting down the QNX VM..."
tmux send-keys -t $S:0.0 C-c
sleep 1
tmux send-keys -t $S:0.0 C-a x
sleep 2
tmux kill-session -t $S 2>/dev/null
# Belt and braces: anything left over.
pkill -f gipc_sim.py 2>/dev/null
sudo pkill -f webserver.py 2>/dev/null; sudo pkill -f core/openplc 2>/dev/null
pkill -f qemu-system 2>/dev/null
echo "=== Everything stopped. Start again with: bash ~/interceptor-qnx/start.sh ==="
