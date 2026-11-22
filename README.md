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
| `sim/` | Field simulator ("the machine"): Modbus TCP slave + passive tap stream. Python, standard library only |
| `plc/` | PLC program for the virtual PLC (IEC 61131-3 Structured Text) |
| `setup/` | Install and run scripts for WSL2 Ubuntu. Start with [`setup/PC_SETUP.md`](setup/PC_SETUP.md) |
| `interceptor/` | The Interceptor itself, C on QNX 8.0 *(in progress)* |

## Status
- ✅ Simulated machine + virtual PLC running (full auto cycles, E-stop recovery, injected faults)
- 🔄 Interceptor on QNX 8.0 (QEMU target)

## Third-party software
- **OpenPLC Runtime v3** (https://github.com/thiagoralves/OpenPLC_v3) is used as the virtual PLC. It is downloaded and built by `setup/install_openplc.sh`; it is not included in this repository. OpenPLC is licensed under GPL-3.0.

## License
MIT. See [LICENSE](LICENSE).
