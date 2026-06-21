#pragma once
#include <stdint.h>

enum TpmsPos { TPMS_T1 = 0, TPMS_T2 = 1, TPMS_T3 = 2, TPMS_T4 = 3 };

struct TpmsSensor {
    uint32_t id;
    float    pressure_kpa;    // 0 = no data yet
    int8_t   temp_c;          // -128 = no data yet / not supported by format
    bool     battery_low;
    bool     pressure_alarm;
    uint32_t last_seen_ms;
    bool     valid;
};

enum TpmsFormat {
    TPMS_FORMAT_FSK          = 0,  // FSK 19.2k — FLM_64/72/80
    TPMS_FORMAT_OOK_SCHRADER = 1,  // OOK 8.192k — Schrader OEM
    // (GMC_96 OOK removed: unreliable to capture via RSSI polling on the SX1262)
};

// freq_433: true = 433.92 MHz, false = 315 MHz
// Returns false if LoRa radio is currently in use.
bool              tpms_start(bool freq_433, TpmsFormat fmt = TPMS_FORMAT_FSK);
void              tpms_stop();
bool              tpms_is_running();
bool              tpms_is_freq_433();
TpmsFormat        tpms_get_format();
void              tpms_bg_tick();
const TpmsSensor* tpms_get_sensor(TpmsPos pos);
int               tpms_get_total_count();
int16_t           tpms_last_error();
int               tpms_get_isr_count();
int               tpms_get_crc_count();
int               tpms_get_sync_count();     // OOK: "1110" found + alt check passed
int               tpms_get_raw_sync_count(); // OOK: "1110" found anywhere (before alt check)
int               tpms_get_man_count();      // OOK: Manchester decode passed (before checksum)
int               tpms_get_last_n_chips();   // OOK: chip count of last captured burst
int               tpms_get_max_n_chips();    // OOK: longest burst since start
uint8_t           tpms_get_last_chip8();     // OOK: first 8 chips of last burst packed
float             tpms_get_ook_threshold();  // OOK: calibrated noise floor threshold (dBm)
