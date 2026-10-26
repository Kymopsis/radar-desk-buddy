#pragma once

/**
 * GPIO map — matches docs/wiring.md.
 * Do NOT remap. GPIO35/36/37 are Octal-PSRAM on N16R8 — unusable.
 */

#include "driver/gpio.h"

/* Round LCD GC9A01 (SPI2 / FSPI default pins) */
#define BOARD_LCD_SPI_HOST   SPI2_HOST
#define BOARD_PIN_LCD_DC     GPIO_NUM_9
#define BOARD_PIN_LCD_CS     GPIO_NUM_10
#define BOARD_PIN_LCD_MOSI   GPIO_NUM_11
#define BOARD_PIN_LCD_SCLK   GPIO_NUM_12
#define BOARD_PIN_LCD_RST    GPIO_NUM_13
#define BOARD_PIN_LCD_BLK    GPIO_NUM_14

#define BOARD_LCD_H_RES      240
#define BOARD_LCD_V_RES      240
#define BOARD_LCD_PCLK_HZ    (40 * 1000 * 1000)

/* Most GC9A01 round modules: BLK active-high. Override if silk says otherwise. */
#define BOARD_LCD_BLK_ON_LEVEL  1

/* Servos — both mounted in the base */
#define BOARD_PIN_SERVO_PAN  GPIO_NUM_5  /* horizontal follow, 60G only */
#define BOARD_PIN_SERVO_LIFT GPIO_NUM_7  /* rise / hide, all SKUs */

/* Touch */
#define BOARD_PIN_TTP223     GPIO_NUM_8

/* Radar UART (step 60G+) */
#define BOARD_PIN_RADAR_RST  GPIO_NUM_4
/* LD6004 pin2 OUT/TX2: stock FW = presence level (high=someone in zone).
 * Used as lift gate; do NOT also use as UART RX. */
#define BOARD_PIN_RADAR_TX2       GPIO_NUM_16
#define BOARD_PIN_RADAR_PRESENCE  BOARD_PIN_RADAR_TX2
#define BOARD_PIN_RADAR_TXD  GPIO_NUM_17  /* MCU TX → module RX0 */
#define BOARD_PIN_RADAR_RXD  GPIO_NUM_18  /* MCU RX ← module TX0 */

/* LD2410B-P presence (downgrade SKU only) */
#define BOARD_PIN_LD2410_OUT GPIO_NUM_15

/* On-board RGB status LED */
#define BOARD_PIN_RGB_LED    GPIO_NUM_48
