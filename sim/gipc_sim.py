"""
GIPC field simulator: "the machine".

Plays the physical side of the fictional Generic Indexing Press Cell:
switches, limit switches, hydraulics, pneumatics, heater, encoder,
the hybrid relay layer (SR1 safety relay, CR1/CR2/CR3 ice cube relays,
M1/M2 contactors), and injectable faults.

  - Serves field I/O to the PLC as a Modbus TCP slave (default :5021)
  - Streams passive tap changes as UDP JSON lines (default 127.0.0.1:6000)
  - Accepts control commands on UDP :6001 (see simctl.py)

Standard library only. License: MIT
"""
import argparse
import asyncio
import json
import random
import socket
import time

from modbus_tcp import DataBank, ModbusTCPServer

TICK = 0.002  # 2 ms physics step

# ---------------- I/O maps (index into the Modbus tables) ----------------
DI = ["SR1_OK", "PB_START", "PB_STOP", "PB_RESET", "SS_AUTO", "SS_MANUAL",
      "PB_JOG_INDEX", "PB_JOG_PRESS",
      "PX_TABLE_HOME", "PX_PIN_LOCKED", "LS_CLAMP_OPEN", "LS_CLAMP_CLOSED",
      "LS_PRESS_UP", "LS_PRESS_DOWN", "LS_EJECT_RET", "LS_EJECT_EXT",
      "PE_PART_LOAD", "PE_PART_INFEED", "M1_AUX", "PS_AIR_OK", "TS_OIL_OK",
      "OL_M1_OK", "LUBE_LOW", "VFD_FAULT"]
COILS = ["Y_CR1_PUMP", "Y_PIN_RETRACT", "Y_CLAMP_CLOSE", "Y_CLAMP_OPEN",
         "Y_PRESS_DOWN", "Y_PRESS_UP", "Y_EJECT_EXT", "Y_CR2_INDEX",
         "LT_RUN", "LT_FAULT", "LT_READY", "HORN", "Y_LUBE", "Y_VFD_RUN",
         "Y_CR3_HEAT"]
IR = ["AI_HYD_PSI", "AI_RAM_POS", "AI_PLATEN_TEMP", "AI_PUMP_AMPS",
      "ENC_COUNT", "ENC_REVS", "VFD_HZ_ACT", "AI_OIL_TEMP"]
HR = ["AO_VFD_SPEED", "AO_PROP_VALVE", "AO_HEAT_DUTY"]

TAPS = ["T_ESTOP_A", "T_ESTOP_B", "T_GUARD_A", "T_GUARD_B", "T_SR1_K1",
        "T_SR1_K2", "T_CR1_COIL", "T_CR1_CON", "T_CR2_COIL", "T_CR2_CON",
        "T_CR3_COIL", "T_CR3_CON", "T_M1_COIL", "T_M2_COIL", "T_SSR_OUT",
        "T_LS_PRESS_UP_2"]

# Healthy machine timing (seconds)
NORMAL = {
    "clamp_close": 0.40, "clamp_open": 0.35,
    "press_down": 1.80, "press_up": 1.50,
    "psi_rise": 0.60,
    "pin_retract": 0.20, "pin_lock": 0.25,
    "index": 1.20,
    "eject_out": 0.50, "eject_back": 0.45,
    "sr1_drop": 0.015, "relay_pick": 0.012, "relay_drop": 0.010,
    "contactor_pick": 0.040,
}
STROKE_MM = 200.0
COUNTS_PER_REV = 4000
STATION = 1000


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
    def __init__(self, tap_addr, preheat=True):
        self.bank = DataBank(n_di=len(DI), n_coils=len(COILS), n_ir=len(IR), n_hr=len(HR))
        self.tap_addr = tap_addr
        self.tap_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.t0 = time.monotonic()
        self.taps = {name: None for name in TAPS}

        # Operator panel (what a person would set)
        self.panel = {"PB_START": 0, "PB_STOP": 1, "PB_RESET": 0, "SS_AUTO": 1,
                      "SS_MANUAL": 0, "PB_JOG_INDEX": 0, "PB_JOG_PRESS": 0,
                      "PS_AIR_OK": 1, "TS_OIL_OK": 1, "OL_M1_OK": 1,
                      "LUBE_LOW": 0, "VFD_FAULT": 0}
        self.momentary = {}  # name -> release time
        self.safety = {"ESTOP_A": 1, "ESTOP_B": 1, "GUARD_A": 1, "GUARD_B": 1}
        self.parts_available = True

        # Physical state (0 = open/up/retracted, 1 = closed/down/extended)
        self.clamp = 0.0
        self.ram = 0.0            # 0..1 of stroke
        self.eject = 0.0
        self.pin = 1.0            # 1 = locked (spring)
        self.counts = 0.0         # continuous table position (counts)
        self.enc_lost = 0         # counts the encoder failed to see (F6)
        self.psi = 0.0
        self.platen = 178.0 if preheat else 25.0
        self.oil = 45.0
        self.vfd_hz = 0.0
        self.part_loaded = True
        self.ssr_phase = 0.0
        self.ssr_on = False

        # Hybrid relay layer
        self.sr1_latched = True
        self.k1 = Delay(0.030, NORMAL["sr1_drop"], True)
        self.k2 = Delay(0.030, NORMAL["sr1_drop"], True)
        self.cr1 = Delay(NORMAL["relay_pick"], NORMAL["relay_drop"])
        self.cr2 = Delay(NORMAL["relay_pick"], NORMAL["relay_drop"])
        self.cr3 = Delay(NORMAL["relay_pick"], NORMAL["relay_drop"])
        self.m1 = Delay(NORMAL["contactor_pick"], 0.030)
        self.m1_aux = Delay(0.010, 0.010)
        self.m2 = Delay(NORMAL["contactor_pick"], 0.030)
        self.guard_b_seen = Delay(0.0, 0.0, True)

        # Injectable faults (all off = healthy machine)
        self.faults = {
            "F1_clamp_drift": 0.0,   # extra seconds added to clamp close (grows)
            "F1_rate": 0.0,          # seconds added per cycle
            "F2_cr1_welded": False,
            "F3_guard_b_lag": 0.0,   # seconds channel B lags A
            "F4_sr1_slow": 0.0,      # seconds added to K1/K2 drop
            "F5_pump_wear": 0.0,     # 0..1 severity
            "F6_enc_drop": 0,        # counts lost per index
            "F7_heater_weak": 0.0,   # 0..1 loss of heater power
            "F8_eject_dead": False,
        }
        self._eject_was_out = False
        self._index_started_at = None

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

    def tap(self, name, val):
        val = 1 if val else 0
        if self.taps[name] != val:
            self.taps[name] = val
            msg = json.dumps({"t": round(self.now(), 6), "tap": name, "v": val})
            try:
                self.tap_sock.sendto(msg.encode() + b"\n", self.tap_addr)
            except OSError:
                pass

    @staticmethod
    def move(pos, target, seconds):
        step = TICK / max(seconds, 1e-3)
        if pos < target:
            return min(target, pos + step)
        return max(target, pos - step)

    # ---------------- one physics step ----------------
    def step(self):
        now = self.now()
        f = self.faults

        # release momentary buttons
        for name, t_rel in list(self.momentary.items()):
            if now >= t_rel:
                self.panel[name] = 0 if name != "PB_STOP" else 1
                del self.momentary[name]

        # ---- safety circuit (SR1) ----
        est_a, est_b = self.safety["ESTOP_A"], self.safety["ESTOP_B"]
        grd_a = self.safety["GUARD_A"]
        # F3: channel B follows channel A late
        grd_b_raw = self.safety["GUARD_B"]
        grd_b = self.guard_b_seen.update(bool(grd_b_raw), now,
                                         pick=f["F3_guard_b_lag"], drop=f["F3_guard_b_lag"])
        chains_ok = est_a and est_b and grd_a and grd_b
        if not chains_ok:
            self.sr1_latched = False
        elif not self.sr1_latched and self.panel["PB_RESET"]:
            self.sr1_latched = True  # monitored manual reset
        drop = NORMAL["sr1_drop"] + f["F4_sr1_slow"]
        k1 = self.k1.update(self.sr1_latched, now, drop=drop)
        k2 = self.k2.update(self.sr1_latched, now, drop=drop)
        power = k1 and k2  # output power through K1/K2

        # ---- relays and contactors ----
        cr1_coil = self.coil("Y_CR1_PUMP") and power
        cr1_con = self.cr1.update(cr1_coil, now)
        if f["F2_cr1_welded"] and self.taps.get("T_CR1_CON"):
            cr1_con = True  # contact welded shut
            self.cr1.state = True
        m1_coil = cr1_con and power and bool(self.panel["OL_M1_OK"])
        m1 = self.m1.update(m1_coil, now)
        m1_aux = self.m1_aux.update(m1, now)
        pump = m1

        press_up_2 = self.ram <= 0.01  # 2nd contact on press-up limit (hardwired)
        cr2_coil = self.coil("Y_CR2_INDEX") and power
        cr2_con = self.cr2.update(cr2_coil, now)
        m2_coil = cr2_con and power and press_up_2
        m2 = self.m2.update(m2_coil, now)

        cr3_coil = self.coil("Y_CR3_HEAT") and power and bool(self.panel["TS_OIL_OK"])
        cr3_con = self.cr3.update(cr3_coil, now)

        # ---- pneumatics: clamp, pin, eject (need air + power) ----
        air = bool(self.panel["PS_AIR_OK"])
        if air and power:
            if self.coil("Y_CLAMP_CLOSE") and not self.coil("Y_CLAMP_OPEN"):
                t = NORMAL["clamp_close"] + f["F1_clamp_drift"]
                self.clamp = self.move(self.clamp, 1.0, t)
            elif self.coil("Y_CLAMP_OPEN") and not self.coil("Y_CLAMP_CLOSE"):
                self.clamp = self.move(self.clamp, 0.0, NORMAL["clamp_open"])
            if self.coil("Y_EJECT_EXT") and not f["F8_eject_dead"]:
                self.eject = self.move(self.eject, 1.0, NORMAL["eject_out"])
            else:
                self.eject = self.move(self.eject, 0.0, NORMAL["eject_back"])
        # pin: solenoid retracts, spring locks (only drops in at a station)
        # the tapered pin pulls the table onto the station as it drops in
        at_station = self._station_error() <= 60
        if power and self.coil("Y_PIN_RETRACT"):
            self.pin = self.move(self.pin, 0.0, NORMAL["pin_retract"])
        elif at_station and not m2:
            self.pin = self.move(self.pin, 1.0, NORMAL["pin_lock"])
            nearest = round(self.counts / STATION) * STATION
            self.counts += (nearest - self.counts) * min(1.0, TICK * 20.0)

        # ---- hydraulics: press ram + pressure ----
        wear = f["F5_pump_wear"]
        if pump:
            if self.coil("Y_PRESS_DOWN") and not self.coil("Y_PRESS_UP"):
                self.ram = self.move(self.ram, 1.0, NORMAL["press_down"] * (1 + 0.3 * wear))
            elif self.coil("Y_PRESS_UP") and not self.coil("Y_PRESS_DOWN"):
                self.ram = self.move(self.ram, 0.0, NORMAL["press_up"] * (1 + 0.3 * wear))
            prop = max(0, self.hreg("AO_PROP_VALVE")) / 32000.0
            if self.ram >= 0.999 and self.coil("Y_PRESS_DOWN"):
                target = 3000.0 * prop * (1.0 - 0.15 * wear)
                rise = NORMAL["psi_rise"] * (1.0 + 1.5 * wear)
            else:
                target, rise = 300.0, 0.3
            self.psi += (target - self.psi) * min(1.0, TICK * 3.0 / rise)
        else:
            self.psi += (0.0 - self.psi) * min(1.0, TICK * 5.0)

        # ---- index table ----
        if m2 and self.pin <= 0.05:
            if self._index_started_at is None:
                self._index_started_at = self.counts
            self.counts += STATION * TICK / NORMAL["index"]
        elif self._index_started_at is not None and not m2:
            if f["F6_enc_drop"]:
                self.enc_lost += f["F6_enc_drop"]  # encoder missed counts
            self._index_started_at = None
            self.part_loaded = False  # finished part moved away

        # ---- eject cycle bookkeeping (F1 grows each cycle) ----
        if self.eject >= 0.99:
            self._eject_was_out = True
        if self._eject_was_out and self.eject <= 0.01:
            self._eject_was_out = False
            f["F1_clamp_drift"] += f["F1_rate"]

        # new part arrives at load once the table is locked and eject is back
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
        target_hz = ref if self.coil("Y_VFD_RUN") else 0.0
        self.vfd_hz += (target_hz - self.vfd_hz) * min(1.0, TICK * 2.0)

        # ---------------- write inputs for the PLC ----------------
        for name in ("PB_START", "PB_STOP", "PB_RESET", "SS_AUTO", "SS_MANUAL",
                     "PB_JOG_INDEX", "PB_JOG_PRESS", "PS_AIR_OK", "TS_OIL_OK",
                     "OL_M1_OK", "LUBE_LOW", "VFD_FAULT"):
            self.set_di(name, self.panel[name])
        self.set_di("SR1_OK", power)
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

        # ---------------- passive taps ----------------
        self.tap("T_ESTOP_A", est_a)
        self.tap("T_ESTOP_B", est_b)
        self.tap("T_GUARD_A", grd_a)
        self.tap("T_GUARD_B", grd_b)
        self.tap("T_SR1_K1", k1)
        self.tap("T_SR1_K2", k2)
        self.tap("T_CR1_COIL", cr1_coil)
        self.tap("T_CR1_CON", cr1_con)
        self.tap("T_CR2_COIL", cr2_coil)
        self.tap("T_CR2_CON", cr2_con)
        self.tap("T_CR3_COIL", cr3_coil)
        self.tap("T_CR3_CON", cr3_con)
        self.tap("T_M1_COIL", m1_coil)
        self.tap("T_M2_COIL", m2_coil)
        self.tap("T_SSR_OUT", self.ssr_on)
        self.tap("T_LS_PRESS_UP_2", press_up_2)

    def _station_error(self):
        r = self.counts % STATION
        return min(r, STATION - r)

    # ---------------- control commands ----------------
    def command(self, line):
        """
        press NAME [seconds]   momentary pushbutton (default 0.3 s)
        set NAME 0|1           panel switch or safety channel
        fault NAME VALUE       set a fault parameter (see self.faults)
        parts 0|1              parts available on infeed
        status                 one-line status
        """
        parts = line.strip().split()
        if not parts:
            return "empty"
        cmd = parts[0].lower()
        if cmd == "press" and len(parts) >= 2:
            name = parts[1].upper()
            secs = float(parts[2]) if len(parts) > 2 else 0.3
            if name not in self.panel:
                return "unknown button " + name
            self.panel[name] = 0 if name == "PB_STOP" else 1
            self.momentary[name] = self.now() + secs
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
        if cmd == "fault" and len(parts) == 3:
            name = parts[1]
            if name not in self.faults:
                return "unknown fault " + name
            cur = self.faults[name]
            v = parts[2]
            self.faults[name] = (v.lower() in ("1", "true", "on")) if isinstance(cur, bool) \
                else type(cur)(float(v))
            return "ok"
        if cmd == "parts" and len(parts) == 2:
            self.parts_available = parts[1] == "1"
            return "ok"
        if cmd == "status":
            return json.dumps({
                "t": round(self.now(), 2), "psi": round(self.psi), "ram": round(self.ram, 2),
                "clamp": round(self.clamp, 2), "pin": round(self.pin, 2),
                "counts": int(self.counts), "platen": round(self.platen, 1),
                "sr1": self.sr1_latched, "coils": {n: self.coil(n) for n in COILS},
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
    ap = argparse.ArgumentParser(description="GIPC field simulator")
    ap.add_argument("--modbus-port", type=int, default=5021)
    ap.add_argument("--tap-host", default="127.0.0.1")
    ap.add_argument("--tap-port", type=int, default=6000)
    ap.add_argument("--ctl-port", type=int, default=6001)
    ap.add_argument("--cold", action="store_true", help="start with a cold platen")
    args = ap.parse_args()

    m = Machine((args.tap_host, args.tap_port), preheat=not args.cold)
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: ControlProtocol(m),
                                        local_addr=("0.0.0.0", args.ctl_port))
    server = ModbusTCPServer(m.bank, port=args.modbus_port)
    print("GIPC simulator: Modbus :%d  taps -> %s:%d  control :%d"
          % (args.modbus_port, args.tap_host, args.tap_port, args.ctl_port), flush=True)
    await asyncio.gather(server.serve(), physics_loop(m))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
