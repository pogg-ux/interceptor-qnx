#!/bin/bash
# Step 2 of 2: load the GIPC program into OpenPLC and point it at the simulator.
# Safe to run again any time the PLC program changes.  Usage: bash configure_gipc.sh
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OPLC="${OPENPLC_DIR:-$HOME/OpenPLC_v3}"
WEB="$OPLC/webserver"

echo "[1/4] Copying PLC program"
cp "$REPO/plc/gipc_main.st" "$WEB/st_files/gipc_main.st"

echo "[2/4] Compiling PLC program"
cd "$WEB"
./scripts/compile_program.sh gipc_main.st | tail -1

echo "[3/4] Setting up slave device, 20 ms polling, auto-run"
FIELD_HOST="${FIELD_HOST:-127.0.0.1}" FIELD_PORT="${FIELD_PORT:-5021}" "$OPLC/.venv/bin/python3" - <<'PY'
import sqlite3, time
db = sqlite3.connect("openplc.db")
c = db.cursor()
c.execute("DELETE FROM Slave_dev WHERE dev_name='GIPC_FIELD'")
# Default: PLC talks to the simulator directly (127.0.0.1:5021).
# Through the Interceptor on QNX: FIELD_HOST=192.168.122.158 FIELD_PORT=5020 bash configure_gipc.sh
import os
host = os.environ.get("FIELD_HOST", "127.0.0.1")
port = int(os.environ.get("FIELD_PORT", "5021"))
c.execute("""INSERT INTO Slave_dev (dev_name,dev_type,slave_id,ip_address,ip_port,
  di_start,di_size,coil_start,coil_size,ir_start,ir_size,
  hr_read_start,hr_read_size,hr_write_start,hr_write_size,pause)
  VALUES ('GIPC_FIELD','TCP',1,?,?,0,30,0,15,0,8,0,0,0,3,0)""", (host, port))
c.execute("UPDATE Settings SET Value='20' WHERE Key='Slave_polling'")
c.execute("UPDATE Settings SET Value='true' WHERE Key='Start_run_mode'")
if not c.execute("SELECT 1 FROM Programs WHERE File='gipc_main.st'").fetchone():
    c.execute("INSERT INTO Programs (Name,Description,File,Date_upload) VALUES (?,?,?,?)",
              ("GIPC main", "Generic Indexing Press Cell", "gipc_main.st", int(time.time())))
db.commit()
print("    field I/O ->", host + ":" + str(port))
PY

echo "[4/4] Writing mbconfig.cfg"
"$OPLC/.venv/bin/python3" -c "import webserver; webserver.generate_mbconfig()" >/dev/null 2>&1
grep -q GIPC_FIELD mbconfig.cfg && echo "    ok"
echo
echo "=== Done. Start everything with: bash run_all.sh ==="
