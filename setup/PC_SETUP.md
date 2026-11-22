# PC Setup: Virtual PLC on Windows (WSL2)

**Total time: about 25 minutes**, mostly waiting. You do steps 1–2 once. After that it's only step 5.

---

## Step 1: Turn on WSL2 + Ubuntu (needs admin, maybe a reboot)

1. Click **Start**, type `PowerShell`, right-click it and choose **Run as administrator**.
2. Type this and press Enter:
   ```
   wsl --install -d Ubuntu-24.04
   ```
3. If it asks you to **reboot**, reboot.
4. After the reboot an **Ubuntu** window opens by itself. If it doesn't, open **Ubuntu** from the Start menu.
5. It asks for a **username** and **password**. Pick something simple. You'll type this password whenever a command says `sudo`.

> If you see an error about **virtualization**, it has to be turned on in the BIOS (AMD "SVM Mode"). Tell me and I'll walk you through it.

---

## Step 2: Put the project files in Ubuntu

1. Download **interceptor-qnx.zip** from our chat.
2. Right-click it → **Extract All** → extract to **`C:\`**, so you end up with **`C:\interceptor-qnx`**.
3. In the **Ubuntu** window, type:
   ```
   cp -r /mnt/c/interceptor-qnx ~/
   cd ~/interceptor-qnx/setup
   ```

---

## Step 3: Install OpenPLC (one time, about 10 minutes)

```
bash install_openplc.sh
```
Type your Ubuntu password when asked. Lots of text scrolls by. It's done when you see:
**`=== OpenPLC installed. Next: bash configure_gipc.sh ===`**

---

## Step 4: Load our machine program into the PLC

```
bash configure_gipc.sh
```
It's done when you see **`Compilation finished successfully!`** and **`=== Done ===`**.

---

## Step 5: Run it

> **Opening Ubuntu:** open PowerShell (no admin needed) and type `wsl -d Ubuntu-24.04`.
> If you have more than one Linux install, make this one the default by running once in PowerShell: `wsl --set-default Ubuntu-24.04`

**Ubuntu window #1:**
```
cd ~/interceptor-qnx/setup
bash run_all.sh
```
It asks for your Ubuntu password (OpenPLC needs admin rights for port 502), shows `Waiting for the PLC on port 502...`, then the PLC state, starting at `IDLE`.

**Ubuntu window #2** (new PowerShell → `wsl -d Ubuntu-24.04`):
```
cd ~/interceptor-qnx/sim
python3 simctl.py press PB_RESET
python3 simctl.py press PB_START
```

Window #1 should now show the machine cycling: CLAMP_CLOSE → PRESS_DOWN → DWELL → PRESS_UP → CLAMP_OPEN → UNLOCK → INDEX → LOCK → EJECT_EXT → EJECT_RET, about **10 seconds per cycle**.

**To stop:** press **Ctrl+C** in window #1.

---

## Things to try (window #2)

| Type this | What happens |
|---|---|
| `python3 simctl.py set ESTOP_A 0` then `set ESTOP_B 0` | E-stop: PLC goes to ESTOP |
| `python3 simctl.py set ESTOP_A 1` then `set ESTOP_B 1` then `press PB_RESET` | Recovers, re-homes, waits for Start |
| `python3 simctl.py press PB_START` | Runs auto again |
| `python3 simctl.py fault F8_eject_dead 1` | Eject cylinder dies → PLC fault 300 after 5 s |
| `python3 simctl.py fault F8_eject_dead 0` then `press PB_RESET` | Clears it |
| `python3 simctl.py status` | Shows the machine's physical state |

**OpenPLC's own web page:** open **http://localhost:8080** in your Windows browser. Login **openplc / openplc**. Under **Monitoring** you can watch every I/O point live.

---

## What's running

| Program | Where | Port |
|---|---|---|
| Field simulator ("the machine") | `~/interceptor-qnx/sim/gipc_sim.py` | Modbus 5021, taps UDP 6000, control UDP 6001 |
| OpenPLC (the virtual PLC) | `~/OpenPLC_v3` | Modbus 502, web page 8080 |
| PLC program | `~/interceptor-qnx/plc/gipc_main.st` | (none) |

Logs go to `~/interceptor-qnx/logs/`.


---

