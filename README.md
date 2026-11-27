# Interceptor (QNX)

> *"You'll never know it's there, but eventually, you'll know if it isn't."*

A **passthrough, read-and-log-only** retrofit that sits around a machine's PLC and watches the operator panel, the field I/O, and the old hardwired relay logic on **one real-time timeline**, running on **QNX 8.0**. It never drives an output or changes a value.

**Core idea: diagnosis by elimination.** The Interceptor proves what *isn't* the problem, so the technician finds what is, even when the fault is somewhere it can't see.

## The demo machine
The demo machine is a **simulated heated compression-molding press cell**: a fictional sample machine built for this project. It uses an architecture common across older industrial equipment: hydraulic clamping, an indexed feed, a main hydraulic drive, proportional valves driven through amplifiers by a separate motion controller, hardwired relay power-up logic, and dual-channel safety circuits.

The same Interceptor, with no code changes, applies to industrial saws, injection molding machines, hydraulic tube benders, press brakes, shears and punches, or any machine built this way. **Teach Mode** learns each machine from a physical walkthrough; it never needs the machine's program.

## What it does
| | |
|---|---|
| **Passthrough** | The PLC's field I/O traffic (Modbus TCP) flows *through* the Interceptor unchanged. A copy is parsed **after** each chunk is forwarded, so watching can never delay the machine. |
| **Taps** | Signals the PLC can't see: relay coils and contacts in the hardwired logic, both channels of every safety circuit, owner-added sensors. |
| **One timeline** | Every change from both sides, stamped with `ClockCycles()` the moment the Interceptor sees it. |
| **Mapper** | Learns *output → result → time* chains (e.g. clamp-close valve ON → open switch off +26 ms → closed switch ON +395 ms) **without reading the PLC program**. |
| **Teach Mode** | The tech walks the machine once (operate devices, run each motion, trip each safety). Baselines and labels are saved on QNX disk and reload at every start. |
| **Rules** | Plain, explainable checks, no machine learning: timing drift, coil/contact mismatch (welded contact), safety channel discrepancy, slow safety response, broken chain, analog curve shape. |
| **Black box** | Every alert freezes the last 60 s of the timeline and the I/O state to disk. |
| **Usage meters** | Pump hours, cycles, strokes, E-stops, counted by the Interceptor itself; they survive power loss, unlike the PLC's counters. |
| **Dashboard** | Read-only web page with desktop notifications for new alerts. |

### Faults it catches that the PLC doesn't
| Fault (simulator) | Interceptor alert |
|---|---|
| F1 sticking clamp valve | clamp close 491 ms vs 395 ms baseline, slow 3 times in a row |
| F2 welded pump contactor | coil off but main contact stays ON: welded contact |
| F3 lazy guard channel | guard channels A/B disagree for 115 ms |
| F4 slow safety relay | contacts opened in 216 ms vs 36 ms baseline |
| F5 pump wear | vibration, pump current, peak pressure, rise time out of band |
| F6 encoder miscount / F7 weak heater | counts per index / heater duty vs baseline |
| F8 dead eject solenoid | output ON but the extended switch never came: shows which link broke |
| F10 / F16 / F18 | coil current low / oil per stroke high (seal bypass) / filter ΔP rising |

## How it uses QNX 8.0 (where to look)
| QNX feature | File | Function(s) | What for |
|---|---|---|---|
| `ClockCycles()` + system page `cycles_per_sec` | `interceptor/hrclock.h` | `hr_now_ns()` | Nanosecond timestamps for every event on the timeline |
| **Resource manager** (`dispatch_create`, `resmgr_attach`, `iofunc_*`, `resmgr_msgread`) | `interceptor/resmgr.c` | `resmgr_start()`, `io_read()`, `io_write()` | `/dev/interceptor/{live,events,status,map,map.json,alerts,teach,meters}`: any QNX tool can `cat` them; Teach commands are written to `/dev/interceptor/teach` |
| Real-time priorities (`pthread_setschedparam`, `SCHED_RR`) | `interceptor/prio.h` | `prio_set()` | Passthrough 28 > resmgr 16 > black box 12 > web 8: readers can never delay the PLC's traffic |
| Threads (POSIX on QNX) | `resmgr.c`, `blackbox.c`, `httpd.c` | `resmgr_thread()`, `writer()`, `server()` | Dispatch loop, detached disk writer, web server, all off the passthrough path |
| Persistent storage on the QNX target | `teach.c`, `meters.c`, `blackbox.c` | `save()`, `meters_save()`, `blackbox_freeze()` | Taught map, meters and black-box files in `/data/home/qnxuser/` |
| QNX networking (io-sock, BSD sockets, `-lsocket`) | `interceptor/interceptor.c` | `main()` poll loop | TCP passthrough, UDP tap and command ports, HTTP |

Try it on the target:
```
cat /dev/interceptor/status
cat /dev/interceptor/map
echo start > /dev/interceptor/teach
```

## Build and run
**Tested with:** QNX SDP 8.0 (QNX Everywhere license) · target **x86_64 in QEMU** (`mkqnximage --type=qemu --arch=x86_64`) · toolchain `qcc -Vgcc_ntox86_64` · host WSL2 Ubuntu 24.04 on Windows 11.

1. One-time PC setup (WSL2, OpenPLC, simulator): [`setup/PC_SETUP.md`](setup/PC_SETUP.md). QNX SDP 8.0 installed at `~/qnx800`, a QNX QEMU image in `~/qnxvm`, libvirt's `default` network (`virbr0`, 192.168.122.0/24) allowed in `/etc/qemu/bridge.conf`.
2. Passwordless login to the VM (once): `bash interceptor/ssh_key.sh`
3. Start everything (boots QNX, builds + deploys the Interceptor, starts the virtual PLC and simulator, opens a 4-pane view):
   ```
   bash start.sh            # machine cycling
   bash start.sh teach      # run the Teach Mode walkthrough first (~2.5 min)
   bash stop.sh
   ```
4. Dashboard: **http://localhost:8090** in Chrome or Edge.
5. Inject faults: `cd sim && python3 simctl.py fault F1_rate 0.03` (full list in [`PROGRESS.md`](PROGRESS.md)).

Manual build only: `source ~/qnx800/qnxsdp-env.sh && cd interceptor && python3 gen_web.py && qcc -Vgcc_ntox86_64 -O2 -Wall *.c -o interceptor -lsocket -lm`, then copy to the target and run `./interceptor` as root (root is needed to create `/dev/interceptor`).

## Repository layout
| Folder | What |
|---|---|
| `interceptor/` | The Interceptor: C on QNX 8.0 (passthrough, Modbus parser, taps, event queue, mapper, rules, Teach Mode, resource manager, black box, meters, web dashboard) |
| `sim/` | Field simulator ("the machine"): Modbus TCP slave + passive tap stream; `simctl.py` controls it; `teach.py` plays the technician's Teach walkthrough. Python, standard library only |
| `plc/` | PLC program for the virtual PLC (IEC 61131-3 Structured Text) |
| `setup/` | Install and run scripts for WSL2 Ubuntu; `labels.txt` holds the Teach labels |
| `start.sh` / `stop.sh` | One-command start and stop |

## The simulated machine (rev 2)
- **Control power:** E-stop breaks a master control relay that powers the whole panel, PLC included. Power returns by cycling the Power/Start selector; the hydraulic pump is started by hand and seals itself in.
- **Two generations:** gen 1 twist-release E-stop; gen 2 spring-back E-stop plus a **Panic** button that cuts only hazardous-direction motion while the pump keeps running for safe-direction release.
- **Split control:** the PLC sequences; a separate motion controller owns the 0–10 V command to a proportional amplifier; Cycle Start is wired to both.
- **Hardwired relay logic** the PLC can't see, plus a guard safety relay with dual channels.
- **Owner-added sensors and flow meters** (pressures, filter ΔP, motor temp, vibration, flow, volume per stroke) streamed only to the Interceptor.
- **Injectable faults F1–F18**, most of them invisible to the PLC.

## Third-party software
- **OpenPLC Runtime v3** (https://github.com/thiagoralves/OpenPLC_v3) is used as the virtual PLC. It is downloaded and built by `setup/install_openplc.sh`; it is not included in this repository. OpenPLC is licensed under GPL-3.0.

## License
MIT. See [LICENSE](LICENSE).
