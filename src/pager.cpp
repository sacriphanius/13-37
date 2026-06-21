#include "pager.h"

void clock_screen_get_local_time(struct tm *out);
#include "lora_screen.h"
#include "aprs.h"
#include "usb_sd.h"
#include "gps_screen.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>

// ---- constants -------------------------------------------------------------

// Each POCSAG batch: 8 frames × 2 codewords = 16 codewords × 4 bytes = 64 bytes.
// RadioLib consumes the sync codeword via hardware detection; the 64 bytes are
// the payload codewords that follow the sync in the transmission.
#define BATCH_BYTES   64
#define BATCH_WORDS   16

#define POCSAG_IDLE  0x7A89C197UL

// FLEX: after preamble + A-sync (consumed by hardware), we receive 64 bytes.
// Same length as a POCSAG batch for simplicity.

// ---- state -----------------------------------------------------------------

// Set from the SX1262 DIO1 interrupt when a packet has been fully received.
// Checked (and cleared) by pager_bg_tick() on the main task.
static volatile bool s_rx_flag = false;
static void IRAM_ATTR on_rx_isr() { s_rx_flag = true; }

static bool      s_running   = false;
static float     s_freq_mhz  = 152.240f;
static PagerMode s_mode       = PAGER_POCSAG_1200;
static int16_t   s_last_error = 0;

// ---- Scanner state --------------------------------------------------------
// Curated list of common pager frequencies (US-centric VHF/UHF for POCSAG +
// 900 MHz NPCS band for FLEX). When the scanner is enabled we hop through
// this list, dwelling SCAN_HOP_MS per channel, extending the dwell for
// SCAN_STICKY_MS after any sync-word detection so a partial decode has time
// to complete before we move on.
//
// The SX1262 only tunes 150–960 MHz, so all entries must stay ≥ 150 MHz —
// anything lower causes radio.beginFSK() to return RADIOLIB_ERR_INVALID_FREQUENCY
// (-12) the moment the user flips the SCAN ALL toggle on. The Federal /
// VHF-low pager allocations at 138.150, 148.000, 148.135 and 149.175 MHz
// are real (and were listed here originally) but physically unreachable
// from this hardware, so they're omitted.
static const float SCANNER_FREQS[] = {
    // VHF — common POCSAG networks (all ≥ 150 MHz per the SX1262 floor)
    152.240f, 152.480f, 152.510f, 153.050f,
    157.450f, 158.100f, 158.700f, 169.625f,
    // UHF — common POCSAG networks
    454.025f, 454.250f, 454.475f,
    462.550f, 462.575f, 463.000f, 466.075f,
    // 900 MHz NPCS — FLEX dominant
    929.612f, 929.662f, 929.687f,
    931.062f, 931.187f, 931.337f,
    931.862f, 931.937f,
};
static const int SCANNER_FREQ_COUNT =
    (int)(sizeof(SCANNER_FREQS) / sizeof(SCANNER_FREQS[0]));
#define SCAN_HOP_MS      300u    // base dwell per channel before hopping
#define SCAN_STICKY_MS  2500u    // extra dwell after any RX activity

static bool      s_scan_all          = false;
static int       s_scan_idx          = 0;
static uint32_t  s_scan_hop_at_ms    = 0;
static uint32_t  s_scan_sticky_until = 0;

// Ring buffer — s_head is the index of the newest message.
static PagerMsg  s_msgs[PAGER_MSG_MAX];
static int       s_head  = 0;
static int       s_count = 0;

// Partial-message carry-over between consecutive POCSAG batches:
// If a batch ends mid-message, we keep the address here so the next batch
// can continue collecting message codewords into the same PagerMsg.
static bool     s_pending        = false;
static PagerMsg s_pending_msg    = {};
// Accumulated message bits for alphanumeric carry-over
static uint8_t  s_alpha_bits[320] = {};  // up to 16 msg codewords × 20 bits
static int      s_alpha_bit_cnt  = 0;

// ---- BCH(31,21) check ------------------------------------------------------
// Generator polynomial G(x) = x^10+x^9+x^8+x^6+x^5+x^3+1 = 0x769
// Used by both POCSAG and FLEX.

// Pure validator: returns true only when parity and BCH syndrome are both zero.
static bool bch_valid(uint32_t cw)
{
    uint32_t p = cw ^ (cw >> 16);
    p ^= p >> 8; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (p & 1) return false;
    uint32_t b = cw >> 1;
    for (int i = 20; i >= 0; i--)
        if (b & (1u << (i + 10))) b ^= (0x769u << i);
    return (b & 0x3FFu) == 0;
}

// Validate, or attempt up to 2-bit error correction (BCH(31,21) d_min=5
// guarantees correction of any 1- or 2-bit error pattern).
// Returns true if cw is valid; cw is updated in-place if corrected.
static bool bch_try_fix(uint32_t &cw)
{
    if (bch_valid(cw)) { return true; }

    // 1-bit: 32 candidates
    for (int b = 0; b < 32; b++) {
        uint32_t c = cw ^ (1u << b);
        if (bch_valid(c)) { cw = c; return true; }
    }

    // 2-bit: 496 candidates — ~1 ms worst-case for the full 16-codeword batch
    for (int b1 = 0; b1 < 31; b1++) {
        for (int b2 = b1 + 1; b2 < 32; b2++) {
            uint32_t c = cw ^ (1u << b1) ^ (1u << b2);
            if (bch_valid(c)) { cw = c; return true; }
        }
    }

    return false;
}

// ---- ring-buffer helpers ---------------------------------------------------

// Human label for the message type, used by both the in-RAM viewer and the
// SD log so the on-card format reads the same as the message list on screen.
static const char *func_name_for(uint8_t func)
{
    switch (func) {
    case PFUNC_TONE:    return "TONE";
    case PFUNC_NUMERIC: return "NUM";
    case PFUNC_ALPHA:   return "ALPHA";
    case PFUNC_VOICE:   return "VOICE";
    case PFUNC_FLEX:    return "FLEX";
    default:            return "?";
    }
}

// Append a tab-separated line for this message to /Pager/messages.txt.
// Same shape as the AirTag / Flipper / EvilTwin logs (RTC-stamped, optional
// GPS suffix when there's a fix). Called from the main task via commit_msg()
// — `pager_bg_tick()` is the only writer — so direct SD I/O here is fine.
// Gated on the card being mounted AND not currently exposed over USB MSC
// (the host owns the FAT while it's mounted).
static void sd_log_msg(const PagerMsg &m)
{
    if (usb_sd_is_running() || !instance.isCardReady()) return;
    if (!SD.exists("/Pager")) SD.mkdir("/Pager");

    File f = SD.open("/Pager/messages.txt", FILE_APPEND);
    if (!f) return;

    struct tm t;
    clock_screen_get_local_time(&t);
    f.printf("%04d-%02d-%02d %02d:%02d:%02d",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);

    f.printf("\tFREQ %.3f", m.freq_mhz);
    f.printf("\tTYPE %s",   func_name_for(m.func));
    f.printf("\tCAP %lu",   (unsigned long)m.capcode);

    // Message body — always emit a quoted field even for tone/voice so log
    // parsers can rely on it being present. \" inside the text is escaped
    // so the quotes around the field stay balanced.
    f.print("\tMSG \"");
    for (const char *p = m.text; *p; p++) {
        if (*p == '"' || *p == '\\') f.print('\\');
        f.print(*p);
    }
    f.print('"');

    if (gps_screen_has_lock() && instance.gps.location.isValid()) {
        f.printf("\tGPS %.6f,%.6f",
            instance.gps.location.lat(), instance.gps.location.lng());
        if (instance.gps.altitude.isValid())
            f.printf("\tAlt %.1fm", instance.gps.altitude.meters());
    }

    f.print("\n");
    f.close();
}

static void commit_msg(const PagerMsg &m)
{
    if (s_count > 0)
        s_head = (s_head + 1) % PAGER_MSG_MAX;
    s_msgs[s_head] = m;
    if (s_count < PAGER_MSG_MAX) s_count++;

    // Persist every decoded page — tone, numeric, alpha, voice, FLEX —
    // to the SD card. Failure is silent on purpose: the in-RAM ring
    // buffer already keeps the last N for the on-screen viewer, and SD
    // writes shouldn't gate radio decode.
    sd_log_msg(m);
}

// ---- POCSAG decode ---------------------------------------------------------

// Decode alphanumeric text from raw message bits (packed 7-bit ASCII, LSB first).
static int alpha_bits_to_text(const uint8_t *bits, int nb, char *out, int max)
{
    int olen = 0;
    for (int i = 0; i + 6 < nb && olen < max - 1; i += 7) {
        uint8_t ch = 0;
        for (int b = 0; b < 7; b++) ch |= bits[i + b] << b;
        if (ch == 0x04) break;                         // EOT
        if (ch >= 0x20 && ch < 0x7F) out[olen++] = (char)ch;
        else if (ch == '\r' || ch == '\n') out[olen++] = ' ';
    }
    out[olen] = '\0';
    return olen;
}

static const char POCSAG_NUM_MAP[] = "0123456789*U -)(";

static int decode_numeric(const uint32_t *cws, int n, char *out, int max)
{
    int olen = 0;
    for (int i = 0; i < n && olen < max - 1; i++) {
        // 5 nibbles per message codeword, packed in bits [30:11], MSB first
        for (int nib = 0; nib < 5 && olen < max - 1; nib++) {
            uint8_t d = (cws[i] >> (27 - nib * 4)) & 0xF;
            if (d == 0xF) goto done;
            out[olen++] = POCSAG_NUM_MAP[d];
        }
    }
done:
    out[olen] = '\0';
    return olen;
}

static void fill_time(PagerMsg &m)
{
    struct tm t;
    clock_screen_get_local_time(&t);
    snprintf(m.time_str, sizeof(m.time_str), "%02d:%02d:%02d",
             t.tm_hour, t.tm_min, t.tm_sec);
    // Stamp the channel the message landed on so the scanner UI can show
    // which preset surfaced the activity.
    m.freq_mhz = s_freq_mhz;
}

// Flush a completed (or timed-out) pending message.
static void flush_pending()
{
    if (!s_pending) return;
    if (s_pending_msg.func == PFUNC_ALPHA && s_alpha_bit_cnt > 0)
        alpha_bits_to_text(s_alpha_bits, s_alpha_bit_cnt,
                           s_pending_msg.text, sizeof(s_pending_msg.text));
    commit_msg(s_pending_msg);
    s_pending       = false;
    s_alpha_bit_cnt = 0;
}

static void process_pocsag_batch(const uint8_t *raw)
{
    uint32_t cws[BATCH_WORDS];
    for (int i = 0; i < BATCH_WORDS; i++) {
        cws[i] = ((uint32_t)raw[i*4+0] << 24) | ((uint32_t)raw[i*4+1] << 16) |
                 ((uint32_t)raw[i*4+2] <<  8) |  raw[i*4+3];
    }

    for (int i = 0; i < BATCH_WORDS; i++) {
        uint32_t cw = cws[i];

        // Fast path for exact IDLE match — no BCH overhead needed.
        if (cw == POCSAG_IDLE) { flush_pending(); continue; }

        // Codewords within 4 bits of IDLE are corrupted IDLEs.  BCH d_min=5
        // guarantees no valid address/message codeword is ever that close to
        // IDLE, so this reject has zero false-discard risk.  Without it,
        // 2-bit correction can map a 3-bit-corrupted IDLE to a different valid
        // codeword, producing phantom address detections.
        if (__builtin_popcount(cw ^ POCSAG_IDLE) <= 4) { flush_pending(); continue; }

        // Validate or attempt up to 2-bit error correction.
        if (!bch_try_fix(cw)) continue;

        // A 1- or 2-bit-corrupted IDLE may have been corrected back to IDLE.
        if (cw == POCSAG_IDLE) { flush_pending(); continue; }

        bool is_msg = (cw >> 31) & 1;

        if (!is_msg) {
            // Address codeword: start a new message
            flush_pending();

            int frame_slot = i / 2;   // which 8-frame slot (0-7)
            // POCSAG address: bits[30:13] = 18 MSBs of capcode, bits[12:11] = function
            uint32_t capcode = (((cw >> 13) & 0x3FFFFu) << 3) | (uint32_t)frame_slot;
            uint8_t  func    = (cw >> 11) & 0x3;

            s_pending_msg            = {};
            s_pending_msg.capcode    = capcode;
            s_pending_msg.func       = (uint8_t)func;
            s_pending_msg.received_ms = millis();
            fill_time(s_pending_msg);

            if (func == PFUNC_TONE) {
                // Pre-fill text; will flush at next IDLE or end-of-batch.
                // If message codewords follow, the address was likely a
                // BCH-miscorrected ALPHA and the text will be overwritten.
                snprintf(s_pending_msg.text, sizeof(s_pending_msg.text), "[tone only]");
                s_pending = true;
            } else if (func == PFUNC_VOICE) {
                snprintf(s_pending_msg.text, sizeof(s_pending_msg.text), "[voice alert]");
                s_pending = true;
            } else {
                s_pending = true;
            }

        } else if (s_pending) {
            // Message codeword continuing an in-progress decode.
            // If the address was BCH-corrected to VOICE or TONE but message
            // codewords follow, it was really an ALPHA address with corrupt
            // func bits — upgrade and accumulate.
            if (s_pending_msg.func == PFUNC_VOICE || s_pending_msg.func == PFUNC_TONE) {
                s_pending_msg.func = (uint8_t)PFUNC_ALPHA;
                s_pending_msg.text[0] = '\0';
            }

            if (s_pending_msg.func == PFUNC_NUMERIC) {
                // Decode numeric codewords immediately
                char tmp[12];
                decode_numeric(&cw, 1, tmp, sizeof(tmp));
                strncat(s_pending_msg.text,
                        tmp,
                        sizeof(s_pending_msg.text) - strlen(s_pending_msg.text) - 1);

            } else if (s_pending_msg.func == PFUNC_ALPHA) {
                // Accumulate bits [30:11] (20 bits) into alpha buffer.
                int base = s_alpha_bit_cnt;
                if (base + 20 <= (int)sizeof(s_alpha_bits)) {
                    for (int b = 30; b >= 11; b--)
                        s_alpha_bits[s_alpha_bit_cnt++] = (cw >> b) & 1;
                }
            }
        }
        // else: orphaned message codeword with no preceding address — skip
    }
    // Flush any message that reached the end of the batch without a trailing
    // IDLE codeword (e.g. capcode slot 7: address at codeword 14, message at
    // codeword 15, leaving no room for a following IDLE to trigger the flush).
    // For real multi-batch messages this produces a partial decode on the first
    // batch, but single-batch is the common case and "partial" beats "nothing".
    flush_pending();
}

// ---- FLEX decode -----------------------------------------------------------
// FLEX messages use BCH(31,21) codewords (same polynomial as POCSAG).
// This is a best-effort decoder: we extract the capcode from the first
// valid address-type codeword and text from subsequent message codewords.
// Full FLEX cycle/frame/block structure is not reconstructed.

static void process_flex_batch(const uint8_t *raw)
{
    // First 4 bytes = Frame Information Word (skip, just note its presence)
    uint32_t cws[BATCH_WORDS];
    for (int i = 0; i < BATCH_WORDS; i++) {
        cws[i] = ((uint32_t)raw[i*4+0] << 24) | ((uint32_t)raw[i*4+1] << 16) |
                 ((uint32_t)raw[i*4+2] <<  8) |  raw[i*4+3];
    }

    // Scan for valid codewords; first non-FIW valid codeword with bit31=0
    // is treated as containing a 21-bit capcode (FLEX uses longer codes but
    // 21 bits from bits[30:10] is a reasonable approximation for display).
    uint32_t capcode = 0;
    bool     found_addr = false;

    // Collect text bits from message codewords
    uint8_t bits[320];
    int     nb = 0;

    for (int i = 1; i < BATCH_WORDS; i++) {   // skip FIW at index 0
        uint32_t cw = cws[i];
        if (!bch_try_fix(cw)) continue;

        if (!found_addr && !(cw >> 31)) {
            // Address-type codeword: extract capcode from bits[30:10] (21 bits)
            capcode    = (cw >> 10) & 0x1FFFFFu;
            found_addr = true;
        } else if ((cw >> 31) && found_addr) {
            // Message codeword: accumulate bits [30:11] for text extraction
            if (nb + 20 <= (int)sizeof(bits)) {
                for (int b = 11; b <= 30; b++)
                    bits[nb++] = (cw >> b) & 1;
            }
        }
    }

    if (!found_addr || nb == 0) return;

    PagerMsg m = {};
    m.capcode     = capcode;
    m.func        = PFUNC_FLEX;
    m.received_ms = millis();
    fill_time(m);
    alpha_bits_to_text(bits, nb, m.text, sizeof(m.text));
    if (m.text[0] == '\0')
        snprintf(m.text, sizeof(m.text), "[FLEX frame]");
    commit_msg(m);
}

// ---- public API ------------------------------------------------------------

bool pager_start(float freq_mhz, PagerMode mode)
{
    if (s_running && s_freq_mhz == freq_mhz && s_mode == mode) return true;
    if (lora_screen_is_powered() || aprs_is_running()) return false;

    // The SX1262 can only tune 150–960 MHz; anything outside that range
    // turns into RADIOLIB_ERR_INVALID_FREQUENCY (-12) on radio.beginFSK()
    // below, which surfaces to the user as the inscrutable "Radio init
    // failed (-12)". Reject the request here with a deterministic error
    // and a log line so future preset / dropdown changes can't reintroduce
    // that confusing failure mode silently.
    if (freq_mhz < 150.0f || freq_mhz > 960.0f) {
        s_last_error = RADIOLIB_ERR_INVALID_FREQUENCY;
        return false;
    }

    s_freq_mhz = freq_mhz;
    s_mode     = mode;
    s_pending  = false;
    s_alpha_bit_cnt = 0;

    float    bitrate, deviation, bw;
    uint8_t  sync_bytes[4];
    size_t   sync_len;

    if (mode == PAGER_FLEX_1600) {
        // FLEX 1600 bps, 2FSK, ±4.8 kHz deviation
        // Phase A sync bytes (two copies of 0x780C after preamble)
        bitrate   = 1.6f;
        deviation = 4.8f;
        bw        = 46.9f;   // SX1262 only accepts discrete RX bandwidths
        sync_bytes[0] = 0x78; sync_bytes[1] = 0x0C;
        sync_bytes[2] = 0x78; sync_bytes[3] = 0x0C;
        sync_len = 4;
    } else {
        // POCSAG: NRZ FSK, ±4.5 kHz, sync = 0x7CD215D8
        bitrate   = (mode == PAGER_POCSAG_512)  ? 0.512f :
                    (mode == PAGER_POCSAG_2400) ? 2.4f   : 1.2f;
        deviation = 4.5f;
        // Carson bandwidth for POCSAG: 2*(4.5+1.2) = 11.4 kHz.
        // 14.6 kHz is the narrowest SX1262 step that fits the signal and
        // still tolerates ~1.5 kHz frequency offset; much narrower than the
        // old 46.9 kHz, giving ~5 dB better SNR.
        bw        = 14.6f;
        // SX1262 RX maps HIGH=1 but POCSAG sends mark=LOW, so all bits arrive
        // inverted.  Give the hardware the bitwise complement of the sync
        // word so the sync detector fires on the correct on-air pattern.
        sync_bytes[0] = 0x83u; sync_bytes[1] = 0x2Du;
        sync_bytes[2] = 0xEAu; sync_bytes[3] = 0x27u;
        sync_len = 4;
    }

    // SX1262::beginFSK args: (freq, br, freqDev, rxBw, power, preambleLen,
    // tcxoVoltage). power is irrelevant for RX but must be a valid SX1262
    // value (-9..+22 dBm). tcxoVoltage 1.6 V matches LilyGoLib's radio.begin().
    int16_t rc = radio.beginFSK(freq_mhz, bitrate, deviation, bw, 10, 32, 1.6f);
    s_last_error = rc;
    if (rc != RADIOLIB_ERR_NONE) return false;

    radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    radio.setCRC(0);
    radio.setSyncWord(sync_bytes, sync_len);
    radio.fixedPacketLengthMode(BATCH_BYTES);
    s_rx_flag = false;
    radio.setPacketReceivedAction(on_rx_isr);
    radio.startReceive();

    s_running = true;
    return true;
}

void pager_stop()
{
    if (!s_running) return;
    flush_pending();
    s_running = false;
    s_scan_all = false;     // single-freq stop also drops scanner mode
    radio.standby();
}

// Hop the radio to a different channel without re-running beginFSK(). All the
// FSK params (bitrate, deviation, sync word) stay valid across a frequency
// retune — only the LO and PLL need to settle. ~3-5 ms typical.
static int16_t scanner_retune_to(float freq_mhz)
{
    int16_t rc = radio.standby();
    if (rc != RADIOLIB_ERR_NONE) return rc;
    rc = radio.setFrequency(freq_mhz);
    if (rc != RADIOLIB_ERR_NONE) return rc;
    s_freq_mhz = freq_mhz;
    return radio.startReceive();
}

bool pager_start_scanner(PagerMode mode)
{
    if (lora_screen_is_powered() || aprs_is_running()) return false;

    s_scan_all          = false;        // pager_start() picks up the new freq
    s_scan_idx          = 0;
    s_scan_hop_at_ms    = millis() + SCAN_HOP_MS;
    s_scan_sticky_until = 0;

    // Spin up RX on the first channel with the requested mode. After that,
    // the scanner hops via the cheap setFrequency() path inside pager_bg_tick.
    if (!pager_start(SCANNER_FREQS[0], mode)) return false;
    s_scan_all = true;
    return true;
}

bool   pager_is_scanning_all() { return s_scan_all;          }
int    pager_scan_freq_count() { return SCANNER_FREQ_COUNT;  }
float  pager_scan_freq_at(int i)
{
    if (i < 0 || i >= SCANNER_FREQ_COUNT) return 0.0f;
    return SCANNER_FREQS[i];
}
int    pager_scan_current_idx() { return s_scan_idx; }

bool      pager_is_running()     { return s_running;  }
float     pager_get_freq()       { return s_freq_mhz; }
PagerMode pager_get_mode()       { return s_mode;     }
int16_t   pager_last_error()     { return s_last_error; }
int       pager_get_msg_count()  { return s_count;    }

// Store the mode for the next pager_start(). While running, mode changes go
// through pager_start() instead so the radio is actually re-tuned.
void pager_set_mode(PagerMode mode) { if (!s_running) s_mode = mode; }

const PagerMsg* pager_get_msg(int idx)
{
    if (idx < 0 || idx >= s_count) return nullptr;
    int pos = (s_head - idx + PAGER_MSG_MAX) % PAGER_MSG_MAX;
    return &s_msgs[pos];
}

void pager_clear_msgs()
{
    s_count = 0;
    s_head  = 0;
}

void pager_bg_tick()
{
    if (!s_running) return;

    if (s_rx_flag) {
        s_rx_flag = false;      // clear before readData() to avoid missing a back-to-back packet

        uint8_t buf[BATCH_BYTES] = {};
        int16_t rc = radio.readData(buf, BATCH_BYTES);
        radio.startReceive();   // re-arm for next batch

        if (rc == RADIOLIB_ERR_NONE) {
            if (s_mode == PAGER_FLEX_1600) {
                process_flex_batch(buf);
            } else {
                // SX1262 maps HIGH=1 but POCSAG marks LOW=1, so every received
                // byte is bit-inverted relative to the POCSAG codewords.
                for (int j = 0; j < BATCH_BYTES; j++) buf[j] = ~buf[j];
                process_pocsag_batch(buf);
            }
        }

        // Whether or not the batch decoded cleanly, the sync word DID match
        // on this channel — extend the dwell so a follow-on batch (long
        // alpha messages span multiple batches) gets a chance to land.
        if (s_scan_all)
            s_scan_sticky_until = millis() + SCAN_STICKY_MS;
    }

    // Scanner hop: when not currently dwelling on a hot channel and the
    // base hop interval has elapsed, retune to the next preset.
    if (s_scan_all) {
        uint32_t now = millis();
        if (now >= s_scan_sticky_until && (int32_t)(now - s_scan_hop_at_ms) >= 0) {
            // If a partial alpha message is still being accumulated when
            // we hop, flush it — the next batch on a different channel
            // would have a different capcode context anyway.
            flush_pending();

            s_scan_idx = (s_scan_idx + 1) % SCANNER_FREQ_COUNT;
            scanner_retune_to(SCANNER_FREQS[s_scan_idx]);
            s_scan_hop_at_ms = now + SCAN_HOP_MS;
        }
    }
}

// ============================================================================
// POCSAG encoder + transmit
// ============================================================================
//
// POCSAG transmission layout we generate:
//
//   [ 576 bits preamble: 0xAA repeated  72 bytes ]
//   [ sync codeword 0x7CD215D8                   ]   <- carried as the SX1262
//                                                     hardware sync word, the
//                                                     radio prepends it
//                                                     automatically.
//   [ batch 1: 16 codewords (64 bytes)           ]
//   [ optional sync + batch 2 ...                ]
//
// On the SX1262 the preamble is configured via beginFSK's preambleLen argument
// (bit count). The sync word is set with setSyncWord(). The "packet" we hand
// to transmit() is the codeword payload only: one 64-byte batch per call.
// Multi-batch messages send a second packet immediately after with another
// sync word — most receivers expect a fresh sync between batches anyway.

// Reverse the 20 message bits so that char-bit-0 (LSB) ends up at the MSB of
// msg20 (= codeword bit 30, the first transmitted message bit).  POCSAG puts
// each character's LSB first in the on-air bit stream.
static uint32_t bitrev20(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 20; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

// Build a 32-bit codeword from a 31-bit value (flag + data + BCH-placeholder).
// Computes the BCH(31,21) check bits and the trailing even-parity bit.
static uint32_t bch_finalize(uint32_t v)
{
    // v occupies bits[31:1] with bits[10:1] reserved for BCH (zero on entry).
    // Compute BCH parity by polynomial division of v[31:11] by 0x769.
    uint32_t b = v;
    for (int i = 20; i >= 0; i--)
        if (b & (1u << (i + 11))) b ^= (0x769u << (i + 1));
    v |= b & 0x7FE;     // place 10 BCH bits into bits[10:1]

    // Trailing even-parity over bits[31:1]
    uint32_t p = v ^ (v >> 16);
    p ^= p >> 8; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    v |= (p & 1);
    return v;
}

// POCSAG address codeword: bit31=0, bits[30:13]=top 18 bits of capcode,
// bits[12:11]=function. Bits[10:1]=BCH, bit0=parity.
static uint32_t pocsag_address_cw(uint32_t capcode, uint8_t func)
{
    uint32_t top18 = (capcode >> 3) & 0x3FFFFu;
    uint32_t v = (top18 << 13) | ((uint32_t)(func & 0x3) << 11);
    return bch_finalize(v);
}

// POCSAG message codeword: bit31=1, bits[30:11]=20 message bits, BCH+parity.
static uint32_t pocsag_message_cw(uint32_t msg20)
{
    uint32_t v = (1u << 31) | ((msg20 & 0xFFFFFu) << 11);
    return bch_finalize(v);
}

// Pack alphanumeric text into a stream of 20-bit message codewords. Bits are
// LSB-first 7-bit ASCII concatenated end-to-end and chunked into 20-bit
// groups; the trailing partial group is padded with EOT (0x04) and then 1s.
// Returns the number of message codewords produced (max=15 — one address
// slot + 15 message slots = a full batch).
static int pocsag_pack_alpha(const char *text, uint32_t *out, int max_words)
{
    // Collect bits LSB-first per char.
    uint64_t acc = 0;
    int      acc_n = 0;
    int      out_n = 0;
    for (const char *p = text; *p && out_n < max_words; p++) {
        char c = *p;
        if (c < 0x20 || c > 0x7E) continue;       // skip control / non-ASCII
        for (int b = 0; b < 7; b++) {
            acc |= ((uint64_t)((c >> b) & 1)) << acc_n;
            acc_n++;
            if (acc_n >= 20) {
                out[out_n++] = bitrev20((uint32_t)(acc & 0xFFFFFu));
                acc >>= 20;
                acc_n -= 20;
                if (out_n >= max_words) return out_n;
            }
        }
    }
    // Append EOT (0x04, 7 bits) so the receiver knows to stop. Pad trailing
    // bits with 1s — the receiver's bits→text loop terminates on EOT before
    // it sees the padding.
    for (int b = 0; b < 7; b++) {
        acc |= ((uint64_t)((0x04 >> b) & 1)) << acc_n;
        acc_n++;
    }
    while (acc_n < 20 && out_n < max_words) {
        acc |= ((uint64_t)1) << acc_n;
        acc_n++;
    }
    if (acc_n > 0 && out_n < max_words)
        out[out_n++] = bitrev20((uint32_t)(acc & 0xFFFFFu));
    return out_n;
}

// Encode numeric digits into 20-bit message codewords. 5 nibbles per word,
// LSB of each nibble first. Pads with 0xC ('space') so the receiver fills to
// the next codeword boundary; trailing 0xF in the final word means EOM.
static int pocsag_pack_numeric(const char *text, uint32_t *out, int max_words)
{
    // Map ASCII back to the POCSAG numeric alphabet "0123456789*U -)("
    auto map_ch = [](char c) -> int {
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9': return c - '0';
        case '*': return 10;
        case 'U': case 'u': return 11;
        case ' ': return 12;
        case '-': return 13;
        case ')': return 14;
        case '(': return 15;
        default:  return -1;
        }
    };

    int out_n = 0;
    int nib_idx = 0;
    uint32_t word = 0;
    for (const char *p = text; *p && out_n < max_words; p++) {
        int d = map_ch(*p);
        if (d < 0) continue;
        // POCSAG numeric is MSB-first: digit 0 occupies codeword bits [30:27]
        // (= msg20 bits [19:16]), so shift each nibble down by 4 per slot.
        word |= ((uint32_t)d & 0xF) << (16 - nib_idx * 4);
        nib_idx++;
        if (nib_idx == 5) {
            out[out_n++] = word & 0xFFFFFu;
            word    = 0;
            nib_idx = 0;
        }
    }
    if (nib_idx > 0 && out_n < max_words) {
        // Trailing nibble 0xF marks EOM
        while (nib_idx < 5) {
            word |= 0xFu << (16 - nib_idx * 4);
            nib_idx++;
        }
        out[out_n++] = word & 0xFFFFFu;
    }
    return out_n;
}

// Build a single 64-byte batch. capcode chooses the frame slot (bits[2:0]);
// the address codeword goes in the first codeword of that slot, message
// codewords follow until the batch is full. Slots before the address slot
// get IDLE codewords; slots after a complete message get IDLE codewords too.
static void pocsag_build_batch(uint32_t capcode, uint8_t func,
                               const uint32_t *msg, int msg_n,
                               uint8_t out[BATCH_BYTES])
{
    uint32_t cws[BATCH_WORDS];
    for (int i = 0; i < BATCH_WORDS; i++) cws[i] = POCSAG_IDLE;

    int slot = (int)(capcode & 0x7);     // 0..7
    int addr_idx = slot * 2;             // which codeword (16 total)
    cws[addr_idx] = pocsag_address_cw(capcode, func);

    for (int i = 0; i < msg_n && (addr_idx + 1 + i) < BATCH_WORDS; i++)
        cws[addr_idx + 1 + i] = pocsag_message_cw(msg[i]);

    for (int i = 0; i < BATCH_WORDS; i++) {
        out[i*4 + 0] = (cws[i] >> 24) & 0xFF;
        out[i*4 + 1] = (cws[i] >> 16) & 0xFF;
        out[i*4 + 2] = (cws[i] >>  8) & 0xFF;
        out[i*4 + 3] =  cws[i]        & 0xFF;
    }
}

// Configure the radio for transmit at the given POCSAG/FLEX mode + frequency
// and send a single 64-byte payload. Caller is responsible for stop/restart
// of RX around this call.
static int16_t pager_tx_one_packet(float freq_mhz, PagerMode mode,
                                   const uint8_t *payload, int len,
                                   int preamble_bits)
{
    float    bitrate, deviation, bw;
    uint8_t  sync_bytes[4];
    size_t   sync_len;

    bool pocsag = (mode != PAGER_FLEX_1600);

    if (mode == PAGER_FLEX_1600) {
        bitrate   = 1.6f;
        deviation = 4.8f;
        bw        = 46.9f;
        sync_bytes[0] = 0x78; sync_bytes[1] = 0x0C;
        sync_bytes[2] = 0x78; sync_bytes[3] = 0x0C;
        sync_len = 4;
    } else {
        bitrate   = (mode == PAGER_POCSAG_512)  ? 0.512f :
                    (mode == PAGER_POCSAG_2400) ? 2.4f   : 1.2f;
        deviation = 4.5f;
        bw        = 46.9f;
        // SX1262 packet-mode FSK maps bit=1 to higher frequency, but POCSAG
        // standard requires bit=1 to map to lower frequency (mark=low,
        // space=high).  Pass the bitwise complement of the sync word so that
        // after the on-air polarity inversion the pager sees 0x7CD215D8.
        sync_bytes[0] = 0x83u; sync_bytes[1] = 0x2Du;
        sync_bytes[2] = 0xEAu; sync_bytes[3] = 0x27u;
        sync_len = 4;
    }

    // Power +10 dBm is conservative for a watch-form-factor transmitter --
    // the SX1262 can do up to +22 but bursts at that level pull noticeably
    // on the battery and heat the package.
    int16_t rc = radio.beginFSK(freq_mhz, bitrate, deviation, bw,
                                10, preamble_bits, 1.6f);
    if (rc != RADIOLIB_ERR_NONE) return rc;

    radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    radio.setCRC(0);   // POCSAG uses BCH; no separate RF-layer CRC wanted
    radio.setSyncWord(sync_bytes, sync_len);
    radio.fixedPacketLengthMode(len);

    if (pocsag) {
        // Invert every payload byte so the on-air bit polarity is correct.
        uint8_t inv[BATCH_BYTES];
        for (int i = 0; i < len && i < BATCH_BYTES; i++) inv[i] = ~payload[i];
        return radio.transmit(inv, len);
    }
    return radio.transmit((uint8_t *)payload, len);
}

// Save / restore RX state across a synchronous TX.
struct RxResume {
    bool      was_running;
    float     freq;
    PagerMode mode;
};

static RxResume pause_rx_for_tx()
{
    RxResume r = { s_running, s_freq_mhz, s_mode };
    if (s_running) {
        flush_pending();
        radio.standby();
        s_running = false;
    }
    return r;
}

static void resume_rx(const RxResume &r)
{
    if (r.was_running) {
        // Re-arm the original RX configuration.
        pager_start(r.freq, r.mode);
    }
}

bool pager_transmit_pocsag(uint32_t capcode, PagerFunc func,
                           const char *text, PagerMode mode, float freq_mhz)
{
    if (mode == PAGER_FLEX_1600) return false;       // wrong helper
    if (capcode == 0 || capcode > 0x1FFFFFu) return false;
    if (freq_mhz < 150.0f || freq_mhz > 960.0f) {
        // Same guard as pager_start() — the SX1262 only tunes 150–960 MHz
        // and asking for anything outside that range surfaces to the user
        // as the inscrutable "Failed (err -12)" rather than a clear
        // out-of-range message. Reject here with a deterministic error.
        s_last_error = RADIOLIB_ERR_INVALID_FREQUENCY;
        return false;
    }
    if (lora_screen_is_powered() || aprs_is_running()) return false;

    // Encode message body (tone-only has none).
    uint32_t msg[15] = {};   // 15 message codewords max per batch
    int msg_n = 0;
    if (func == PFUNC_ALPHA && text && *text)
        msg_n = pocsag_pack_alpha(text, msg, 15);
    else if (func == PFUNC_NUMERIC && text && *text)
        msg_n = pocsag_pack_numeric(text, msg, 15);

    // Build one batch — message + address + idle padding (one batch is
    // enough for ~30 alpha chars; longer messages would need multiple
    // batches, which we don't currently emit).
    uint8_t payload[BATCH_BYTES];
    pocsag_build_batch(capcode, (uint8_t)func, msg, msg_n, payload);

    RxResume saved = pause_rx_for_tx();
    // POCSAG preamble = 576 bits of alternating 0/1 (the radio synth itself
    // emits an alternating pattern when its preamble window is set).
    int16_t rc = pager_tx_one_packet(freq_mhz, mode, payload, BATCH_BYTES, 576);
    s_last_error = rc;
    resume_rx(saved);
    return rc == RADIOLIB_ERR_NONE;
}

// ============================================================================
// FLEX encoder + transmit (best-effort, matching the symmetric decoder)
// ============================================================================

bool pager_transmit_flex(uint32_t capcode, const char *text, float freq_mhz)
{
    if (capcode == 0 || capcode > 0x1FFFFFu) return false;
    if (freq_mhz < 150.0f || freq_mhz > 960.0f) {
        // SX1262 tuning range — see comment in pager_transmit_pocsag().
        s_last_error = RADIOLIB_ERR_INVALID_FREQUENCY;
        return false;
    }
    if (lora_screen_is_powered() || aprs_is_running()) return false;

    // Build a 16-codeword payload symmetric with process_flex_batch():
    //   codeword 0   = FIW (any valid BCH word — we use IDLE)
    //   codeword 1   = address codeword (bit31=0, capcode in bits[30:10])
    //   codeword 2.. = message codewords (bit31=1, 20 bits at [30:11])
    uint32_t cws[BATCH_WORDS];
    for (int i = 0; i < BATCH_WORDS; i++) cws[i] = POCSAG_IDLE;

    // FLEX address codeword — 21-bit capcode at [30:10], bit31=0.
    {
        uint32_t v = ((capcode & 0x1FFFFFu) << 10);
        cws[1] = bch_finalize(v);
    }

    // Pack text into 20-bit chunks just like the POCSAG alpha path. The RX
    // side reads bits[30:11] LSB-first as 7-bit ASCII, EOT-terminated.
    uint32_t msg[14] = {};
    int msg_n = (text && *text) ? pocsag_pack_alpha(text, msg, 14) : 0;
    for (int i = 0; i < msg_n && (2 + i) < BATCH_WORDS; i++)
        cws[2 + i] = pocsag_message_cw(msg[i]);

    uint8_t payload[BATCH_BYTES];
    for (int i = 0; i < BATCH_WORDS; i++) {
        payload[i*4 + 0] = (cws[i] >> 24) & 0xFF;
        payload[i*4 + 1] = (cws[i] >> 16) & 0xFF;
        payload[i*4 + 2] = (cws[i] >>  8) & 0xFF;
        payload[i*4 + 3] =  cws[i]        & 0xFF;
    }

    RxResume saved = pause_rx_for_tx();
    // FLEX preamble is shorter than POCSAG's — 128 bits is enough for the
    // SX1262 hardware sync detector at the lower 1.6 kbps rate.
    int16_t rc = pager_tx_one_packet(freq_mhz, PAGER_FLEX_1600,
                                     payload, BATCH_BYTES, 128);
    s_last_error = rc;
    resume_rx(saved);
    return rc == RADIOLIB_ERR_NONE;
}
