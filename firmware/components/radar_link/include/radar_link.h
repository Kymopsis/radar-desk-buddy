#pragma once

/**
 * radar_link — TinyFrame UART parser for the 60 GHz radar modules.
 * TYPE 0x0A04 = tracked targets (preferred); 0x0A08 = point cloud (optional).
 * Payload stride auto: 16 B 2D firmware, 20 B stock module firmware, 24 B 3D firmware.
 *
 * Module frame (as marked on the module): +X left, +Y forward, +Z up;
 * azimuth_deg = atan2(x, y).
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADAR_FW_UNKNOWN = 0,
    RADAR_FW_2D,  /* 2D firmware — 16 B/item */
    RADAR_FW_3D,  /* 3D firmware — 24 B/item */
    RADAR_FW_STOCK, /* stock module firmware — 20 B/item */
} radar_fw_mode_t;

typedef struct {
    float azimuth_deg; /* atan2(x,y), + = left */
    float distance_m;
    float x_m;
    float y_m;
    int32_t track_id;
} radar_blip_t;

#define RADAR_LINK_MAX_BLIPS 8

typedef struct {
    bool present;            /* lift gate: GPIO OUT debounce, else UART target */
    bool gpio_present;       /* debounced module OUT/TX2 (pin2) */
    bool uart_present;       /* ≥1 valid 0x0A04 target in freshness window */
    bool azimuth_ok;         /* pan-usable: forward hemisphere with enough y */
    float x_m;               /* +X = left */
    float y_m;               /* +Y = forward (antenna) */
    float z_m;               /* +Z = up */
    float azimuth_deg;       /* atan2(x,y) deg, 0 = +Y, + = left */
    float elevation_deg;     /* asin(z/r); 0 on 2D */
    float distance_m;
    float distance_cm;
    float radial_speed_mps;  /* 3D module field; valid only when speed_mps_valid */
    int32_t doppler_raw;     /* 2D / stock-firmware Doppler field; m/s scale uncalibrated */
    int32_t track_id;
    bool speed_mps_valid;
    int target_count;
    int blip_count;
    radar_blip_t blips[RADAR_LINK_MAX_BLIPS];
    radar_fw_mode_t fw_mode;
    int64_t ts_ms;
    bool from_uart;          /* UART link fresh (frames recently accepted) */
} radar_target_t;

esp_err_t radar_link_init(void);

/** Latest fused primary target (closest in range among 0x0A04). */
esp_err_t radar_link_get_target(radar_target_t *out);

radar_fw_mode_t radar_link_fw_mode(void);

/** Frames accepted since boot (debug). */
uint32_t radar_link_frame_count(void);

/** Actual configured UART rate; starts at 115200 and auto-hunts if silent. */
int radar_link_baud_rate(void);

#ifdef __cplusplus
}
#endif
