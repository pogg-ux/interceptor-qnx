(* =====================================================================
   Generic Indexing Press Cell (GIPC) - PLC program
   Fictional machine for the Interceptor project. Written from scratch.
   Target: OpenPLC Runtime v3 (IEC 61131-3 Structured Text)
   Field I/O comes from the simulator as an OpenPLC Modbus slave device:
     discrete inputs -> %IX100.0..   coils -> %QX100.0..
     input registers -> %IW100..     holding registers -> %QW100..
   HMI talks to this PLC through %MW (Modbus holding registers 1024+).
   License: MIT
   ===================================================================== *)

PROGRAM gipc_main
  VAR
    (* ---------- Digital inputs ---------- *)
    SR1_OK          AT %IX100.0 : BOOL;   (* safety relay aux contact *)
    PB_START        AT %IX100.1 : BOOL;
    PB_STOP         AT %IX100.2 : BOOL;   (* NC: TRUE = healthy *)
    PB_RESET        AT %IX100.3 : BOOL;
    SS_AUTO         AT %IX100.4 : BOOL;
    SS_MANUAL       AT %IX100.5 : BOOL;
    PB_JOG_INDEX    AT %IX100.6 : BOOL;
    PB_JOG_PRESS    AT %IX100.7 : BOOL;
    PX_TABLE_HOME   AT %IX101.0 : BOOL;
    PX_PIN_LOCKED   AT %IX101.1 : BOOL;
    LS_CLAMP_OPEN   AT %IX101.2 : BOOL;
    LS_CLAMP_CLOSED AT %IX101.3 : BOOL;
    LS_PRESS_UP     AT %IX101.4 : BOOL;
    LS_PRESS_DOWN   AT %IX101.5 : BOOL;
    LS_EJECT_RET    AT %IX101.6 : BOOL;
    LS_EJECT_EXT    AT %IX101.7 : BOOL;
    PE_PART_LOAD    AT %IX102.0 : BOOL;
    PE_PART_INFEED  AT %IX102.1 : BOOL;
    M1_AUX          AT %IX102.2 : BOOL;
    PS_AIR_OK       AT %IX102.3 : BOOL;
    TS_OIL_OK       AT %IX102.4 : BOOL;   (* NC *)
    OL_M1_OK        AT %IX102.5 : BOOL;   (* NC *)
    LUBE_LOW        AT %IX102.6 : BOOL;
    VFD_FAULT       AT %IX102.7 : BOOL;

    (* ---------- Digital outputs ---------- *)
    Y_CR1_PUMP      AT %QX100.0 : BOOL;
    Y_PIN_RETRACT   AT %QX100.1 : BOOL;
    Y_CLAMP_CLOSE   AT %QX100.2 : BOOL;
    Y_CLAMP_OPEN    AT %QX100.3 : BOOL;
    Y_PRESS_DOWN    AT %QX100.4 : BOOL;
    Y_PRESS_UP      AT %QX100.5 : BOOL;
    Y_EJECT_EXT     AT %QX100.6 : BOOL;
    Y_CR2_INDEX     AT %QX100.7 : BOOL;
    LT_RUN          AT %QX101.0 : BOOL;
    LT_FAULT        AT %QX101.1 : BOOL;
    LT_READY        AT %QX101.2 : BOOL;
    HORN            AT %QX101.3 : BOOL;
    Y_LUBE          AT %QX101.4 : BOOL;
    Y_VFD_RUN       AT %QX101.5 : BOOL;
    Y_CR3_HEAT      AT %QX101.6 : BOOL;

    (* ---------- Analog inputs (raw 0..32000) ---------- *)
    AI_HYD_PSI      AT %IW100 : INT;
    AI_RAM_POS      AT %IW101 : INT;
    AI_PLATEN_TEMP  AT %IW102 : INT;      (* 0.1 degC *)
    AI_PUMP_AMPS    AT %IW103 : INT;
    ENC_COUNT       AT %IW104 : INT;      (* 0..3999 *)
    ENC_REVS        AT %IW105 : INT;
    VFD_HZ_ACT      AT %IW106 : INT;      (* Hz x10 *)
    AI_OIL_TEMP     AT %IW107 : INT;      (* 0.1 degC *)

    (* ---------- Analog outputs ---------- *)
    AO_VFD_SPEED    AT %QW100 : INT;
    AO_PROP_VALVE   AT %QW101 : INT;
    AO_HEAT_DUTY    AT %QW102 : INT;      (* 0..1000 = 0..100.0 % *)

    (* ---------- HMI <-> PLC memory ---------- *)
    HMI_CMD         AT %MW0  : WORD;      (* b0 start b1 stop b2 reset b3 jog index b4 jog press *)
    SP_PRESS_PSI    AT %MW1  : INT;
    SP_DWELL_MS     AT %MW2  : INT;
    SP_PLATEN_C     AT %MW3  : INT;
    ST_STATE        AT %MW10 : INT;
    ST_FAULT        AT %MW11 : INT;
    ST_CYCLES       AT %MW12 : INT;
    ST_CYCLE_MS     AT %MW13 : INT;

  END_VAR

  VAR
    (* ---------- Internal ---------- *)
    state           : INT := 0;
    fault_code      : INT := 0;
    first_scan      : BOOL := TRUE;
    manual_index    : BOOL := FALSE;
    cmd_start       : BOOL;
    cmd_stop        : BOOL;
    cmd_reset       : BOOL;
    cmd_jog_index   : BOOL;
    cmd_jog_press   : BOOL;
    start_edge      : R_TRIG;
    reset_edge      : R_TRIG;
    jog_index_edge  : R_TRIG;
    step_timer      : TON;     (* per-step watchdog, coarse on purpose *)
    dwell_timer     : TON;
    pump_timer      : TON;
    cycle_timer     : TON;
    flash_timer     : TON;
    flash           : BOOL;
    prev_state      : INT := -1;
    enc_start       : INT;
    enc_moved       : INT;
    platen_c        : INT;     (* whole degC *)
    heat_err        : INT;
    heat_duty       : INT;
    lube_cycles     : INT;
    lube_timer      : TON;
    cycles          : INT;
  END_VAR

  (* ================= Defaults ================= *)
  IF first_scan THEN
    IF SP_PRESS_PSI <= 0 THEN SP_PRESS_PSI := 1800; END_IF;
    IF SP_DWELL_MS  <= 0 THEN SP_DWELL_MS  := 3000; END_IF;
    IF SP_PLATEN_C  <= 0 THEN SP_PLATEN_C  := 180;  END_IF;
    first_scan := FALSE;
  END_IF;

  (* ================= Commands (panel OR HMI) ================= *)
  cmd_start     := PB_START     OR ((HMI_CMD AND 16#0001) <> 16#0000);
  cmd_stop      := (NOT PB_STOP) OR ((HMI_CMD AND 16#0002) <> 16#0000);
  cmd_reset     := PB_RESET     OR ((HMI_CMD AND 16#0004) <> 16#0000);
  cmd_jog_index := PB_JOG_INDEX OR ((HMI_CMD AND 16#0008) <> 16#0000);
  cmd_jog_press := PB_JOG_PRESS OR ((HMI_CMD AND 16#0010) <> 16#0000);

  start_edge(CLK := cmd_start);
  reset_edge(CLK := cmd_reset);
  jog_index_edge(CLK := cmd_jog_index);

  (* Flasher, 0.5 s *)
  flash_timer(IN := NOT flash_timer.Q, PT := T#500ms);
  IF flash_timer.Q THEN flash := NOT flash; END_IF;

  (* ================= Safety: any time SR1 drops ================= *)
  IF (NOT SR1_OK) AND (state <> 0) THEN
    state := 999;
  END_IF;

  (* Restart step watchdog whenever the state changes *)
  step_timer(IN := (state = prev_state), PT := T#5s);
  prev_state := state;

  (* All motion outputs off by default; each state turns on what it needs *)
  Y_PIN_RETRACT := FALSE;
  Y_CLAMP_CLOSE := FALSE;
  Y_CLAMP_OPEN  := FALSE;
  Y_PRESS_DOWN  := FALSE;
  Y_PRESS_UP    := FALSE;
  Y_EJECT_EXT   := FALSE;
  Y_CR2_INDEX   := FALSE;
  AO_PROP_VALVE := 0;
  HORN          := FALSE;

  (* ================= Sequence ================= *)
  CASE state OF

    0: (* IDLE *)
      Y_CR1_PUMP := FALSE;
      IF SR1_OK AND cmd_reset THEN state := 10; END_IF;

    10: (* SAFETY_CHECK *)
      IF NOT PS_AIR_OK THEN fault_code := 3; state := 900;
      ELSIF NOT TS_OIL_OK THEN fault_code := 4; state := 900;
      ELSIF NOT OL_M1_OK THEN fault_code := 5; state := 900;
      ELSE state := 20;
      END_IF;

    20: (* HYD_START *)
      Y_CR1_PUMP := TRUE;
      IF M1_AUX THEN state := 30;
      ELSIF pump_timer.Q THEN fault_code := 1; state := 900;
      END_IF;

    30: (* HOMING: press up, clamp open, eject back, pin locked *)
      Y_PRESS_UP := NOT LS_PRESS_UP;
      IF LS_PRESS_UP THEN Y_CLAMP_OPEN := NOT LS_CLAMP_OPEN; END_IF;
      IF LS_PRESS_UP AND LS_CLAMP_OPEN AND LS_EJECT_RET AND PX_PIN_LOCKED AND PX_TABLE_HOME THEN
        state := 40;
      ELSIF step_timer.Q THEN fault_code := 130; state := 900;
      END_IF;

    40: (* READY *)
      IF SS_AUTO AND start_edge.Q THEN state := 110; cycle_timer(IN := FALSE);
      ELSIF SS_MANUAL THEN state := 50;
      END_IF;

    50: (* MANUAL *)
      IF cmd_jog_press AND LS_CLAMP_CLOSED THEN
        Y_PRESS_DOWN := NOT LS_PRESS_DOWN;
        AO_PROP_VALVE := DINT_TO_INT(INT_TO_DINT(SP_PRESS_PSI) * 32000 / 3000);
      ELSIF cmd_jog_press THEN
        Y_CLAMP_CLOSE := NOT LS_CLAMP_CLOSED;
      ELSE
        Y_PRESS_UP := NOT LS_PRESS_UP;
        IF LS_PRESS_UP THEN Y_CLAMP_OPEN := NOT LS_CLAMP_OPEN; END_IF;
      END_IF;
      IF jog_index_edge.Q AND LS_PRESS_UP AND LS_CLAMP_OPEN THEN
        manual_index := TRUE; state := 170;
      ELSIF SS_AUTO AND start_edge.Q AND LS_PRESS_UP AND LS_CLAMP_OPEN THEN
        state := 110;
      ELSIF NOT SS_MANUAL AND NOT SS_AUTO THEN
        state := 40;
      END_IF;

    110: (* WAIT_PART *)
      IF cmd_stop THEN state := 40;
      ELSIF PE_PART_LOAD THEN state := 120; cycle_timer(IN := FALSE);
      END_IF;

    120: (* CLAMP_CLOSE *)
      Y_CLAMP_CLOSE := TRUE;
      IF LS_CLAMP_CLOSED THEN state := 130;
      ELSIF step_timer.Q THEN fault_code := 220; state := 900;
      END_IF;

    130: (* PRESS_DOWN *)
      Y_PRESS_DOWN := TRUE;
      AO_PROP_VALVE := DINT_TO_INT(INT_TO_DINT(SP_PRESS_PSI) * 32000 / 3000);
      IF LS_PRESS_DOWN THEN state := 140;
      ELSIF step_timer.Q THEN fault_code := 230; state := 900;
      END_IF;

    140: (* DWELL: hold pressure, platen must be within 10 degC *)
      Y_PRESS_DOWN := TRUE;
      AO_PROP_VALVE := DINT_TO_INT(INT_TO_DINT(SP_PRESS_PSI) * 32000 / 3000);
      IF dwell_timer.Q THEN state := 150; END_IF;

    150: (* PRESS_UP *)
      Y_PRESS_UP := TRUE;
      IF LS_PRESS_UP THEN state := 160;
      ELSIF step_timer.Q THEN fault_code := 250; state := 900;
      END_IF;

    160: (* CLAMP_OPEN *)
      Y_CLAMP_OPEN := TRUE;
      IF LS_CLAMP_OPEN THEN state := 170;
      ELSIF step_timer.Q THEN fault_code := 260; state := 900;
      END_IF;

    170: (* UNLOCK table pin *)
      Y_PIN_RETRACT := TRUE;
      IF NOT PX_PIN_LOCKED THEN state := 180; enc_start := ENC_COUNT;
      ELSIF step_timer.Q THEN fault_code := 270; state := 900;
      END_IF;

    180: (* INDEX one station = 1000 counts *)
      Y_PIN_RETRACT := TRUE;
      Y_CR2_INDEX := TRUE;
      enc_moved := (ENC_COUNT - enc_start + 4000) MOD 4000;
      IF (enc_moved >= 950) AND PX_TABLE_HOME THEN state := 190;
      ELSIF step_timer.Q THEN fault_code := 280; state := 900;
      END_IF;

    190: (* LOCK table pin *)
      IF PX_PIN_LOCKED THEN
        IF manual_index THEN manual_index := FALSE; state := 50;
        ELSE state := 200;
        END_IF;
      ELSIF step_timer.Q THEN fault_code := 290; state := 900;
      END_IF;

    200: (* EJECT out *)
      Y_EJECT_EXT := TRUE;
      IF LS_EJECT_EXT THEN state := 210;
      ELSIF step_timer.Q THEN fault_code := 300; state := 900;
      END_IF;

    210: (* EJECT back, count the cycle *)
      IF LS_EJECT_RET THEN
        cycles := cycles + 1;
        lube_cycles := lube_cycles + 1;
        ST_CYCLE_MS := REAL_TO_INT(TIME_TO_REAL(cycle_timer.ET) * 1000.0);
        IF cmd_stop OR NOT SS_AUTO THEN state := 40; ELSE state := 110; END_IF;
      ELSIF step_timer.Q THEN fault_code := 310; state := 900;
      END_IF;

    900: (* FAULT: motion off, pump stays on *)
      HORN := flash;
      IF reset_edge.Q THEN fault_code := 0; manual_index := FALSE; state := 10; END_IF;

    999: (* E-STOP *)
      Y_CR1_PUMP := FALSE;
      (* SR1 re-arms on the same reset press, so accept the held button *)
      IF SR1_OK AND cmd_reset THEN fault_code := 0; manual_index := FALSE; state := 10; END_IF;

  END_CASE;

  (* ================= Timers that depend on state ================= *)
  pump_timer(IN := (state = 20), PT := T#2s);
  dwell_timer(IN := (state = 140), PT := REAL_TO_TIME(INT_TO_REAL(SP_DWELL_MS) / 1000.0));
  cycle_timer(IN := (state >= 120) AND (state <= 210), PT := T#60s);

  (* ================= Background: platen heater ================= *)
  (* Simple proportional duty: 4% per degC of error, clamped 0..100% *)
  platen_c := AI_PLATEN_TEMP / 10;
  Y_CR3_HEAT := (state >= 20) AND (state < 900) AND TS_OIL_OK;
  IF Y_CR3_HEAT THEN
    heat_err := SP_PLATEN_C - platen_c;
    heat_duty := heat_err * 40 + 350;
    IF heat_duty < 0 THEN heat_duty := 0; END_IF;
    IF heat_duty > 1000 THEN heat_duty := 1000; END_IF;
  ELSE
    heat_duty := 0;
  END_IF;
  AO_HEAT_DUTY := heat_duty;

  (* ================= Background: lube pulse every 60 cycles ================= *)
  lube_timer(IN := Y_LUBE, PT := T#3s);
  IF lube_cycles >= 60 THEN Y_LUBE := TRUE; lube_cycles := 0; END_IF;
  IF lube_timer.Q THEN Y_LUBE := FALSE; END_IF;

  (* ================= Background: conveyor ================= *)
  Y_VFD_RUN := (state >= 110) AND (state < 900);
  IF Y_VFD_RUN THEN AO_VFD_SPEED := 16000; ELSE AO_VFD_SPEED := 0; END_IF;

  (* ================= Pilot lights + status ================= *)
  LT_RUN   := (state >= 110) AND (state < 900);
  LT_READY := ((state = 40) OR (state = 50)) OR ((state = 0) AND flash);
  LT_FAULT := (state >= 900) AND flash;
  ST_STATE := state;
  ST_FAULT := fault_code;
  ST_CYCLES := cycles;
END_PROGRAM


CONFIGURATION Config0
  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#10ms, PRIORITY := 0);
    PROGRAM Inst0 WITH Main : gipc_main;
  END_RESOURCE
END_CONFIGURATION
