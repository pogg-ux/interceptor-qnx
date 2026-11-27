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

#define N_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

#endif
