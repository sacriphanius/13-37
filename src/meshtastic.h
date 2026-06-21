#pragma once
#include <stdint.h>
#include <stddef.h>

#define MESH_MAX_MESSAGES 20
#define MESH_MAX_TEXT_LEN 200

struct MeshMessage {
    uint32_t from_node;
    uint32_t dest_node;     // OTA dest field. 0xFFFFFFFF = broadcast (channel
                            // chatter); our own node id = DM addressed to us.
    char     text[MESH_MAX_TEXT_LEN];
    char     time_str[6];   // "HH:MM\0"
    // Link-quality stats captured at RX time. rssi/snr in dBm/dB straight
    // from the SX1262 packet stats; hop_start/hop_limit from byte 12 of
    // the OTA header (bits 5-7 and 0-2 respectively). Hops traversed =
    // hop_start - hop_limit. NaN rssi means the packet didn't go through
    // the normal RX path (only TEXT packets get stats today).
    float    rssi;
    float    snr;
    uint8_t  hop_start;
    uint8_t  hop_limit;
};

void meshtastic_set_active(bool active);
void meshtastic_bg_tick();

// Queues a broadcast text message for transmission on the next bg tick.
// Returns false if the LoRa radio is not active (nothing queued).
bool meshtastic_send_text(const char *text);

// DM variant: target a specific node ID instead of broadcasting. The
// recipient's Meshtastic UI shows the message as a direct message from
// us. dest_node = 0xFFFFFFFF is equivalent to broadcast (meshtastic_send_text).
bool meshtastic_send_text_to(const char *text, uint32_t dest_node);

// Traceroute. Fires a Routing-app request at dest_node with an empty
// RouteDiscovery body and want_response=1. The target (and any relays
// on the way back) populate the route_reply we receive; their node
// IDs land in MeshTraceroute::hops. Tracks a single in-flight request
// at a time - calling again before the previous reply arrives resets
// the slot to point at the new request.
struct MeshTraceroute {
    uint32_t target_node;
    uint32_t request_id;
    uint32_t sent_ms;
    bool     has_response;
    uint32_t response_ms;
    uint8_t  hop_count;
    uint32_t hops[8];
};
bool                 meshtastic_send_traceroute(uint32_t dest_node);
bool                 meshtastic_traceroute_in_flight();
const MeshTraceroute *meshtastic_get_last_traceroute();

// An outbound TEXT we sent with want_ack on. State machine:
//   PENDING   - TX'd, waiting for a Routing ACK from the recipient
//   DELIVERED - Routing app reported error_reason = NONE (recipient got it)
//   NO_ACK    - local 30 s timeout fired, no response at all
//   NAKED     - Routing app reported a non-NONE error_reason; the
//               specific code is in error_reason
#define MESH_MAX_OUTGOING 6
struct MeshOutgoing {
    uint32_t packet_id;
    char     text[MESH_MAX_TEXT_LEN];
    uint32_t sent_ms;
    uint32_t dest_node;
    enum State { PENDING = 0, DELIVERED = 1, NO_ACK = 2, NAKED = 3 } state;
    uint8_t  error_reason;
};
int                  meshtastic_get_outgoing_count();
const MeshOutgoing  *meshtastic_get_outgoing(int idx);     // 0 = newest

// Human-readable Routing.Error enum (the value carried in error_reason
// when a NAK comes back). Unknown codes return "Err <n>".
const char          *meshtastic_routing_error_name(uint8_t reason);

// Multi-channel support. Slot 0 is always the public LongFast channel
// (hardcoded name + PSK, can be enabled/disabled but not edited).
// Slots 1..3 are user-defined - enabling an empty slot auto-generates
// a random AES-128 PSK + a default name; PSK + name can be edited by
// hand in /Meshtastic/channels.txt on the SD card. set_active_channel
// picks which slot outgoing TX uses; the RX decoder walks every
// enabled slot looking for a channel_hash match in the OTA header.
#define MESH_MAX_CHANNELS 4
struct MeshChannel {
    char    name[16];
    uint8_t psk[32];
    uint8_t psk_len;       // 0 = empty slot
    uint8_t channel_hash;  // cached XOR(name bytes) ^ XOR(psk bytes)
    bool    enabled;
};
const MeshChannel *meshtastic_get_channel(int idx);
void               meshtastic_set_channel_enabled(int idx, bool on);
int                meshtastic_get_active_channel();
void               meshtastic_set_active_channel(int idx);

// Read /Meshtastic/channels.txt at boot and rewrite it on every
// channel state change. Called from main.cpp setup() (load) and from
// the channels screen (save).
void               meshtastic_load_channels_from_sd();
void               meshtastic_save_channels_to_sd();

// Position request. Sends an empty Position-app packet with
// want_response=1 addressed at dest_node. The recipient (if it's
// running this firmware or stock Meshtastic) replies with its current
// Position; that reply lands in the normal POSITION RX path and
// updates the node's MeshNode entry. Returns false if LoRa is off or
// dest is the broadcast address (we never spam-request).
bool                 meshtastic_request_position(uint32_t dest_node);

#define MESH_MAX_LONG_NAME  40
#define MESH_MAX_SHORT_NAME  5   // Meshtastic short_name is <= 4 chars + NUL

// Node identity advertised in NodeInfo broadcasts. Defaults: long
// "T-Watch Ultra", short "1337". Not persisted across reboots.
void        meshtastic_set_long_name(const char *name);
void        meshtastic_set_short_name(const char *name);
const char *meshtastic_get_long_name();
const char *meshtastic_get_short_name();

// Periodic NodeInfo announce control. When `on` is true the bg_tick()
// blasts a NodeInfo (and a Telemetry broadcast on the same cadence)
// every `interval_ms`. Defaults: on=true, interval_ms=600000 (10 min)
// so a freshly booted watch shows up in nearby Meshtastic node lists
// without the user having to flip anything. Toggling off->on while
// the radio is active reseeds the timer to fire ~5 s later instead of
// waiting up to a full interval.
void     meshtastic_set_announce(bool on, uint32_t interval_ms);
bool     meshtastic_announce_enabled();
uint32_t meshtastic_announce_interval_ms();

#define MESH_MAX_NODES 20

// One direct radio neighbour as reported by a NEIGHBORINFO_APP (portnum
// 71) broadcast. Each entry pairs a node ID with its measured SNR (dB).
struct MeshNeighbor {
    uint32_t node_id;
    float    snr;
};
#define MESH_MAX_NEIGHBORS  8

// A node heard advertising itself via a NodeInfo broadcast.
// Mirrors the Meshtastic User protobuf (fields 1-8). Each optional field
// has a presence flag so the UI can omit fields the node did not send.
struct MeshNode {
    uint32_t node_id;
    char     id[16];                          // User.id         (field 1) "!433d3c08"
    char     long_name[MESH_MAX_LONG_NAME];   // User.long_name  (field 2)
    char     short_name[MESH_MAX_SHORT_NAME]; // User.short_name (field 3)
    uint8_t  macaddr[6];                      // User.macaddr    (field 4)
    bool     has_macaddr;
    uint32_t hw_model;                        // User.hw_model   (field 5)
    bool     has_hw_model;
    bool     is_licensed;                     // User.is_licensed(field 6)
    bool     has_is_licensed;
    uint32_t role;                            // User.role       (field 7)
    bool     has_role;
    uint8_t  public_key[32];                  // User.public_key (field 8)
    uint8_t  public_key_len;
    // Position (from a separate POSITION_APP packet, portnum 3).
    // lat/lon are degrees * 1e7, as sent on the wire.
    int32_t  latitude_i;
    int32_t  longitude_i;
    bool     has_position;
    int32_t  altitude;                        // metres
    bool     has_altitude;
    char     time_str[6];                     // "HH:MM" — when last heard
    char     disco_ts[16];                    // "YYYYMMDD_HHMMSS" — first heard

    // Telemetry (TELEMETRY_APP, portnum 67). DeviceMetrics is what
    // most nodes broadcast; EnvironmentMetrics is for nodes with
    // sensors (BME280 etc); PowerMetrics is for solar / multi-rail
    // nodes. Each subfield is independently optional - render only
    // what the node actually sent.
    bool     has_telemetry;
    uint32_t battery_level;                   bool has_battery_level;  // 0..100 %
    float    voltage;                         bool has_voltage;        // volts
    uint32_t uptime_seconds;                  bool has_uptime;

    bool     has_environment;
    float    temperature_c;                   bool has_temperature;
    float    relative_humidity;               bool has_humidity;
    float    pressure_hpa;                    bool has_pressure;
    float    lux;                             bool has_lux;
    uint32_t iaq;                             bool has_iaq;

    bool     has_power_metrics;
    float    ch1_voltage;                     bool has_ch1_voltage;
    float    ch1_current;                     bool has_ch1_current;

    // NeighborInfo (portnum 71). Up to MESH_MAX_NEIGHBORS direct radio
    // neighbours with their measured SNR. Sender ID + interval fields
    // are not stored - we only care about the neighbor list for the
    // node detail card.
    uint8_t       neighbor_count;
    MeshNeighbor  neighbors[MESH_MAX_NEIGHBORS];
    bool          has_neighborinfo;
};

// Human-readable Meshtastic device role (Config.DeviceConfig.Role).
const char *meshtastic_role_name(uint32_t role);

int             meshtastic_get_node_count();
const MeshNode *meshtastic_get_node(int idx); // 0 = most recently heard

int  meshtastic_get_count();      // messages currently in ring buffer (for display)
int  meshtastic_get_rx_count();   // total RF packets successfully received
int  meshtastic_get_unread();
void meshtastic_mark_read();

const MeshMessage *meshtastic_get_message(int idx); // 0 = newest

// Wipe the in-memory message ring (and unread counter). Permanent SD
// archive at /Meshtastic/Messages/<timestamp>.txt is left intact -
// these calls only affect the on-screen list. Receiver keeps running.
void meshtastic_clear_messages();

// Remove a single message from the ring. idx is 0 = newest, matching
// meshtastic_get_message() indexing. Same separation from the SD
// archive: the per-message file on disk is left in place. Returns
// false on out-of-range idx.
bool meshtastic_delete_message(int idx);

float    meshtastic_get_rssi();
bool     meshtastic_get_cad();
bool     meshtastic_is_active();
uint32_t meshtastic_get_node_id();  // derived from ESP32 base MAC

// Debug counters — shown on Meshtastic screen and via Serial
struct MeshDebugInfo {
    int      isr_count;       // radio IRQ fires (should go up when RF traffic exists)
    int      rx_ok;           // readData returned RADIOLIB_ERR_NONE
    int      rx_fail;         // readData error or bad pkt_len
    int16_t  rx_last_rc;      // last readData return code (0 = OK)
    int      rx_last_pkt_len; // last getPacketLength() value
    int      parse_ok;        // protobuf decoded a TEXT message
    int      nodeinfo_tx;     // NodeInfo broadcast count
    uint32_t last_portnum;    // portnum from last decoded packet (1=TEXT 3=POS 4=NODEINFO)
    uint8_t  last_plain0;     // plain[0] of last packet (0x08 = valid protobuf start)
    uint8_t  last_plain1;     // plain[1] = portnum value if plain[0]==0x08
};
const MeshDebugInfo *meshtastic_get_debug();
