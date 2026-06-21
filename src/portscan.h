#pragma once
#include <stdint.h>
#include <stdbool.h>

// Port scanner. Reachable from a ping-sweep result row — tap a discovered
// host and the UI opens with that IP pre-filled. Runs on top of the lwIP
// BSD socket layer that's already brought up when the WiFi screen has the
// watch associated, so no extra build flags are needed.
//
// Three techniques exposed via the top button row on the screen:
//   - TCP Connect : full BSD socket() + connect() per port. Definitive
//                   open/closed; doesn't read any payload.
//   - UDP         : sends a small probe per port, waits briefly for any
//                   response. "Open" if anything comes back, otherwise
//                   "open|filtered" (no raw ICMP receive available here).
//   - Banner      : TCP Connect + a short recv() on success; the first
//                   ~96 bytes of any banner/server-greeting are captured
//                   alongside the open state.

// Techniques are independent flags so any combination can be run together.
// TCP_CONNECT + BANNER are coalesced into a single probe per port (the
// banner grab implies the connect) — the result rows still distinguish the
// two because banner output only appears when BANNER is in the mask.
#define PSTECH_TCP_CONNECT  (1u << 0)
#define PSTECH_UDP          (1u << 1)
#define PSTECH_BANNER       (1u << 2)
typedef uint8_t PortScanTech;   // bitmask of PSTECH_* flags

enum PortScanPreset {
    PSPRE_TOP_20  = 0,
    PSPRE_TOP_100 = 1,
    PSPRE_CUSTOM  = 2,
};

enum PortState {
    PSTATE_CLOSED      = 0,
    PSTATE_OPEN        = 1,
    // For UDP scans without raw-ICMP receive: response → OPEN, silence →
    // OPEN_OR_FILTERED (the host might be filtering or just not responding
    // to our probe). We never use it for TCP.
    PSTATE_OPEN_OR_FILT = 2,
};

#define PORTSCAN_MAX_RESULTS 256
#define PORTSCAN_BANNER_MAX  96

struct PortScanResult {
    uint16_t  port;
    uint8_t   state;   // PortState
    uint8_t   is_udp;  // 0 = TCP/banner result, 1 = UDP result
    char      banner[PORTSCAN_BANNER_MAX];  // empty unless banner-grab mode
};

// Kicks off a scan against `ip_host_order` with the given technique +
// preset. For PSPRE_CUSTOM, `lo`/`hi` define the inclusive port range
// (1..65535, lo<=hi). Returns false if a scan is already in progress, the
// WiFi isn't associated, or the args are out of range.
bool portscan_start(uint32_t ip_host_order, PortScanTech tech,
                    PortScanPreset preset, uint16_t lo, uint16_t hi);

void portscan_stop();
bool portscan_is_running();
bool portscan_just_finished();    // true once after each scan completes

int  portscan_scanned();
int  portscan_total();
int  portscan_result_count();
const PortScanResult *portscan_result(int idx);  // 0 = first added
void portscan_clear_results();

uint32_t       portscan_target_ip();
PortScanTech   portscan_tech();
PortScanPreset portscan_preset();
uint16_t       portscan_custom_lo();
uint16_t       portscan_custom_hi();

// Writes a per-scan log line set to /PingSweeps/portscan_<ip>_<ts>.txt
// once the scan finishes. Call from loop(). No-op during a scan.
void portscan_poll();
