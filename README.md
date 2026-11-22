# Interceptor (QNX)

> *"You'll never know it's there, but eventually, you'll know if it isn't."*

A **passthrough, read-and-log-only** retrofit that sits around a machine's PLC and watches the operator panel, the field I/O, and the old hardwired relay logic on **one real-time timeline**, running on **QNX 8.0**. It never drives an output or changes a value.

**Core idea: diagnosis by elimination.** The Interceptor proves what *isn't* the problem, so the technician finds what is, even when the fault is somewhere it can't see.

## The demo machine
The demo machine is a **simulated heated compression-molding press cell**: a fictional sample machine built for this project. It uses an architecture common across older industrial equipment: hydraulic clamping, an indexed feed, a main hydraulic drive, proportional valves driven through amplifiers by a separate motion controller, hardwired relay power-up logic, and dual-channel safety circuits.

The same Interceptor, with no code changes, applies to industrial saws, injection molding machines, hydraulic tube benders, press brakes, shears and punches, or any machine built this way. **Teach Mode** learns each machine from a physical walkthrough; it never needs the machine's program.

## Repository layout
| Folder | What |
|---|---|
| `sim/` | Field simulator ("the machine"): Modbus TCP slave + passive tap stream; `simctl.py` controls it, `tap_watch.py` shows the taps. Python, standard library only |
| `plc/` | PLC program for the virtual PLC (IEC 61131-3 Structured Text) |
| `setup/` | Install and run scripts for WSL2 Ubuntu. Start with [`setup/PC_SETUP.md`](setup/PC_SETUP.md) |
| `interceptor/` | The Interceptor itself, C on QNX 8.0 *(in progress)* |

## The simulated machine (rev 2)
- **Control power:** E-stop breaks a master control relay that powers the whole panel, PLC included. Power returns by cycling the Power/Start selector; the hydraulic pump is started by hand and seals itself in.
- **Two generations:** gen 1 twist-release E-stop; gen 2 spring-back E-stop plus a **Panic** button that cuts only hazardous-direction motion while the pump keeps running for safe-direction release.
- **Split control:** the PLC sequences; a separate motion controller owns the 0–10 V command to a proportional amplifier; Cycle Start is wired to both.
- **Hardwired relay logic** the PLC can't see, plus a guard safety relay with dual channels.
- **Owner-added sensors and flow meters** (pressures, filter ΔP, motor temp, vibration, flow, volume per stroke) streamed only to the Interceptor.
- **18 injectable faults**, most of them invisible to the PLC (see `setup/PC_SETUP.md`).

## Status
- ✅ Simulated machine rev 2 + virtual PLC running (auto, semi-auto, manual, E-stop power loss, Panic, 18 faults)
- ✅ Passive tap stream (digital on change + analog every 20 ms): `sim/tap_watch.py`
- 🔄 Interceptor on QNX 8.0 (QEMU target)

## Third-party software
- **OpenPLC Runtime v3** (https://github.com/thiagoralves/OpenPLC_v3) is used as the virtual PLC. It is downloaded and built by `setup/install_openplc.sh`; it is not included in this repository. OpenPLC is licensed under GPL-3.0.

## License
MIT. See [LICENSE](LICENSE).
