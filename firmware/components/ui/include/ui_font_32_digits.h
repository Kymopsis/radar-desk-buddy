#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
const uint8_t *ui_font32_bits(uint32_t cp, int *out_w);
int ui_font32_height(void);
#ifdef __cplusplus
}
#endif
