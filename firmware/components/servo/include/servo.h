#pragma once

/**
 * Dual SG90: pan (GPIO5, ±90°) + lift (GPIO7, 0–100%).
 * Soft-home on init; idle can detach PWM (SCHEME: 静止断 PWM).
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t servo_init(void);

/** Soft-home both axes to safe defaults (pan 0°, lift 0%). Blocking ramp. */
esp_err_t servo_home(uint32_t duration_ms);

esp_err_t servo_set_pan_deg(float deg);   /* clamped ±90, 0 = forward */
esp_err_t servo_set_lift_pct(float pct);  /* clamped 0–100 */

float servo_get_pan_deg(void);
float servo_get_lift_pct(void);

/** Relative jog for SoftAP bring-up. */
esp_err_t servo_jog_pan(float delta_deg);
esp_err_t servo_jog_lift(float delta_pct);

/** Enable/disable PWM (detach = coast / friction hold). */
esp_err_t servo_attach(bool on);
bool servo_attached(void);

/** Stop only pan PWM; leave lift holding its last pulse (or already idle). */
esp_err_t servo_detach_pan(void);

/**
 * Calibration pulse widths (µs). Defaults suit typical SG90;
 * SoftAP can tweak before locking into enclosure.
 */
esp_err_t servo_set_pulse_us(int pan_center, int pan_span, int lift_down, int lift_up);

#ifdef __cplusplus
}
#endif
