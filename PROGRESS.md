# Progress log

## Fri Nov 27, 2026 (one day: schedule items Nov 25 – Dec 5 done)

Everything below runs on QNX 8.0 in QEMU on the EVO X2 and was verified there.

| Schedule day | Item | Status |
|---|---|---|
| Wed Nov 25 | Hello-world built on host, run on QNX | ✅ |
| Fri Nov 27 | Interceptor v0: TCP passthrough PLC ↔ QNX ↔ simulator | ✅ machine cycles through QNX, 0 bad frames |
| Sat Nov 28 | Modbus copy-side parser, live I/O table | ✅ |
| Sun Nov 29 | Tap receiver, `ClockCycles()` timestamps, one event queue | ✅ |
| Mon Nov 30 | Resource manager `/dev/interceptor/*` | ✅ live, events, status, map, map.json, alerts, teach |
| Wed Dec 2 | Mapper: output → result → time chains | ✅ learns chains with no PLC program access |
| Thu Dec 3 | Teach Mode + teach script | ✅ 43 baselines, safety response, 75 labels; saved to VM disk, reloads at start |
| Fri Dec 4 | Drift rules F1–F4 | ✅ all caught in simulator tests; **F1 caught on the real setup** (491 ms vs 395 ms baseline) |
| Sat Dec 5 | F8 broken chain | ✅ (F5–F7 analog/count rules, black box, usage meters: still to do) |
| Tue Dec 1 | HMI proxy | ⏳ not started (first item on the cut list) |

### One-command start / stop
```
bash ~/interceptor-qnx/start.sh          # everything, machine cycling
bash ~/interceptor-qnx/start.sh teach    # run the Teach walkthrough first
bash ~/interceptor-qnx/stop.sh
```
The launcher repairs the VM network (stale `virbr0` after WSL restarts) and refuses to start a second VM.

### Views (from any WSL window)
`bash ~/interceptor-qnx/interceptor/live.sh [live|events|status|map|alerts|teach]`
or `ssh qnxuser@192.168.122.158 cat /dev/interceptor/<view>`

### Fault test commands (in `~/interceptor-qnx/sim`)
| Fault | Inject | Clear |
|---|---|---|
| F1 clamp drift | `python3 simctl.py fault F1_rate 0.03` | `fault F1_rate 0; fault F1_clamp_drift 0` |
| F2 welded pump contactor | `fault F2_m1_welded 1`, then `estop` | `fault F2_m1_welded 0`, `twist`, `poweron`, `press PB_RESET` |
| F3 lazy guard channel | `fault F3_guard_b_lag 0.6`, `set GUARD_A 0`, `set GUARD_B 0`, then both back to 1 | `fault F3_guard_b_lag 0`, `press PB_RESET` |
| F4 slow safety relay | `fault F4_sr1_slow 0.18`, then `estop` | `fault F4_sr1_slow 0`, recover as F2 |
| F8 dead eject | `fault F8_eject_dead 1` | `fault F8_eject_dead 0` |

### Found on the real PLC program (fixed)
- OpenPLC switches some outputs in the same scan (press down off + press up on). Broken-chain rule now only expects a shared result when its twin output also switched.
- A teach-time coincidence (Reset released as the heater relay came on) was learned as a chain. Operator controls are now never treated as results. **Re-teach once** (`start.sh teach`) to clear it from the saved map.

### Next
1. Re-teach with the latest code; test F2, F3, F4, F8 on the real setup.
2. F5–F7 (analog curve shape, encoder count, heater duty), black-box freeze, usage meters.
3. Live web view + desktop notifications (Dec 7).
4. README / build steps / demo script / video (Dec 8–12). QNX submit target: Sun Dec 13.
