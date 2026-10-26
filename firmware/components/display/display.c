#include "display.h"

#include <string.h>
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static bool s_ready;
static bool s_rotate_180;

static esp_err_t apply_mirror(void)
{
    if (!s_ready || !s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Default bring-up: mirror(false, true). 180° = invert both axes. */
    const bool mx = s_rotate_180;
    const bool my = !s_rotate_180;
    return esp_lcd_panel_mirror(s_panel, mx, my);
}

esp_err_t display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* Backlight via LEDC — dimming path for "sleep", not hard off only */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "bl timer");

    ledc_channel_config_t bl_ch = {
        .gpio_num = BOARD_PIN_LCD_BLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_ch), TAG, "bl channel");

    spi_bus_config_t buscfg = {
        .sclk_io_num = BOARD_PIN_LCD_SCLK,
        .mosi_io_num = BOARD_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_PIN_LCD_CS,
        .dc_gpio_num = BOARD_PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                                 &io_cfg, &io),
                        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel), TAG, "gc9a01");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    s_ready = true;
    s_rotate_180 = false;
    ESP_RETURN_ON_ERROR(apply_mirror(), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    ESP_RETURN_ON_ERROR(display_set_backlight(80), TAG, "bl on");
    ESP_LOGI(TAG, "GC9A01 ready %dx%d @ GPIO CS%d DC%d RST%d BLK%d",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             BOARD_PIN_LCD_CS, BOARD_PIN_LCD_DC, BOARD_PIN_LCD_RST, BOARD_PIN_LCD_BLK);
    return ESP_OK;
}

esp_err_t display_set_rotate_180(bool rotate_180)
{
    s_rotate_180 = rotate_180;
    if (!s_ready) {
        return ESP_OK;
    }
    esp_err_t err = apply_mirror();
    ESP_LOGI(TAG, "screen rotate_180=%d", rotate_180 ? 1 : 0);
    return err;
}

bool display_get_rotate_180(void)
{
    return s_rotate_180;
}

esp_err_t display_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (1023U * percent) / 100U;
    if (BOARD_LCD_BLK_ON_LEVEL == 0) {
        duty = 1023U - duty;
    }
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "update");
    return ESP_OK;
}

esp_err_t display_flush(const uint16_t *rgb565, int x, int y, int w, int h)
{
    if (!s_ready || !rgb565 || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, rgb565);
}

esp_err_t display_fill(uint16_t color)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Line buffer — avoids a full-frame stack alloc */
    uint16_t line[BOARD_LCD_H_RES];
    for (int i = 0; i < BOARD_LCD_H_RES; ++i) {
        line[i] = color;
    }
    for (int y = 0; y < BOARD_LCD_V_RES; ++y) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, y, BOARD_LCD_H_RES, y + 1, line),
                            TAG, "fill line");
    }
    return ESP_OK;
}
