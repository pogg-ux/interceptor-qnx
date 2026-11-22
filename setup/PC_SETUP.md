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
(`bash run_all.sh 2` starts the gen 2 machine with the Panic button.)
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

## Things to try (window #2, in `~/interceptor-qnx/sim`)

**Running the machine**
| Type this | What happens |
|---|---|
| `python3 simctl.py press PB_RESET` | Machine homes, goes to READY |
| `python3 simctl.py press PB_START` | Auto cycles (~10 s each) |
| `python3 simctl.py press PB_STOP` | Finishes the current cycle, then READY |
| `python3 simctl.py set SS_AUTO 0` then `set SS_SEMI 1` | Semi-auto: one cycle per Start |

**Power and safety**
| Type this | What happens |
|---|---|
| `python3 simctl.py estop` | Control power cut: PLC shows `PLC_OFF`, pump stops |
| `python3 simctl.py twist` | Gen 1 only: releases the latched E-stop |
| `python3 simctl.py poweron` | Selector OFF → POWER → START: panel power + pump start, then `press PB_RESET` |
| `python3 simctl.py set GUARD_A 0` | Guard opens: `GUARD_STOP`. Set back to 1, then `press PB_RESET` |
| `python3 simctl.py panic 1` | Gen 2 only (`bash run_all.sh 2`): hazardous directions cut, pump keeps running |
| `python3 simctl.py press PB_SAFE_JOG 4` | During Panic: press up + clamp open (safe directions) |
| `python3 simctl.py panic 0` then `press PB_RESET` | Clears Panic |

**See what the Interceptor will see:** `python3 tap_watch.py` (Ctrl+C to stop)

**Faults** (`python3 simctl.py fault NAME VALUE`; set back to 0 to clear)
| Fault | Example | What it does |
|---|---|---|
| F1 clamp drift | `fault F1_rate 0.01` | Clamp gets 10 ms slower every cycle |
| F3 lazy guard channel | `fault F3_guard_b_lag 0.6` | Guard channel B lags A by 0.6 s |
| F4 slow safety relay | `fault F4_sr1_slow 0.18` | Safety contacts open 180 ms late |
| F5 pump wear | `fault F5_pump_wear 0.5` | Slower, weaker, more current, more vibration |
| F6 encoder miscount | `fault F6_enc_drop 15` | Encoder loses 15 counts per index |
| F7 weak heater | `fault F7_heater_weak 0.4` | Heater needs more duty to hold temperature |
| F8 dead eject | `fault F8_eject_dead 1` | PLC faults 300 after 5 s |
| F10 coil going open | `fault F10_coil_open 0.3` | Coil current low for the same command |
| F11 amp max set wrong | `fault F11_amp_max_low 0.35` | Press slower, never reaches full force |
| F12 amp ramp wrong | `fault F12_amp_ramp_slow 0.8` | Sluggish start to every motion |
| F13 noisy command | `fault F13_cmd_noise 1.5` | Coil current jitters (bad shield/ground) |
| F14 amp forgets settings | `fault F14_amp_forgets 1`, then `estop` + `poweron` | Machine runs weaker after a power cycle |
| F15 split Start wiring | `fault F15_mc_start_open 1` (Manual mode) | PLC sees Start, motion controller doesn't |
| F16 cylinder seal bypass | `fault F16_seal_bypass 0.3` | Same stroke time, ~45% more oil per stroke |
| F17 seal-in failure | `fault F17_seal_in_bad 1`, then `estop` + `poweron` | Pump drops when the selector springs back |
| F18 filter clogging | `fault F18_filter_rate 0.5` | Filter pressure drop climbs every cycle |

**OpenPLC's own web page:** **http://localhost:8080** in your Windows browser (login **openplc / openplc**) → **Monitoring**.

---

## Updating to a new version
```
cd ~
mv interceptor-qnx interceptor-qnx-old
git clone https://github.com/pogg-ux/interceptor-qnx
cd ~/interceptor-qnx/setup
bash configure_gipc.sh
bash run_all.sh
```
(`configure_gipc.sh` must be re-run whenever the PLC program or I/O list changes.)

---

## What's running

| Program | Where | Port |
|---|---|---|
| Field simulator ("the machine") | `~/interceptor-qnx/sim/gipc_sim.py` | Modbus 5021, taps UDP 6000, control UDP 6001 |
| Tap viewer | `~/interceptor-qnx/sim/tap_watch.py` | listens on UDP 6000 |
| OpenPLC (the virtual PLC) | `~/OpenPLC_v3` | Modbus 502, web page 8080 |
| PLC program | `~/interceptor-qnx/plc/gipc_main.st` | (none) |

Logs go to `~/interceptor-qnx/logs/`.


---

