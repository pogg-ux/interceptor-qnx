/*
 * I/O names for the Generic Indexing Press Cell (GIPC) demo machine.
 * Same order as the simulator's Modbus tables (sim/gipc_sim.py).
 * For a different machine, Teach Mode will replace these labels.
 */
#ifndef IO_MAP_H
#define IO_MAP_H

static const char *const DI_NAMES[] = {
    "SR1_OK", "PB_START", "PB_STOP", "PB_RESET", "SS_AUTO", "SS_MANUAL",
    "PB_JOG_INDEX", "PB_SAFE_JOG",
    "PX_TABLE_HOME", "PX_PIN_LOCKED", "LS_CLAMP_OPEN", "LS_CLAMP_CLOSED",
    "LS_PRESS_UP", "LS_PRESS_DOWN", "LS_EJECT_RET", "LS_EJECT_EXT",
    "PE_PART_LOAD", "PE_PART_INFEED", "M1_AUX", "PS_AIR_OK", "TS_OIL_OK",
    "OL_M1_OK", "LUBE_LOW", "VFD_FAULT",
    "SS_SEMI", "PANIC_ACTIVE", "SIM_PLC_POWERED", "SW_MAN_CLAMP",
    "SW_MAN_CONV", "MC_READY",
};
static const char *const COIL_NAMES[] = {
    "Y_MC_ENABLE", "Y_PIN_RETRACT", "Y_CLAMP_CLOSE", "Y_CLAMP_OPEN",
    "Y_PRESS_DOWN", "Y_PRESS_UP", "Y_EJECT_EXT", "Y_CR2_INDEX",
    "LT_RUN", "LT_FAULT", "LT_READY", "HORN", "Y_LUBE", "Y_VFD_RUN",
    "Y_CR3_HEAT",
};
static const char *const IR_NAMES[] = {
    "AI_HYD_PSI", "AI_RAM_POS", "AI_PLATEN_TEMP", "AI_PUMP_AMPS",
    "ENC_COUNT", "ENC_REVS", "VFD_HZ_ACT", "AI_OIL_TEMP",
};
static const char *const HR_NAMES[] = {
    "AO_VFD_SPEED", "AO_PROP_VALVE_UNUSED", "AO_HEAT_DUTY",
};

/* Hardwired relay/contactor coils and their contacts (taps). The contact
 * must follow its coil; disagreement means a welded or stuck contact. */
static const char *const COIL_CONTACT[][2] = {
    { "T_MCR_COIL", "T_MCR_CON" },
    { "T_M1_COIL",  "T_M1_CON"  },
    { "T_CR2_COIL", "T_CR2_CON" },
    { "T_CR3_COIL", "T_CR3_CON" },
};
/* Dual-channel safety signals. Both channels must agree. */
static const char *const DUAL_CHANNEL[][2] = {
    { "T_ESTOP_A", "T_ESTOP_B" },
    { "T_GUARD_A", "T_GUARD_B" },
    { "T_SR1_K1",  "T_SR1_K2"  },
};
/* Safety trips (tap goes off) and the safety contacts that must open after. */
static const char *const SAFETY_TRIPS[] = { "T_ESTOP_A", "T_ESTOP_B", "T_GUARD_A", "T_GUARD_B" };
static const char *const SAFETY_CONTACTS[] = { "T_SR1_K1", "T_SR1_K2" };
/* All of these ON = the machine is allowed to move (power to the outputs). */
static const char *const POWER_OK_TAPS[] = { "T_CTRL_POWER", "T_SR1_K1", "T_SR1_K2" };

/* Operator controls: a person changes these, never a PLC output, so the
 * mapper never treats them as the result of an output. */
static const char *const OPERATOR_INPUTS[] = {
    "PB_START", "PB_STOP", "PB_RESET", "PB_JOG_INDEX", "PB_SAFE_JOG",
    "SS_AUTO", "SS_MANUAL", "SS_SEMI", "SW_MAN_CLAMP", "SW_MAN_CONV",
    "T_ESTOP_A", "T_ESTOP_B", "T_SEL_POWER", "T_SEL_START", "T_ALL_OFF",
    "T_PANIC", "T_GUARD_A", "T_GUARD_B", "T_MC_START", "T_MC_MANUAL",
};

/* Points that change constantly in normal running (heater pulse control).
 * Still shown in the live table, but kept off the event timeline and console
 * so they don't bury the motion and safety events. */
static const char *const QUIET_POINTS[] = { "T_SSR_OUT", "AO_HEAT_DUTY" };

#define N_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

#endif
