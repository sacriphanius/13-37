#include "tpms.h"
#include "lora_screen.h"
#include "aprs.h"
#include <LilyGoLib.h>

// Mayhem FSK_19k2_Schrader TPMS format
//   Chip rate : 19.2 kbps NRZ FSK
//   Deviation : +-38.4 kHz
//   Preamble  : "01"x14 + "10" (30 chips), NO sync byte
//   Payload   : Manchester pre-encoded, MSB first; chip "10" = bit 1, "01" = bit 0
//   Formats   : FLM_64 (8 data bytes, sum checksum in byte[7])
//                FLM_72 (9 data bytes, CRC-8 poly=0x01 over bytes[0..8] = 0)
//                FLM_80 (10 data bytes, CRC-8 poly=0x01 over bytes[1..9] = 0)
//
// Receive strategy: configure SX1262 for 19.2 kbps NRZ with a 1-byte preamble
// sync (0x55 fires on the alternating preamble itself), then capture 24 raw
// chip bytes. Software scans even bit offsets to locate and decode the payload.
//
// Mayhem OOK_8k192 Schrader format
//   Chip rate : 8192 baud OOK
//   Preamble  : "01"x10 + "1110" = 24 chips (no "1111" prefix)
//   Payload   : 37 Manchester-decoded bits; chip "10"=1, "01"=0
//   Checksum  : (bit[0] + sum of 2-bit pairs bits[1..36]) & 3 == 3
//
// (Mayhem's OOK_8k4 GMC_96 format is NOT supported: its 76-bit/~240-chip packet
// is too long to capture reliably via RSSI polling at ~2.3 samples/chip. See
// README and the project notes; the selector only offers FSK and Schrader.)
//
// OOK receive strategy: SX1262 has no OOK hardware support via RadioLib.
// The radio is placed in wide-band FSK continuous-RX mode so GetRssiInst
// (radio.getRSSI(false)) stays accessible. A FreeRTOS task on core 0 polls
// RSSI continuously, detects OOK bursts, fills a chip buffer, then decodes.

#define TPMS_CHIP_BUF     32
#define TPMS_DATA_MAX     10
#define OOK_BUF_SCHRADER  256

static bool            s_running    = false;
static bool            s_freq_433   = true;
static TpmsFormat      s_format     = TPMS_FORMAT_FSK;
static int16_t         s_last_error = 0;
static volatile bool   s_rx_flag    = false;
static void IRAM_ATTR  on_rx_isr()  { s_rx_flag = true; }

static volatile int     s_isr_count      = 0;
static volatile int     s_crc_count      = 0;
static volatile int     s_sync_count     = 0;  // "1110" found + alt check passed
static volatile int     s_raw_sync_count = 0;  // "1110" found anywhere (before alt)
static volatile int     s_man_count      = 0;  // Manchester decode passed (checksum not yet checked)
static volatile int     s_last_n_chips   = 0;  // chip count of last captured burst
static volatile int     s_max_n_chips    = 0;  // longest burst seen since start
static volatile uint8_t s_last_chip8     = 0;  // first 8 chips of last burst packed
static volatile float   s_ook_threshold  = 0;  // calibrated noise threshold (dBm)

int     tpms_get_isr_count()      { return s_isr_count;      }
int     tpms_get_crc_count()      { return s_crc_count;      }
int     tpms_get_sync_count()     { return s_sync_count;     }
int     tpms_get_raw_sync_count() { return s_raw_sync_count; }
int     tpms_get_man_count()      { return s_man_count;      }
int     tpms_get_last_n_chips()   { return s_last_n_chips;   }
int     tpms_get_max_n_chips()    { return s_max_n_chips;    }
uint8_t tpms_get_last_chip8()     { return s_last_chip8;     }
float   tpms_get_ook_threshold()  { return s_ook_threshold;  }

static TpmsSensor s_sensors[4]   = {};
static uint32_t   s_ids[4]       = {};
static bool       s_id_set[4]    = {};
static int        s_count        = 0;

// OOK task state
static TaskHandle_t s_ook_task_handle = nullptr;
static uint8_t      s_ook_chip_buf[OOK_BUF_SCHRADER];  // static — safe to force-delete task

// ---- shared helpers --------------------------------------------------------

static void store_sensor(const TpmsSensor &s)
{
    for (int i = 0; i < 4; i++) {
        if (s_id_set[i] && s_ids[i] == s.id) {
            s_sensors[i] = s;
            return;
        }
    }
    if (s_count < 4) {
        s_ids[s_count]     = s.id;
        s_id_set[s_count]  = true;
        s_sensors[s_count] = s;
        s_count++;
        return;
    }
    int oldest = 0;
    for (int i = 1; i < 4; i++) {
        if (s_sensors[i].last_seen_ms < s_sensors[oldest].last_seen_ms)
            oldest = i;
    }
    s_ids[oldest]     = s.id;
    s_id_set[oldest]  = true;
    s_sensors[oldest] = s;
}

// ---- FSK helpers -----------------------------------------------------------

// Bit accessor: MSB-first byte order (SX1262 FSK NRZ convention).
static int chip_at(const uint8_t *buf, int pos)
{
    return (buf[pos >> 3] >> (7 - (pos & 7))) & 1;
}

// Manchester-decode `nbytes` data bytes starting at chip-bit offset `start`.
// Chip pair "10" -> data 1, "01" -> data 0. Returns false on violation or
// buffer overrun.
static bool man_decode(const uint8_t *chips, int start, int nbytes, uint8_t *out)
{
    const int limit = TPMS_CHIP_BUF * 8;
    for (int di = 0; di < nbytes; di++) {
        uint8_t b = 0;
        for (int bi = 0; bi < 8; bi++) {
            int p = start + (di * 8 + bi) * 2;
            if (p + 1 >= limit) return false;
            int c0 = chip_at(chips, p);
            int c1 = chip_at(chips, p + 1);
            if (c0 == c1) return false;
            if (c0) b |= (0x80u >> bi);
        }
        out[di] = b;
    }
    return true;
}


static float raw_to_kpa_fsk(uint8_t raw) { return raw * 8.0f / 3.0f; }
static int8_t raw_to_c_fsk(uint8_t raw)  { return (int8_t)((int)raw * 5 / 8); }

// Scan even bit offsets in the 24-byte chip buffer, trying FLM_64/72/80.
static bool decode_fsk_packet(const uint8_t *chips, TpmsSensor *out)
{
    uint8_t d[TPMS_DATA_MAX];

    // All FLM variants share byte layout:
    //   d[0]      = flags (status byte, not part of sensor ID)
    //   d[1..3]   = 24-bit sensor ID
    //   d[4]      = pressure raw   (plausible range 30..220 covers ~8..58 PSI)
    //   d[5]      = temperature raw
    //   d[6..]    = format-specific extra bytes
    //   d[last]   = checksum
    //
    // Scan all positions and return the first that passes both checksum and a
    // pressure sanity check.  The simple sum checksum has a ~1/256 false-positive
    // rate per position; the plausibility gate eliminates most coincidental hits.
    for (int start = 0; start < 64; start += 2) {
        // FLM_64: sum of bytes[0..6] == byte[7]
        if (man_decode(chips, start, 8, d)) {
            uint8_t s = 0;
            for (int i = 0; i < 7; i++) s += d[i];
            if (s == d[7] && d[4] >= 30 && d[4] <= 220 && d[5] <= 160) {
                out->id             = (((uint32_t)d[1]<<16) | ((uint32_t)d[2]<<8) | d[3]) << 1;
                out->pressure_alarm = false;
                out->battery_low    = false;
                out->pressure_kpa   = raw_to_kpa_fsk(d[4]);
                out->temp_c         = raw_to_c_fsk(d[5]);
                out->last_seen_ms   = millis();
                out->valid          = true;
                return true;
            }
        }
        // FLM_72: 9 bytes; XOR of bytes[0..7] == byte[8].
        //   d[0]=flags  d[1..3]=ID (<<1)  d[4]=status  d[5]=pressure  d[6]=temp
        if (man_decode(chips, start, 9, d)) {
            uint8_t x72 = 0;
            for (int i = 0; i < 8; i++) x72 ^= d[i];
            if (x72 == d[8] && d[5] >= 30 && d[5] <= 220 && d[6] <= 160) {
                out->id             = (((uint32_t)d[1]<<16) | ((uint32_t)d[2]<<8) | d[3]) << 1;
                out->pressure_alarm = false;
                out->battery_low    = false;
                out->pressure_kpa   = raw_to_kpa_fsk(d[5]);
                out->temp_c         = raw_to_c_fsk(d[6]);
                out->last_seen_ms   = millis();
                out->valid          = true;
                return true;
            }
        }
        // FLM_80: 10 bytes; XOR of bytes[0..8] == byte[9].  Same as FLM_72 but
        // with an extra byte after the flags, so every field shifts down one:
        //   d[0]=flags  d[1]=status  d[2..4]=ID (<<1)  d[5]=status
        //   d[6]=pressure  d[7]=temp
        if (man_decode(chips, start, 10, d)) {
            uint8_t x80 = 0;
            for (int i = 0; i < 9; i++) x80 ^= d[i];
            if (x80 == d[9] && d[6] >= 30 && d[6] <= 220 && d[7] <= 160) {
                out->id             = (((uint32_t)d[2]<<16) | ((uint32_t)d[3]<<8) | d[4]) << 1;
                out->pressure_alarm = false;
                out->battery_low    = false;
                out->pressure_kpa   = raw_to_kpa_fsk(d[6]);
                out->temp_c         = raw_to_c_fsk(d[7]);
                out->last_seen_ms   = millis();
                out->valid          = true;
                return true;
            }
        }
    }
    return false;
}

// ---- OOK helpers -----------------------------------------------------------

// Manchester chip pair "10" = 1, "01" = 0, else -1 (violation).
static int ook_man_pair(const uint8_t *chips, int pos)
{
    uint8_t c0 = chips[pos];
    uint8_t c1 = chips[pos + 1];
    if (c0 == 1 && c1 == 0) return 1;
    if (c0 == 0 && c1 == 1) return 0;
    return -1;
}

// Manchester-decode n_bits bits from chip array starting at chip offset start.
// bits[] receives individual 0/1 values. Returns false on violation or overrun.
static bool ook_man_decode_bits(const uint8_t *chips, int n_chips,
                                 int start, uint8_t *bits, int n_bits)
{
    for (int i = 0; i < n_bits; i++) {
        int pos = start + i * 2;
        if (pos + 1 >= n_chips) return false;
        int b = ook_man_pair(chips, pos);
        if (b < 0) return false;
        bits[i] = (uint8_t)b;
    }
    return true;
}

// Read n_bits from a bit array as an unsigned integer (MSB first).
static uint32_t bits_read(const uint8_t *bits, int offset, int n_bits)
{
    uint32_t v = 0;
    for (int i = 0; i < n_bits; i++)
        v = (v << 1) | bits[offset + i];
    return v;
}

// ---- Schrader OOK_8k192 decoder --------------------------------------------
//   Preamble : "01"x10 + "1110" = 24 chips
//   Data     : 37 Manchester bits starting after the "1110" sync
//   Checksum : bit[0] + sum of 2-bit pairs bits[1..36] (Mayhem verbatim) & 3 == 3
//
//   RSSI-based capture misses the leading "0" chip (carrier off), so the
//   buffer starts from the first carrier-on chip of the preamble. We brute-
//   force every chip offset and let the Manchester decode + checksum reject
//   misaligned positions.
//
static bool ook_decode_schrader(const uint8_t *chips, int n_chips,
                                 TpmsSensor *out)
{
    if (n_chips < 78) return false;

    // Diagnostic pass: track "1110" sync marker occurrences.
    for (int s = 0; s + 4 <= n_chips; s++) {
        if (chips[s] != 1 || chips[s+1] != 1 ||
            chips[s+2] != 1 || chips[s+3] != 0) continue;
        s_raw_sync_count++;
        int alt = 0;
        for (int j = 1; j <= 24 && (s - j) >= 0; j++) {
            if (chips[s - j] == (uint8_t)(j & 1 ? 1 : 0)) alt++;
            else break;
        }
        if (alt >= 2) s_sync_count++;
    }

    // Decode pass: brute-force every chip offset.
    // No sync marker required — Manchester decode eliminates nearly all
    // misaligned positions, and the checksum rejects the rest.
    // This approach is insensitive to preamble structure or sync position
    // uncertainty. s_man_count tracks how deep we get before checksum.
    for (int s = 0; s + 78 <= n_chips; s++) {
        uint8_t bits[37];
        if (!ook_man_decode_bits(chips, n_chips, s, bits, 37)) continue;

        s_man_count++;  // Manchester decode passed

        // Checksum matches Mayhem tpms_packet.cpp verbatim:
        //   uint32_t sum = reader_.read(0, 1);
        //   for (i = 1; i < 37; i += 2) sum += reader_.read(i, 2);
        //   if ((sum & 3) == 3) valid
        uint32_t chk = bits[0];
        for (int i = 1; i < 37; i += 2)
            chk += (bits[i] << 1) | bits[i + 1];
        if ((chk & 3) != 3) continue;

        // bits[0..2]:  3-bit flags; bits[3..26]: 24-bit sensor ID
        // bits[27..34]: 8-bit pressure; bits[35..36]: 2-bit checksum
        out->id             = bits_read(bits, 3, 24);
        out->pressure_alarm = bits[0] != 0;
        out->battery_low    = bits[1] != 0;
        out->pressure_kpa   = bits_read(bits, 27, 8) * 4.0f / 3.0f;
        out->temp_c         = -128;  // not present in this format
        out->last_seen_ms   = millis();
        out->valid          = true;
        return true;
    }
    return false;
}

// ---- OOK FreeRTOS task (core 0) --------------------------------------------

static void ook_task(void *param)
{
    // Schrader is the only supported OOK format.
    const int  chip_us  = 122;              // 1e6/8192
    const int  buf_size = OOK_BUF_SCHRADER;

    // Calibrate noise threshold: 64 reads with 2-tick gaps. The margin above
    // the measured noise floor must exceed the instantaneous RSSI variance
    // (~5-8 dB) so carrier-off chips reliably slice to 0; too tight and noise
    // spikes read as 1, breaking Manchester decode (man stays 0). A strong
    // local transmitter sits tens of dB above noise, so +10 dB is safe.
    float noise_sum = 0;
    for (int i = 0; i < 64; i++) {
        noise_sum += radio.getRSSI(false);
        vTaskDelay(2);
    }
    float threshold = noise_sum / 64.0f + 10.0f;
    s_ook_threshold = threshold;

    int  n_chips   = 0;
    bool capturing = false;
    int  silence   = 0;

    while (true) {
        if (!s_running) {
            // Signal exit and block; tpms_stop() will force-delete us.
            vTaskSuspend(nullptr);
        }
        if (!capturing) {
            // Yield core 0 for ~1 ms so the Arduino loop on core 1
            // (instance.loop, LVGL, touch) gets uncontested SPI access.
            // Then poll RSSI 8 times in a tight burst to catch preamble onset.
            vTaskDelay(pdMS_TO_TICKS(1));
            bool hit = false;
            for (int k = 0; k < 8 && !hit; k++) {
                if (radio.getRSSI(false) > threshold) { hit = true; break; }
                delayMicroseconds(40);
            }
            if (hit) {
                capturing = true;
                n_chips   = 0;
                silence   = 0;
                s_ook_chip_buf[n_chips++] = 1;
            }
        } else {
            unsigned long t_chip = micros();

            float rssi = radio.getRSSI(false);
            int   chip = (rssi > threshold) ? 1 : 0;

            if (n_chips < buf_size) s_ook_chip_buf[n_chips++] = (uint8_t)chip;

            if (chip == 0) silence++;
            else           silence = 0;

            // 16 consecutive zero chips ~= 2 ms of silence: end of burst.
            if (silence >= 16 || n_chips >= buf_size) {
                s_last_n_chips = n_chips;
                if (n_chips > s_max_n_chips) s_max_n_chips = n_chips;
                uint8_t b8 = 0;
                for (int i = 0; i < 8 && i < n_chips; i++)
                    b8 |= (s_ook_chip_buf[i] & 1u) << (7 - i);
                s_last_chip8 = b8;
                s_isr_count++;

                TpmsSensor s = {};
                bool ok = ook_decode_schrader(s_ook_chip_buf, n_chips, &s);
                if (ok) {
                    s_crc_count++;
                    store_sensor(s);
                }

                capturing = false;
                n_chips   = 0;
                silence   = 0;
            } else {
                // Busy-wait for remainder of chip period to pace at chip rate.
                // Use unsigned arithmetic throughout: if getRSSI() blocked on
                // BUSY longer than chip_us, elapsed >= chip_us and the
                // condition is false so we skip the delay instead of passing
                // a wrapped-negative value to delayMicroseconds().
                unsigned long elapsed = (unsigned long)(micros() - t_chip);
                if (elapsed < (unsigned long)chip_us)
                    delayMicroseconds((unsigned long)chip_us - elapsed);
            }
        }
    }

    // Never reached — loop exits only via vTaskSuspend above, then
    // tpms_stop() force-deletes the task handle.
}

// ---- public API ------------------------------------------------------------

bool tpms_start(bool freq_433, TpmsFormat fmt)
{
    if (s_running) {
        if (s_freq_433 == freq_433 && s_format == fmt) return true;
        tpms_stop();
    }
    if (lora_screen_is_powered() || aprs_is_running()) return false;

    s_freq_433 = freq_433;
    s_format   = fmt;
    s_isr_count      = 0;
    s_crc_count      = 0;
    s_sync_count     = 0;
    s_raw_sync_count = 0;
    s_man_count      = 0;
    s_last_n_chips   = 0;
    s_max_n_chips    = 0;
    s_last_chip8     = 0;
    s_rx_flag        = false;

    if (fmt == TPMS_FORMAT_FSK) {
        // 19.2 kbps NRZ FSK, +-38.4 kHz deviation.
        // rxBW 117.3 kHz >= Carson BW 2*(38.4+9.6)=96 kHz.
        int16_t rc = radio.beginFSK(freq_433 ? 433.92f : 315.0f,
                                    19.2, 38.4, 117.3, 10, 16, 1.6);
        s_last_error = rc;
        if (rc != RADIOLIB_ERR_NONE) return false;

        radio.setEncoding(RADIOLIB_ENCODING_NRZ);
        uint8_t sync[] = { 0x55 };
        radio.setSyncWord(sync, sizeof(sync));
        radio.setCRC(0);
        radio.fixedPacketLengthMode(TPMS_CHIP_BUF);
        radio.setPacketReceivedAction(on_rx_isr);
        radio.startReceive();
    } else {
        // OOK: wide-band FSK receive so GetRssiInst stays accessible.
        // rxBW 234.3 kHz passes the full OOK amplitude envelope.
        // A 4-byte unlikely sync word keeps the radio in continuous sync-hunt
        // mode (no accidental packet completions from OOK patterns).
        int16_t rc = radio.beginFSK(freq_433 ? 433.92f : 315.0f,
                                    4.8, 5.0, 234.3, 10, 16, 1.6);
        s_last_error = rc;
        if (rc != RADIOLIB_ERR_NONE) return false;

        radio.setEncoding(RADIOLIB_ENCODING_NRZ);
        uint8_t ook_sync[] = { 0xD3, 0x91, 0xD3, 0x91 };
        radio.setSyncWord(ook_sync, sizeof(ook_sync));
        radio.setCRC(0);
        radio.fixedPacketLengthMode(64);
        // No packet received action -- RSSI polling does all the work.
        radio.startReceive();

        // The OOK task busy-polls RSSI continuously on core 0, which
        // starves the IDLE_0 task (priority 0) and would trip the task
        // watchdog after ~5 s. Suppress core-0 IDLE monitoring for the
        // duration; tpms_stop() re-enables it.
        disableCore0WDT();
        xTaskCreatePinnedToCore(ook_task, "ook_rx", 4096, nullptr,
                                5, &s_ook_task_handle, 0);
    }

    s_running = true;
    return true;
}

void tpms_stop()
{
    if (!s_running) return;
    s_running    = false;

    if (s_ook_task_handle) {
        vTaskDelete(s_ook_task_handle);
        s_ook_task_handle = nullptr;
        enableCore0WDT();  // restore IDLE_0 watchdog now that the task is gone
    }

    radio.clearPacketReceivedAction();
    radio.standby();
}

bool       tpms_is_running()      { return s_running;    }
bool       tpms_is_freq_433()     { return s_freq_433;   }
TpmsFormat tpms_get_format()      { return s_format;     }
int        tpms_get_total_count() { return s_count;      }
int16_t    tpms_last_error()      { return s_last_error; }

const TpmsSensor* tpms_get_sensor(TpmsPos pos)
{
    if (pos < 0 || pos > 3) return nullptr;
    return &s_sensors[pos];
}

void tpms_bg_tick()
{
    if (!s_running || !s_rx_flag) return;
    s_rx_flag = false;
    s_isr_count++;

    uint8_t buf[TPMS_CHIP_BUF] = {};
    int16_t rc = radio.readData(buf, TPMS_CHIP_BUF);
    radio.startReceive();

    if (rc != RADIOLIB_ERR_NONE) return;

    TpmsSensor s = {};
    if (decode_fsk_packet(buf, &s)) {
        s_crc_count++;
        store_sensor(s);
    }
}
