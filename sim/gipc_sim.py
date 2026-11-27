"""
GIPC field simulator, revision 2: "the machine".

Plays the physical side of a fictional heated compression-molding press cell
(Generic Indexing Press Cell, GIPC). Everything here is generic: it is not
modeled on any real product.

What it simulates
  - Control power: E-stop breaks a master control relay (MCR) that powers the
    whole control panel, PLC included. Power is restored by cycling the
    Power/Start selector OFF -> POWER. The PLC sees its own power only through
    the emulation input SIM_PLC_POWERED.
  - Hydraulic pump started by hand: selector START (spring return) with all
    manual switches OFF; the pump contactor seals itself in through its aux.
  - Gen 1: twist-release (latched) E-stop.
    Gen 2: spring-back E-stop + PANIC button that cuts power to
    hazardous-direction outputs only; the pump keeps running.
  - Guard door safety relay (SR1), dual channel, monitored manual reset.
  - Split control: the PLC sequences; a separate motion controller ("stack")
    owns the 0-10 V command to a proportional amplifier; the amplifier
    drives the valve coil current. Cycle Start is wired to BOTH the PLC and
    the motion controller.
  - Hardwired relays CR2/CR3, index-motor interlock through the press-up
    limit's second contact.
  - Owner-added sensors and flow meters (Interceptor only, never the PLC).
  - Injectable faults F1-F18.

Interfaces
  - Modbus TCP slave for the PLC (default :5021)
  - Tap stream to the Interceptor, UDP JSON lines (default 127.0.0.1:6000):
      digital taps on change:  {"t": 12.345678, "tap": "T_CR1_COIL", "v": 1}
      all digital taps once a second (snapshot, same format, one packet)
      analog frame every 20 ms: {"t": 12.34, "a": {"AMP_CMD_V": 7.5, ...}}
  - Control commands on UDP :6001 (see simctl.py)

Standard library only. License: MIT
"""
import argparse
import asyncio
import json
import random
import socket
import time

from modbus_tcp import DataBank, ModbusTCPServer

TICK = 0.002            # 2 ms physics step
ANALOG_PERIOD = 0.020   # analog tap frame every 20 ms

# ---------------- PLC I/O maps (index into the Modbus tables) ----------------
DI = ["SR1_OK", "PB_START", "PB_STOP", "PB_RESET", "SS_AUTO", "SS_MANUAL",
      "PB_JOG_INDEX", "PB_SAFE_JOG",
      "PX_TABLE_HOME", "PX_PIN_LOCKED", "LS_CLAMP_OPEN", "LS_CLAMP_CLOSED",
      "LS_PRESS_UP", "LS_PRESS_DOWN", "LS_EJECT_RET", "LS_EJECT_EXT",
      "PE_PART_LOAD", "PE_PART_INFEED", "M1_AUX", "PS_AIR_OK", "TS_OIL_OK",
      "OL_M1_OK", "LUBE_LOW", "VFD_FAULT",
      # rev 2
      "SS_SEMI", "PANIC_ACTIVE", "SIM_PLC_POWERED", "SW_MAN_CLAMP",
      "SW_MAN_CONV", "MC_READY"]
COILS = ["Y_MC_ENABLE", "Y_PIN_RETRACT", "Y_CLAMP_CLOSE", "Y_CLAMP_OPEN",
         "Y_PRESS_DOWN", "Y_PRESS_UP", "Y_EJECT_EXT", "Y_CR2_INDEX",
         "LT_RUN", "LT_FAULT", "LT_READY", "HORN", "Y_LUBE", "Y_VFD_RUN",
         "Y_CR3_HEAT"]
IR = ["AI_HYD_PSI", "AI_RAM_POS", "AI_PLATEN_TEMP", "AI_PUMP_AMPS",
      "ENC_COUNT", "ENC_REVS", "VFD_HZ_ACT", "AI_OIL_TEMP"]
HR = ["AO_VFD_SPEED", "AO_PROP_VALVE_UNUSED", "AO_HEAT_DUTY"]

# Outputs cut by PANIC (hazardous directions). Safe directions stay live.
HAZARDOUS = {"Y_CLAMP_CLOSE", "Y_PRESS_DOWN", "Y_CR2_INDEX", "Y_VFD_RUN",
             "Y_EJECT_EXT"}

TAPS = ["T_ESTOP_A", "T_ESTOP_B", "T_SEL_POWER", "T_SEL_START", "T_ALL_OFF",
        "T_MCR_COIL", "T_MCR_CON", "T_CTRL_POWER",
        "T_GUARD_A", "T_GUARD_B", "T_SR1_K1", "T_SR1_K2",
        "T_M1_COIL", "T_M1_SEAL", "T_M1_CON", "T_PANIC", "T_PANIC_CUT",
        "T_CR2_COIL", "T_CR2_CON", "T_CR3_COIL", "T_CR3_CON",
        "T_M2_COIL", "T_SSR_OUT", "T_LS_PRESS_UP_2",
        "T_MC_START", "T_MC_MANUAL", "T_MC_ENABLE_IN", "T_MC_READY",
        "T_AMP_ENABLE", "T_AMP_FAULT"]

# Healthy machine timing (seconds)
NORMAL = {
    "clamp_close": 0.40, "clamp_open": 0.35,
    "press_down": 1.80, "press_up": 1.50,
    "psi_rise": 0.60,
    "pin_retract": 0.20, "pin_lock": 0.25,
    "index": 1.20,
    "eject_out": 0.50, "eject_back": 0.45,
    "sr1_drop": 0.015, "relay_pick": 0.012, "relay_drop": 0.010,
    "contactor_pick": 0.040, "mcr_drop": 0.020,
    "plc_boot": 2.0, "mc_boot": 1.5,
}
STROKE_MM = 200.0
STROKE_L = 1.2              # oil volume for one full press stroke (liters)
COUNTS_PER_REV = 4000
STATION = 1000

# Motion controller ("stack") function parameters: command volts per function
MC_PARAMS = {"press_down": 7.5, "press_hold": 6.0, "press_up": 6.0,
             "ramp_v_per_s": 40.0}
# Amplifier settings (as configured at commissioning)
AMP_DEFAULT = {"min_ma": 200.0, "max_ma": 1000.0, "ramp_s": 0.10}
AMP_FACTORY = {"min_ma": 100.0, "max_ma": 750.0, "ramp_s": 0.0}  # F14 lost settings


def noisy(t, pct=0.03):
    return t * (1.0 + random.uniform(-pct, pct))


class Delay:
    """Output follows input after a pick/drop delay (relay contacts, aux contacts)."""

    def __init__(self, pick, drop, state=False):
        self.pick, self.drop = pick, drop
        self.state = state
        self.target = state
        self.t_change = 0.0

    def update(self, want, now, pick=None, drop=None):
        if want != self.target:
            self.target = want
            delay = (pick if pick is not None else self.pick) if want else \
                    (drop if drop is not None else self.drop)
            self.t_change = now + noisy(delay)
        if self.state != self.target and now >= self.t_change:
            self.state = self.target
        return self.state


class Machine:
    def __init__(self, tap_addr, gen=1, preheat=True):
        self.gen = gen
        self.bank = DataBank(n_di=len(DI), n_coils=len(COILS), n_ir=len(IR), n_hr=len(HR))
        self.tap_addr = tap_addr
        self.tap_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.t0 = time.monotonic()
        self.taps = {name: None for name in TAPS}
        self.next_analog = 0.0
        self.next_snapshot = 0.0

        # Operator panel (what a person sets). Panel starts powered up and
        # homed so the first demo is quick; use "estop" + "poweron" for the
        # full power-up story.
        self.panel = {"PB_START": 0, "PB_STOP": 1, "PB_RESET": 0, "SS_AUTO": 1,
                      "SS_SEMI": 0, "SS_MANUAL": 0, "PB_JOG_INDEX": 0,
                      "PB_SAFE_JOG": 0, "SW_MAN_CLAMP": 0, "SW_MAN_CONV": 0,
                      "PS_AIR_OK": 1, "TS_OIL_OK": 1, "OL_M1_OK": 1,
                      "LUBE_LOW": 0, "VFD_FAULT": 0}
        self.momentary = {}            # name -> release time
        self.safety = {"ESTOP_A": 1, "ESTOP_B": 1, "GUARD_A": 1, "GUARD_B": 1}
        self.estop_latched = False     # gen 1 twist-release
        self.estop_release_at = None   # gen 2 spring-back
        self.selector = "POWER"        # OFF / POWER / START
        self.selector_return_at = None
        self.prev_selector = "POWER"
        self.panic = False             # gen 2 only
        self.parts_available = True

        # Power + controllers
        self.mcr_latched = True
        self.mcr = Delay(0.015, NORMAL["mcr_drop"], True)
        self.ctrl_power_since = -10.0
        self.ctrl_power_prev = True
        self.amp = dict(AMP_DEFAULT)
        self.amp_cmd_v = 0.0           # command at the amplifier input
        self.mc_cmd_v = 0.0            # motion controller output (before noise)
        self.coil_ma = 0.0
        self.pump_sealed = True

        # Physical state (0 = open/up/retracted, 1 = closed/down/extended)
        self.clamp = 0.0
        self.ram = 0.0
        self.eject = 0.0
        self.pin = 1.0
        self.counts = 0.0
        self.enc_lost = 0
        self.psi = 0.0
        self.platen = 178.0 if preheat else 25.0
        self.oil = 45.0
        self.motor_temp = 55.0
        self.vfd_hz = 0.0
        self.part_loaded = True
        self.ssr_phase = 0.0
        self.ssr_on = False
        self.flow_lpm = 0.0
        self.flow_total_l = 0.0
        self.stroke_vol_l = 0.0        # volume during the current down stroke
        self.last_stroke_vol_l = 0.0
        self.filter_dp = 8.0

        # Hybrid relay layer
        self.sr1_latched = True
        self.k1 = Delay(0.030, NORMAL["sr1_drop"], True)
        self.k2 = Delay(0.030, NORMAL["sr1_drop"], True)
        self.cr2 = Delay(NORMAL["relay_pick"], NORMAL["relay_drop"])
        self.cr3 = Delay(NORMAL["relay_pick"], NORMAL["relay_drop"])
        self.m1 = Delay(NORMAL["contactor_pick"], 0.030, True)
        self.m1_welded = False
        self.m1_aux = Delay(0.010, 0.010, True)
        self.m2 = Delay(NORMAL["contactor_pick"], 0.030)
        self.guard_b_seen = Delay(0.0, 0.0, True)

        # Injectable faults (all off = healthy machine)
        self.faults = {
            "F1_clamp_drift": 0.0,   # extra seconds on clamp close (grows by F1_rate/cycle)
            "F1_rate": 0.0,
            "F2_m1_welded": False,   # pump contactor main contact welded shut
            "F3_guard_b_lag": 0.0,   # seconds guard channel B lags A
            "F4_sr1_slow": 0.0,      # seconds added to K1/K2 drop
            "F5_pump_wear": 0.0,     # 0..1 severity
            "F6_enc_drop": 0,        # counts lost per index
            "F7_heater_weak": 0.0,   # 0..1 loss of heater power
            "F8_eject_dead": False,
            "F10_coil_open": 0.0,    # 0..1: fraction of coil current lost (high-resistance coil)
            "F11_amp_max_low": 0.0,  # 0..1: max current set this much too low
            "F12_amp_ramp_slow": 0.0,  # extra seconds of amp ramp
            "F13_cmd_noise": 0.0,    # volts of noise on the amp command (bad shield/ground)
            "F14_amp_forgets": False,  # amp loses settings on the next power cycle
            "F15_mc_start_open": False,  # motion controller's leg of Start wiring open
            "F16_seal_bypass": 0.0,  # 0..1 cylinder internal leakage
            "F17_seal_in_bad": False,  # pump seal-in contact fails
            "F18_filter_rate": 0.0,  # filter dP growth (psi per cycle)
        }
        self._eject_was_out = False
        self._index_started_at = None
        self._was_at_bottom = False

    # ---------------- helpers ----------------
    def now(self):
        return time.monotonic() - self.t0

    def coil(self, name):
        return bool(self.bank.coils[COILS.index(name)])

    def hreg(self, name):
        v = self.bank.hr[HR.index(name)]
        return v - 65536 if v >= 32768 else v

    def set_di(self, name, val):
        self.bank.di[DI.index(name)] = 1 if val else 0

    def set_ir(self, name, val):
        self.bank.ir[IR.index(name)] = int(max(0, min(65535, round(val))))

    def _send(self, obj):
        try:
            self.tap_sock.sendto(json.dumps(obj).encode() + b"\n", self.tap_addr)
        except OSError:
            pass

    def tap_snapshot(self):
        """Every tap's current state in one packet (like a tap input board's
        periodic scan), so a listener that starts late still knows every state."""
        t = round(self.now(), 6)
        lines = [json.dumps({"t": t, "tap": n, "v": v}) for n, v in self.taps.items() if v is not None]
        if lines:
            try:
                self.tap_sock.sendto(("\n".join(lines) + "\n").encode(), self.tap_addr)
            except OSError:
                pass

    def tap(self, name, val):
        val = 1 if val else 0
        if self.taps[name] != val:
            self.taps[name] = val
            self._send({"t": round(self.now(), 6), "tap": name, "v": val})

    @staticmethod
    def move(pos, target, seconds):
        step = TICK / max(seconds, 1e-3)
        if pos < target:
            return min(target, pos + step)
        return max(target, pos - step)

    def _station_error(self):
        r = self.counts % STATION
        return min(r, STATION - r)

    # ---------------- one physics step ----------------
    def step(self):
        now = self.now()
        f = self.faults

        # release momentary buttons / spring returns
        for name, t_rel in list(self.momentary.items()):
            if now >= t_rel:
                self.panel[name] = 1 if name == "PB_STOP" else 0
                del self.momentary[name]
        if self.selector == "START" and self.selector_return_at and now >= self.selector_return_at:
            self.selector = "POWER"
            self.selector_return_at = None
        if self.estop_release_at and now >= self.estop_release_at:   # gen 2 spring back
            self.safety["ESTOP_A"] = self.safety["ESTOP_B"] = 1
            self.estop_release_at = None

        # ---- control power: E-stop chain + master control relay ----
        est_a, est_b = self.safety["ESTOP_A"], self.safety["ESTOP_B"]
        estop_ok = bool(est_a and est_b)
        sel_power = self.selector in ("POWER", "START")
        if not estop_ok or not sel_power:
            self.mcr_latched = False
        elif self.prev_selector == "OFF" and sel_power:
            self.mcr_latched = True            # OFF -> POWER re-energizes the panel
        self.prev_selector = self.selector
        mcr_coil = self.mcr_latched and estop_ok and sel_power
        ctrl_power = self.mcr.update(mcr_coil, now)

        if ctrl_power and not self.ctrl_power_prev:
            self.ctrl_power_since = now
            if f["F14_amp_forgets"]:
                self.amp = dict(AMP_FACTORY)   # settings lost in the power cycle
        self.ctrl_power_prev = ctrl_power
        powered_for = now - self.ctrl_power_since if ctrl_power else -1.0
        plc_up = ctrl_power and powered_for >= NORMAL["plc_boot"]
        mc_up = ctrl_power and powered_for >= NORMAL["mc_boot"]

        # ---- guard safety relay SR1 (monitored by the PLC) ----
        grd_a = self.safety["GUARD_A"]
        grd_b = self.guard_b_seen.update(bool(self.safety["GUARD_B"]), now,
                                         pick=f["F3_guard_b_lag"], drop=f["F3_guard_b_lag"])
        if not (grd_a and grd_b) or not ctrl_power:
            self.sr1_latched = False
        elif not self.sr1_latched and self.panel["PB_RESET"]:
            self.sr1_latched = True
        drop = NORMAL["sr1_drop"] + f["F4_sr1_slow"]
        k1 = self.k1.update(self.sr1_latched, now, drop=drop)
        k2 = self.k2.update(self.sr1_latched, now, drop=drop)
        out_power = ctrl_power and k1 and k2          # power to solenoids/relays
        panic = self.panic and self.gen == 2

        def out(name):
            """Is this PLC output actually energized in the field?"""
            if not (out_power and plc_up and self.coil(name)):
                return False
            if panic and name in HAZARDOUS:
                return False
            return True

        # ---- hydraulic pump: hand start + seal-in (hardwired) ----
        all_off = not (self.panel["SW_MAN_CLAMP"] or self.panel["SW_MAN_CONV"])
        start_pos = self.selector == "START"
        seal = self.m1_aux.state and self.pump_sealed and not f["F17_seal_in_bad"]
        m1_coil = ctrl_power and bool(self.panel["OL_M1_OK"]) and ((start_pos and all_off) or seal)
        m1 = self.m1.update(m1_coil, now)
        # F2: once welded while closed, the main contact stays closed no matter what the coil does
        if f["F2_m1_welded"]:
            if m1:
                self.m1_welded = True
            if self.m1_welded:
                m1 = True
        else:
            self.m1_welded = False
        m1_aux = self.m1_aux.update(m1, now)
        self.pump_sealed = True
        pump = m1

        # ---- relays CR2 (index motor), CR3 (heater) ----
        press_up_2 = self.ram <= 0.01
        cr2_coil = out("Y_CR2_INDEX")
        cr2_con = self.cr2.update(cr2_coil, now)
        m2_coil = cr2_con and out_power and press_up_2
        m2 = self.m2.update(m2_coil, now)
        cr3_coil = out("Y_CR3_HEAT") and bool(self.panel["TS_OIL_OK"])
        cr3_con = self.cr3.update(cr3_coil, now)

        # ---- motion controller ("stack") ----
        mc_enable_in = out_power and plc_up and self.coil("Y_MC_ENABLE")
        mc_start = bool(self.panel["PB_START"]) and not f["F15_mc_start_open"]
        mc_manual = bool(self.panel["SS_MANUAL"])
        target_v = 0.0
        if mc_up and mc_enable_in and pump:
            down = out("Y_PRESS_DOWN") and not out("Y_PRESS_UP")
            up = out("Y_PRESS_UP") and not out("Y_PRESS_DOWN")
            if down and (not mc_manual or mc_start):   # manual: Start is its trigger
                target_v = MC_PARAMS["press_hold"] if self.ram >= 0.999 else MC_PARAMS["press_down"]
            elif up:
                target_v = MC_PARAMS["press_up"]
        step_v = MC_PARAMS["ramp_v_per_s"] * TICK
        self.mc_cmd_v += max(-step_v, min(step_v, target_v - self.mc_cmd_v))
        noise = random.uniform(-f["F13_cmd_noise"], f["F13_cmd_noise"]) if self.mc_cmd_v > 0.2 else 0.0
        self.amp_cmd_v = max(0.0, min(10.0, self.mc_cmd_v + noise))

        # ---- proportional amplifier -> coil current ----
        amp_enable = mc_up and mc_enable_in
        max_ma = self.amp["max_ma"] * (1.0 - f["F11_amp_max_low"])
        if amp_enable and self.amp_cmd_v > 0.2:
            want_ma = self.amp["min_ma"] + (max_ma - self.amp["min_ma"]) * self.amp_cmd_v / 10.0
        else:
            want_ma = 0.0
        want_ma *= (1.0 - f["F10_coil_open"])
        ramp_s = self.amp["ramp_s"] + f["F12_amp_ramp_slow"]
        if ramp_s <= 0:
            self.coil_ma = want_ma
        else:
            self.coil_ma += (want_ma - self.coil_ma) * min(1.0, TICK / ramp_s)
        amp_fault = amp_enable and f["F10_coil_open"] >= 0.5 and self.amp_cmd_v > 1.0
        # valve opening relative to a healthy amp at the nominal down command
        nominal_ma = AMP_DEFAULT["min_ma"] + (AMP_DEFAULT["max_ma"] - AMP_DEFAULT["min_ma"]) * MC_PARAMS["press_down"] / 10.0
        valve = max(0.0, (self.coil_ma - AMP_DEFAULT["min_ma"]) / (nominal_ma - AMP_DEFAULT["min_ma"]))

        # ---- pneumatics: clamp, pin, eject ----
        air = bool(self.panel["PS_AIR_OK"])
        if air:
            if out("Y_CLAMP_CLOSE") and not out("Y_CLAMP_OPEN"):
                self.clamp = self.move(self.clamp, 1.0, NORMAL["clamp_close"] + f["F1_clamp_drift"])
            elif out("Y_CLAMP_OPEN") and not out("Y_CLAMP_CLOSE"):
                self.clamp = self.move(self.clamp, 0.0, NORMAL["clamp_open"])
            if out("Y_EJECT_EXT") and not f["F8_eject_dead"]:
                self.eject = self.move(self.eject, 1.0, NORMAL["eject_out"])
            else:
                self.eject = self.move(self.eject, 0.0, NORMAL["eject_back"])
        at_station = self._station_error() <= 60
        if out("Y_PIN_RETRACT"):
            self.pin = self.move(self.pin, 0.0, NORMAL["pin_retract"])
        elif at_station and not m2:
            self.pin = self.move(self.pin, 1.0, NORMAL["pin_lock"])
            nearest = round(self.counts / STATION) * STATION
            self.counts += (nearest - self.counts) * min(1.0, TICK * 20.0)

        # ---- hydraulics: press ram, pressure, flow ----
        wear = f["F5_pump_wear"]
        bypass = f["F16_seal_bypass"]
        moving_down = moving_up = False
        flow = 0.0
        if pump and valve > 0.02:
            speed_scale = min(1.3, valve) / (1 + 0.3 * wear)
            if out("Y_PRESS_DOWN") and not out("Y_PRESS_UP") and self.ram < 1.0:
                new = self.move(self.ram, 1.0, NORMAL["press_down"] / max(speed_scale, 0.05))
                moving_down = new > self.ram
                flow = (new - self.ram) * STROKE_L / TICK * 60.0 * (1.0 + bypass)
                self.ram = new
            elif out("Y_PRESS_UP") and not out("Y_PRESS_DOWN") and self.ram > 0.0:
                new = self.move(self.ram, 0.0, NORMAL["press_up"] / max(speed_scale, 0.05))
                moving_up = new < self.ram
                flow = (self.ram - new) * STROKE_L * 0.6 / TICK * 60.0   # rod side
                self.ram = new
        if pump:
            if self.ram >= 0.999 and out("Y_PRESS_DOWN") and valve > 0.02:
                target = (300.0 + 2700.0 * min(valve, 1.2) * 0.62) * (1.0 - 0.15 * wear)
                rise = NORMAL["psi_rise"] * (1.0 + 1.5 * wear)
                flow += bypass * 12.0          # leakage flow while holding
            else:
                target, rise = 300.0, 0.3
            self.psi += (target - self.psi) * min(1.0, TICK * 3.0 / rise)
        else:
            self.psi += (0.0 - self.psi) * min(1.0, TICK * 5.0)
        self.flow_lpm += (flow - self.flow_lpm) * min(1.0, TICK * 25.0)
        self.flow_total_l += self.flow_lpm / 60.0 * TICK
        if out("Y_PRESS_DOWN"):
            self.stroke_vol_l += self.flow_lpm / 60.0 * TICK
        elif self.stroke_vol_l > 0.05:
            self.last_stroke_vol_l = self.stroke_vol_l
            self.stroke_vol_l = 0.0

        # ---- index table ----
        if m2 and self.pin <= 0.05:
            if self._index_started_at is None:
                self._index_started_at = self.counts
            self.counts += STATION * TICK / NORMAL["index"]
        elif self._index_started_at is not None and not m2:
            if f["F6_enc_drop"]:
                self.enc_lost += f["F6_enc_drop"]
            self._index_started_at = None
            self.part_loaded = False

        # ---- cycle bookkeeping (slow faults grow once per cycle) ----
        if self.eject >= 0.99:
            self._eject_was_out = True
        if self._eject_was_out and self.eject <= 0.01:
            self._eject_was_out = False
            f["F1_clamp_drift"] += f["F1_rate"]
            self.filter_dp += f["F18_filter_rate"]
        if (not self.part_loaded and self.parts_available and self.pin >= 0.99
                and self.eject <= 0.01):
            self.part_loaded = True

        # ---- heater (SSR time-proportioned, 1 s period) ----
        duty = max(0, min(1000, self.hreg("AO_HEAT_DUTY"))) / 1000.0 if cr3_con else 0.0
        self.ssr_phase = (self.ssr_phase + TICK) % 1.0
        self.ssr_on = self.ssr_phase < duty
        heat = (4.4 * (1.0 - f["F7_heater_weak"])) if self.ssr_on else 0.0
        self.platen += (heat - (self.platen - 25.0) * 0.01) * TICK

        # ---- conveyor drive ----
        ref = max(0, self.hreg("AO_VFD_SPEED")) / 32000.0 * 60.0
        target_hz = ref if out("Y_VFD_RUN") else 0.0
        if self.panel["SW_MAN_CONV"] and ctrl_power and not panic:
            target_hz = max(target_hz, 20.0)
        self.vfd_hz += (target_hz - self.vfd_hz) * min(1.0, TICK * 2.0)

        # ---- slow temperatures ----
        motor_target = (62.0 + 10.0 * wear) if pump else 30.0
        self.motor_temp += (motor_target - self.motor_temp) * TICK * 0.002

        # ---------------- inputs to the PLC ----------------
        if plc_up:
            for name in ("PB_START", "PB_STOP", "PB_RESET", "SS_AUTO", "SS_SEMI",
                         "SS_MANUAL", "PB_JOG_INDEX", "PB_SAFE_JOG", "SW_MAN_CLAMP",
                         "SW_MAN_CONV", "PS_AIR_OK", "TS_OIL_OK", "OL_M1_OK",
                         "LUBE_LOW", "VFD_FAULT"):
                self.set_di(name, self.panel[name])
            self.set_di("SR1_OK", k1 and k2)
            self.set_di("PX_TABLE_HOME", self._station_error() <= 40)
            self.set_di("PX_PIN_LOCKED", self.pin >= 0.95)
            self.set_di("LS_CLAMP_OPEN", self.clamp <= 0.02)
            self.set_di("LS_CLAMP_CLOSED", self.clamp >= 0.98)
            self.set_di("LS_PRESS_UP", self.ram <= 0.01)
            self.set_di("LS_PRESS_DOWN", self.ram >= 0.99)
            self.set_di("LS_EJECT_RET", self.eject <= 0.02)
            self.set_di("LS_EJECT_EXT", self.eject >= 0.98)
            self.set_di("PE_PART_LOAD", self.part_loaded)
            self.set_di("PE_PART_INFEED", self.parts_available)
            self.set_di("M1_AUX", m1_aux)
            self.set_di("PANIC_ACTIVE", panic)
            self.set_di("MC_READY", mc_up)
        else:
            for i in range(len(DI)):
                self.bank.di[i] = 0
        self.set_di("SIM_PLC_POWERED", plc_up)   # emulation of the PLC's own power

        self.set_ir("AI_HYD_PSI", 6400 + self.psi / 3000.0 * 25600)
        self.set_ir("AI_RAM_POS", self.ram * 32000)
        self.set_ir("AI_PLATEN_TEMP", self.platen * 10)
        amps = (18.0 + self.psi / 3000.0 * 14.0) * (1 + 0.25 * wear) if pump else 0.0
        self.set_ir("AI_PUMP_AMPS", 6400 + amps / 50.0 * 25600)
        enc = int(self.counts) - self.enc_lost
        self.set_ir("ENC_COUNT", enc % COUNTS_PER_REV)
        self.set_ir("ENC_REVS", (enc // COUNTS_PER_REV) % 65536)
        self.set_ir("VFD_HZ_ACT", self.vfd_hz * 10)
        self.set_ir("AI_OIL_TEMP", self.oil * 10)

        # ---------------- passive digital taps ----------------
        self.tap("T_ESTOP_A", est_a)
        self.tap("T_ESTOP_B", est_b)
        self.tap("T_SEL_POWER", sel_power)
        self.tap("T_SEL_START", start_pos)
        self.tap("T_ALL_OFF", all_off)
        self.tap("T_MCR_COIL", mcr_coil)
        self.tap("T_MCR_CON", ctrl_power)
        self.tap("T_CTRL_POWER", ctrl_power)
        self.tap("T_GUARD_A", grd_a)
        self.tap("T_GUARD_B", grd_b)
        self.tap("T_SR1_K1", k1)
        self.tap("T_SR1_K2", k2)
        self.tap("T_M1_COIL", m1_coil)
        self.tap("T_M1_SEAL", seal)
        self.tap("T_M1_CON", m1)
        self.tap("T_PANIC", panic)
        self.tap("T_PANIC_CUT", panic and out_power)
        self.tap("T_CR2_COIL", cr2_coil)
        self.tap("T_CR2_CON", cr2_con)
        self.tap("T_CR3_COIL", cr3_coil)
        self.tap("T_CR3_CON", cr3_con)
        self.tap("T_M2_COIL", m2_coil)
        self.tap("T_SSR_OUT", self.ssr_on)
        self.tap("T_LS_PRESS_UP_2", press_up_2)
        self.tap("T_MC_START", mc_start)
        self.tap("T_MC_MANUAL", mc_manual)
        self.tap("T_MC_ENABLE_IN", mc_enable_in)
        self.tap("T_MC_READY", mc_up)
        self.tap("T_AMP_ENABLE", amp_enable)
        self.tap("T_AMP_FAULT", amp_fault)

        if now >= self.next_snapshot:
            self.next_snapshot = now + 1.0
            self.tap_snapshot()

        # ---------------- analog taps + owner-added sensors ----------------
        if now >= self.next_analog:
            self.next_analog = now + ANALOG_PERIOD
            vib = 1.4 + 3.0 * wear + (0.6 if pump else -1.2) + random.uniform(-0.1, 0.1)
            self._send({"t": round(now, 4), "a": {
                "AMP_CMD_V": round(self.amp_cmd_v, 3),
                "COIL_MA": round(self.coil_ma, 1),
                "HYD_PSI": round(self.psi, 1),
                "PUMP_OUT_PSI": round(self.psi + 150.0 if pump else 0.0, 1),
                "RETURN_PSI": round((40.0 + 0.04 * self.psi) * (1.0 if pump else 0.0), 1),
                "FILTER_DP_PSI": round(self.filter_dp * (1.0 if pump else 0.0), 2),
                "MOTOR_TEMP_C": round(self.motor_temp, 2),
                "VIBRATION_MM_S": round(max(0.0, vib), 2),
                "AMBIENT_C": 24.0,
                "FLOW_PRESS_LPM": round(self.flow_lpm, 2),
                "FLOW_PRESS_TOTAL_L": round(self.flow_total_l, 3),
                "FLOW_COOLANT_LPM": round((8.0 + random.uniform(-0.2, 0.2)) if pump else 0.0, 2),
                "LAST_STROKE_L": round(self.last_stroke_vol_l, 3),
            }})

    # ---------------- control commands ----------------
    def command(self, line):
        """
        press NAME [seconds]   momentary pushbutton (default 0.3 s)
        set NAME 0|1           panel switch or raw safety channel
        selector OFF|POWER|START   (START springs back to POWER after 0.5 s)
        poweron                OFF -> POWER, then START for 0.6 s (pump)
        estop                  press the E-stop (gen 1 latches; gen 2 springs back)
        twist                  release a latched E-stop (gen 1)
        panic 0|1              gen 2 panic button
        fault NAME VALUE       set a fault parameter (see self.faults)
        parts 0|1              parts available on infeed
        status                 one-line status
        """
        parts = line.strip().split()
        if not parts:
            return "empty"
        cmd = parts[0].lower()
        now = self.now()
        if cmd == "press" and len(parts) >= 2:
            name = parts[1].upper()
            secs = float(parts[2]) if len(parts) > 2 else 0.3
            if name not in self.panel:
                return "unknown button " + name
            self.panel[name] = 0 if name == "PB_STOP" else 1
            self.momentary[name] = now + secs
            return "ok"
        if cmd == "set" and len(parts) == 3:
            name, val = parts[1].upper(), int(parts[2])
            if name in self.panel:
                self.panel[name] = val
            elif name in self.safety:
                self.safety[name] = val
            else:
                return "unknown " + name
            return "ok"
        if cmd == "selector" and len(parts) == 2:
            pos = parts[1].upper()
            if pos not in ("OFF", "POWER", "START"):
                return "selector must be OFF, POWER or START"
            self.selector = pos
            self.selector_return_at = now + 0.5 if pos == "START" else None
            return "ok"
        if cmd == "poweron":
            async def seq():
                self.selector = "OFF"
                await asyncio.sleep(0.3)
                self.selector = "POWER"
                await asyncio.sleep(2.5)           # let the PLC boot
                self.selector = "START"
                self.selector_return_at = self.now() + 0.6
            asyncio.get_event_loop().create_task(seq())
            return "ok (OFF -> POWER -> START)"
        if cmd == "estop":
            self.safety["ESTOP_A"] = self.safety["ESTOP_B"] = 0
            if self.gen == 2:
                self.estop_release_at = now + 0.4
            else:
                self.estop_latched = True
            return "ok"
        if cmd == "twist":
            self.safety["ESTOP_A"] = self.safety["ESTOP_B"] = 1
            self.estop_latched = False
            return "ok"
        if cmd == "panic" and len(parts) == 2:
            if self.gen != 2:
                return "panic button exists only on gen 2 (start sim with --gen 2)"
            self.panic = parts[1] == "1"
            return "ok"
        if cmd == "fault" and len(parts) == 3:
            name = parts[1]
            if name not in self.faults:
                return "unknown fault " + name + ". Known: " + ", ".join(self.faults)
            cur = self.faults[name]
            v = parts[2]
            if isinstance(cur, bool):
                self.faults[name] = v.lower() in ("1", "true", "on")
            else:
                self.faults[name] = type(cur)(float(v))
            if name == "F14_amp_forgets" and not self.faults[name]:
                self.amp = dict(AMP_DEFAULT)       # re-commissioned
            return "ok"
        if cmd == "parts" and len(parts) == 2:
            self.parts_available = parts[1] == "1"
            return "ok"
        if cmd == "status":
            return json.dumps({
                "t": round(now, 2), "gen": self.gen, "selector": self.selector,
                "ctrl_power": bool(self.taps.get("T_CTRL_POWER")),
                "pump": bool(self.taps.get("T_M1_COIL")), "panic": self.panic,
                "psi": round(self.psi), "ram": round(self.ram, 2), "clamp": round(self.clamp, 2),
                "amp_cmd_v": round(self.amp_cmd_v, 2), "coil_ma": round(self.coil_ma),
                "counts": int(self.counts), "platen": round(self.platen, 1),
                "last_stroke_l": round(self.last_stroke_vol_l, 3),
                "coils": {n: self.coil(n) for n in COILS},
                "faults": self.faults})
        return "bad command"


class ControlProtocol(asyncio.DatagramProtocol):
    def __init__(self, machine):
        self.m = machine

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        reply = self.m.command(data.decode(errors="replace"))
        self.transport.sendto(reply.encode(), addr)


async def physics_loop(m):
    next_t = time.monotonic()
    while True:
        m.step()
        next_t += TICK
        delay = next_t - time.monotonic()
        if delay > 0:
            await asyncio.sleep(delay)
        else:
            next_t = time.monotonic()  # fell behind; don't spiral


async def main():
    ap = argparse.ArgumentParser(description="GIPC field simulator (rev 2)")
    ap.add_argument("--modbus-port", type=int, default=5021)
    ap.add_argument("--tap-host", default="127.0.0.1")
    ap.add_argument("--tap-port", type=int, default=6000)
    ap.add_argument("--ctl-port", type=int, default=6001)
    ap.add_argument("--gen", type=int, choices=(1, 2), default=1,
                    help="1 = twist-release E-stop; 2 = spring-back E-stop + Panic button")
    ap.add_argument("--cold", action="store_true", help="start with a cold platen")
    args = ap.parse_args()

    m = Machine((args.tap_host, args.tap_port), gen=args.gen, preheat=not args.cold)
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: ControlProtocol(m),
                                        local_addr=("0.0.0.0", args.ctl_port))
    server = ModbusTCPServer(m.bank, port=args.modbus_port)
    print("GIPC simulator rev 2 (gen %d): Modbus :%d  taps -> %s:%d  control :%d"
          % (args.gen, args.modbus_port, args.tap_host, args.tap_port, args.ctl_port), flush=True)
    await asyncio.gather(server.serve(), physics_loop(m))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
