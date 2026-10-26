#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_init(void);
esp_err_t display_set_backlight(uint8_t percent); /* 0–100 */
/** Enclosure mount: false = factory default; true = rotate panel 180°. */
esp_err_t display_set_rotate_180(bool rotate_180);
bool display_get_rotate_180(void);
esp_err_t display_flush(const uint16_t *rgb565, int x, int y, int w, int h);
esp_err_t display_fill(uint16_t color);

#ifdef __cplusplus
}
#endif
