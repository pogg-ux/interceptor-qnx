#!/bin/bash
# =====================================================================
#  Interceptor master launcher.  One command starts everything:
#    QNX VM -> deploy + start Interceptor -> OpenPLC + simulator
#    -> machine reset + start -> live views.
#  Usage:  bash ~/interceptor-qnx/start.sh          (machine starts cycling)
#          bash ~/interceptor-qnx/start.sh idle     (machine left at READY)
#          bash ~/interceptor-qnx/start.sh teach    (run the Teach Mode walkthrough
#                                                    first, ~2.5 min, then cycle)
#  Stop everything:  bash ~/interceptor-qnx/stop.sh
#
#  Screen layout (one window, 4 panes; click a pane to use it):
#    +---------------------+---------------------+
#    | QNX console (root): | Event timeline      |
#    |  Interceptor runs   |  /dev/interceptor/  |
#    |  here, logs events  |       events        |
#    +---------------------+---------------------+
#    | PLC state           | Live I/O table      |
#    |                     |  /dev/interceptor/  |
#    |                     |       live          |
#    +---------------------+---------------------+
# =====================================================================
set -e
REPO="$(cd "$(dirname "$0")" && pwd)"
QNX_IP="${QNX_IP:-192.168.122.158}"
S=icpt
MODE="${1:-run}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=3 qnxuser@$QNX_IP"

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

command -v tmux >/dev/null || { say "Installing tmux (one time)"; sudo apt-get install -y tmux; }

if tmux has-session -t $S 2>/dev/null; then
  say "Already running. Opening it. (To restart: bash $REPO/stop.sh first)"
  exec tmux attach -t $S
fi

if pgrep -f qemu-system >/dev/null; then
  say "A QNX VM is already running outside the launcher (two VMs fight over one address)."
  say "Run:  bash $REPO/stop.sh; pkill -f qemu-system   then start.sh again."
  exit 1
fi

say "Asking for your Ubuntu password now (OpenPLC needs it for port 502)"
sudo -v

# Make sure OpenPLC points at the Interceptor, not straight at the simulator.
MB="$HOME/OpenPLC_v3/webserver/mbconfig.cfg"
if ! grep -q "$QNX_IP" "$MB" 2>/dev/null; then
  say "Pointing OpenPLC at the Interceptor (one time, ~1 min)"
  (cd "$REPO/setup" && FIELD_HOST=$QNX_IP FIELD_PORT=5020 bash configure_gipc.sh)
fi

say "1/5 Booting QNX VM"
tmux new-session -d -s $S -x 220 -y 55 -n interceptor
tmux set -t $S mouse on
tmux set-option -w -t $S:0 pane-border-status top
tmux set -t $S status-right ' stop: bash ~/interceptor-qnx/stop.sh '
tmux split-window -h -t $S:0.0
tmux split-window -v -t $S:0.0
tmux split-window -v -t $S:0.2
# panes: 0 = top-left, 1 = bottom-left, 2 = top-right, 3 = bottom-right
tmux select-pane -t $S:0.0 -T "QNX console (root) - Interceptor"
tmux select-pane -t $S:0.1 -T "PLC state"
tmux select-pane -t $S:0.2 -T "Event timeline  /dev/interceptor/events"
tmux select-pane -t $S:0.3 -T "Live I/O  /dev/interceptor/live"
tmux send-keys -t $S:0.0 "source ~/qnx800/qnxsdp-env.sh >/dev/null && cd ~/qnxvm && mkqnximage --type=qemu --arch=x86_64 --run" Enter

printf "    waiting for QNX to answer"
for i in $(seq 1 90); do
  $SSH true 2>/dev/null && break
  printf "."; sleep 2
done
echo
if ! $SSH true 2>/dev/null; then
  say "Diagnostics:"
  ping -c 1 -W 1 $QNX_IP >/dev/null 2>&1 && echo "    ping $QNX_IP: OK" || echo "    ping $QNX_IP: NO ANSWER (VM still booting, or it got a different address)"
  echo "    ssh says: $($SSH true 2>&1 | tail -1)"
  say "QNX did not answer over ssh. If it asks for a password, run: bash $REPO/interceptor/ssh_key.sh"
  say "Then: bash $REPO/stop.sh  and run start.sh again."
  exit 1
fi

say "2/5 Building + copying the Interceptor to QNX"
bash "$REPO/interceptor/deploy.sh" | tail -1

say "3/5 Starting the Interceptor on QNX (as root, so it can create /dev/interceptor)"
tmux send-keys -t $S:0.0 "" Enter
sleep 1
tmux send-keys -t $S:0.0 "/data/home/qnxuser/interceptor" Enter
sleep 2

say "4/5 Starting OpenPLC + simulator (taps go to QNX)"
OPLC="${OPENPLC_DIR:-$HOME/OpenPLC_v3}"
LOGS="$REPO/logs"; mkdir -p "$LOGS"
sudo pkill -f webserver.py 2>/dev/null || true; sudo pkill -f core/openplc 2>/dev/null || true
pkill -f gipc_sim.py 2>/dev/null || true
sleep 1
(cd "$REPO/sim" && nohup python3 gipc_sim.py --gen 1 --tap-host "$QNX_IP" > "$LOGS/sim.log" 2>&1 &)
sleep 1
(cd "$OPLC" && sudo nohup ./start_openplc.sh > "$LOGS/openplc.log" 2>&1 &)
printf "    waiting for the PLC"
for i in $(seq 1 90); do
  (echo > /dev/tcp/127.0.0.1/502) 2>/dev/null && break
  printf "."; sleep 2
done
echo
if ! (echo > /dev/tcp/127.0.0.1/502) 2>/dev/null; then
  say "PLC did not start. Last lines of $LOGS/openplc.log:"; tail -15 "$LOGS/openplc.log"; exit 1
fi
if [ "$MODE" = "teach" ]; then
  tmux send-keys -t $S:0.1 "cd $REPO/sim && python3 teach.py && python3 plc_watch.py 127.0.0.1 502" Enter
else
  tmux send-keys -t $S:0.1 "cd $REPO/sim && python3 plc_watch.py 127.0.0.1 502" Enter
fi
sleep 3   # let the PLC connect through the Interceptor

if [ "$MODE" = "teach" ]; then
  say "5/5 Teach Mode walkthrough running in the bottom-left pane (~2.5 min)"
elif [ "$MODE" != "idle" ]; then
  say "5/5 Reset + start the machine"
  (cd "$REPO/sim" && python3 simctl.py press PB_RESET >/dev/null && sleep 2 && python3 simctl.py press PB_START >/dev/null) || true
else
  say "5/5 Machine left idle (press PB_RESET / PB_START with simctl when ready)"
fi

tmux send-keys -t $S:0.2 "bash $REPO/interceptor/live.sh events" Enter
tmux send-keys -t $S:0.3 "bash $REPO/interceptor/live.sh" Enter
tmux select-pane -t $S:0.0
say "Ready. Opening the 4-pane view. Click a pane to use it. Stop everything: bash ~/interceptor-qnx/stop.sh"
sleep 1
exec tmux attach -t $S
