#pragma once

/*
 * Stateless-from-the-caller TinyFrame decoder used by radar_link and host tests.
 * Wire header: SOF(0x01), ID(u16 BE), LEN(u16 BE), TYPE(u16 BE),
 * ~xor(header), payload, ~xor(payload). Payload scalars are little-endian.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADAR_TF_SOF          0x01u
#define RADAR_TF_TYPE_TARGET  0x0A04u
#define RADAR_TF_TYPE_CLOUD   0x0A08u
#define RADAR_TF_PAYLOAD_MAX  4096u
#define RADAR_MAX_BLIPS       8

typedef enum {
    RADAR_WIRE_UNKNOWN = 0,
    RADAR_WIRE_2D,      /* Andar 2D: x,y,dop,id — 16 B/item */
    RADAR_WIRE_3D,      /* Andar 3D: x,y,z,id,power,speed — 24 B/item */
    RADAR_WIRE_HLK,     /* HLK stock: x,y,z,dop_idx,cluster_id — 20 B/item */
} radar_wire_mode_t;

typedef struct {
    float x_m;
    float y_m;
    float z_m;
    float azimuth_deg;
    float distance_m;
    int32_t track_id;
} radar_wire_blip_t;

/*
 * Module frame (HLK-LD6004 方向标识 / Andar 3D host tools):
 *   +X = left along board long axis
 *   +Y = forward (out of antenna face)
 *   +Z = up
 * Azimuth for pan: atan2(x, y) deg — 0 = +Y forward, + = left (+X).
 * TinyFrame envelope is shared; payload stride differs by firmware.
 */
typedef struct {
    bool present;
    bool azimuth_ok;            /* true only when y is large enough for pan */
    bool speed_mps_valid;       /* false for 2D: Doppler has no public m/s scale */
    float x_m;
    float y_m;
    float z_m;
    float azimuth_deg;          /* atan2(x, y), forward is +Y */
    float elevation_deg;
    float distance_m;
    float radial_speed_mps;     /* valid only when speed_mps_valid */
    int32_t doppler_raw;        /* 2D / HLK Doppler field */
    int32_t track_id;           /* cluster/id; -1 if unknown */
    int target_count;
    int blip_count;
    radar_wire_blip_t blips[RADAR_MAX_BLIPS];
    radar_wire_mode_t mode;
} radar_wire_sample_t;

typedef struct {
    bool complete;              /* a complete, checksum-valid TinyFrame */
    uint16_t type;
    radar_wire_mode_t mode;     /* detected from 0x0A04 / 0x0A08 payload */
    bool has_target_sample;     /* target frame was structurally valid */
    radar_wire_sample_t sample;
} radar_wire_event_t;

typedef enum {
    RADAR_TF_WAIT_SOF = 0,
    RADAR_TF_READ_ID,
    RADAR_TF_READ_LEN,
    RADAR_TF_READ_TYPE,
    RADAR_TF_READ_HEAD_CKSUM,
    RADAR_TF_READ_PAYLOAD,
    RADAR_TF_READ_PAYLOAD_CKSUM,
} radar_tf_state_t;

typedef struct {
    radar_tf_state_t state;
    uint8_t header[7];
    uint8_t field[2];
    uint8_t payload[RADAR_TF_PAYLOAD_MAX];
    uint16_t payload_len;
    uint16_t payload_used;
    uint16_t type;
    uint8_t field_used;
    radar_wire_mode_t latched_mode;
    int32_t latched_track_id; /* sticky primary across frames; -1 = none */
    uint8_t track_misses;
} radar_tf_decoder_t;

void radar_tf_decoder_init(radar_tf_decoder_t *decoder);

/* Returns true only once per complete, checksum-valid frame. */
bool radar_tf_decoder_feed(radar_tf_decoder_t *decoder, uint8_t byte,
                           radar_wire_event_t *event);

