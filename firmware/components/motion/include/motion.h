#pragma once

/**
 * Local presence → lift; pan smoothly follows nearest 0x0A04 target (slew-limited).
 * Follow is ON at boot (eyes + toy). SoftAP / MQTT can stop it for this
 * power cycle only — not saved to NVS. Jog/home only pauses follow.
 * Hide order: pan→0 then lift→0.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOTION_HIDDEN = 0,
    MOTION_RISING,
    MOTION_TRACKING,
    MOTION_HIDING,
} motion_phase_t;

esp_err_t motion_init(void);

/** SoftAP jog: pause auto follow so manual moves are not overwritten. */
void motion_pause_auto(uint32_t duration_ms);

/** Clear SoftAP pause so follow can run immediately. */
void motion_clear_pause(void);

/** SoftAP moved pan — keep motion cmd in sync so follow won't yank back. */
void motion_sync_pan_cmd(float deg);

/**
 * SoftAP home / jog took the servos — sync phase to real lift so a later
 * follow enable can rise again (avoids stuck TRACKING with lift at 0).
 */
void motion_notify_manual(void);

/**
 * SoftAP takes servos now: follow OFF, phase HIDDEN, no auto-hide race.
 * Call before blocking servo_home / jog so motion_task will not also park.
 */
void motion_claim_servos(void);

/**
 * Radar auto follow (lift+pan + screen eyes). ON at every boot; session-only.
 * false = SoftAP stop: motion_task slowly homes if raised.
 * Cloud takeover (keep pose) uses motion_claim_servos().
 */
void motion_set_follow_enabled(bool on);
bool motion_follow_enabled(void);

/** SoftAP live tune: present confirm + hide delay. */
void motion_set_timing(uint32_t enter_ms, uint32_t leave_ms);
void motion_get_timing(uint32_t *enter_ms, uint32_t *leave_ms);

/** SoftAP live tune: pan slew and ±soft limit. */
void motion_set_pan_limits(float slew_dps, float soft_max_deg);
void motion_get_pan_limits(float *slew_dps, float *soft_max_deg);

/** Beyond this range, treat UART target as gone (aligns with screen FAR max). */
void motion_set_far_max_m(float metres);
float motion_get_far_max_m(void);

/**
 * Cloud / web jog. Does not turn follow off: auto is paused ~12s then resumes.
 * Hidden + non-zero pan: rise first, then yaw (do not scrape the hatch).
 */
esp_err_t motion_cloud_jog(float lift_pct, float pan_deg);

/**
 * Cloud preset: rise / hide / peek / look_around / wave.
 * Same pause-then-resume rule as jog. repeat is clamped 1–5.
 */
esp_err_t motion_cloud_preset(const char *name, int repeat);

motion_phase_t motion_phase(void);
const char *motion_phase_name(void);
float motion_cmd_pan_deg(void);

void motion_set_pan_sign(int sign);
int motion_get_pan_sign(void);

#ifdef __cplusplus
}
#endif
