#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define UI_FONT16_H 16
#define UI_FONT16_W 16
const uint8_t *ui_font16_bits(uint32_t cp);
int ui_font16_advance(uint32_t cp);
#ifdef __cplusplus
}
#endif
