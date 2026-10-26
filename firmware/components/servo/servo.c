#include "servo.h"

#include <math.h>

#include "board_pins.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "servo";

/* LEDC: display uses TIMER_0 / CH0 — keep clear. */
#define SERVO_TIMER     LEDC_TIMER_1
#define SERVO_MODE      LEDC_LOW_SPEED_MODE
#define SERVO_CH_PAN    LEDC_CHANNEL_1
#define SERVO_CH_LIFT   LEDC_CHANNEL_2
#define SERVO_FREQ_HZ   50
#define SERVO_RES       LEDC_TIMER_14_BIT
#define SERVO_PERIOD_US 20000

static int s_pan_center_us = 1500;
static int s_pan_span_us = 1000;
static int s_lift_down_us = 1000;
static int s_lift_up_us = 2000;

static float s_pan_deg;
static float s_lift_pct;
static bool s_attached;
static bool s_inited;
static bool s_pan_driven;
static bool s_lift_driven;

static uint32_t us_to_duty(int us)
{
    if (us < 400) {
        us = 400;
    }
    if (us > 2600) {
        us = 2600;
    }
    return (uint32_t)((uint64_t)us * ((1u << 14) - 1) / SERVO_PERIOD_US);
}

static int pan_deg_to_us(float deg)
{
    if (deg > 90.0f) {
        deg = 90.0f;
    }
    if (deg < -90.0f) {
        deg = -90.0f;
    }
    return s_pan_center_us + (int)lroundf(deg / 90.0f * (float)s_pan_span_us);
}

static int lift_pct_to_us(float pct)
{
    if (pct > 100.0f) {
        pct = 100.0f;
    }
    if (pct < 0.0f) {
        pct = 0.0f;
    }
    return s_lift_down_us + (int)lroundf(pct / 100.0f * (float)(s_lift_up_us - s_lift_down_us));
}

/* After ledc_stop(), set_duty alone is unreliable on IDF 5/6 — rebind channel. */
static esp_err_t rebind_channel(ledc_channel_t ch, int gpio)
{
    ledc_channel_config_t cfg = {
        .gpio_num = gpio,
        .speed_mode = SERVO_MODE,
        .channel = ch,
        .timer_sel = SERVO_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    return ledc_channel_config(&cfg);
}

static esp_err_t write_pulse(ledc_channel_t ch, int gpio, int us, bool *driven_flag)
{
    if (!*driven_flag) {
        ESP_RETURN_ON_ERROR(rebind_channel(ch, gpio), TAG, "rebind");
    }
    ESP_RETURN_ON_ERROR(ledc_set_duty(SERVO_MODE, ch, us_to_duty(us)), TAG, "duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(SERVO_MODE, ch), TAG, "update");
    *driven_flag = true;
    s_attached = true;
    return ESP_OK;
}

static esp_err_t drive_pan(void)
{
    return write_pulse(SERVO_CH_PAN, BOARD_PIN_SERVO_PAN, pan_deg_to_us(s_pan_deg), &s_pan_driven);
}

static esp_err_t drive_lift(void)
{
    return write_pulse(SERVO_CH_LIFT, BOARD_PIN_SERVO_LIFT, lift_pct_to_us(s_lift_pct),
                       &s_lift_driven);
}

static esp_err_t apply_outputs(void)
{
    ESP_RETURN_ON_ERROR(drive_pan(), TAG, "pan");
    ESP_RETURN_ON_ERROR(drive_lift(), TAG, "lift");
    return ESP_OK;
}

esp_err_t servo_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    ledc_timer_config_t tmr = {
        .speed_mode = SERVO_MODE,
        .duty_resolution = SERVO_RES,
        .timer_num = SERVO_TIMER,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tmr), TAG, "timer");

    s_pan_deg = 0.0f;
    s_lift_pct = 0.0f;
    s_pan_driven = false;
    s_lift_driven = false;
    s_attached = false;
    s_inited = true;

    ESP_LOGI(TAG, "soft-home pan=0 lift=0");
    ESP_RETURN_ON_ERROR(servo_home(1200), TAG, "home");
    return ESP_OK;
}

esp_err_t servo_home(uint32_t duration_ms)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    float p0 = s_pan_deg;
    float l0 = s_lift_pct;
    const int steps = (duration_ms < 100) ? 1 : (int)(duration_ms / 20);
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        s_pan_deg = p0 + (0.0f - p0) * t;
        s_lift_pct = l0 + (0.0f - l0) * t;
        ESP_RETURN_ON_ERROR(apply_outputs(), TAG, "ramp");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_pan_deg = 0.0f;
    s_lift_pct = 0.0f;
    ESP_RETURN_ON_ERROR(apply_outputs(), TAG, "end");
    vTaskDelay(pdMS_TO_TICKS(200));
    /* Keep holding center — do not ledc_stop (invalid pulse → twitch / SoftAP dead). */
    return ESP_OK;
}

esp_err_t servo_set_pan_deg(float deg)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (deg > 90.0f) {
        deg = 90.0f;
    }
    if (deg < -90.0f) {
        deg = -90.0f;
    }
    s_pan_deg = deg;
    return drive_pan();
}

esp_err_t servo_set_lift_pct(float pct)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pct > 100.0f) {
        pct = 100.0f;
    }
    if (pct < 0.0f) {
        pct = 0.0f;
    }
    s_lift_pct = pct;
    return drive_lift();
}

float servo_get_pan_deg(void)
{
    return s_pan_deg;
}

float servo_get_lift_pct(void)
{
    return s_lift_pct;
}

esp_err_t servo_jog_pan(float delta_deg)
{
    return servo_set_pan_deg(s_pan_deg + delta_deg);
}

esp_err_t servo_jog_lift(float delta_pct)
{
    return servo_set_lift_pct(s_lift_pct + delta_pct);
}

esp_err_t servo_attach(bool on)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on) {
        return apply_outputs();
    }
    /*
     * Soft-detach: keep last valid pulse (friction hold) instead of ledc_stop.
     * Stopping forces GPIO idle level → SG90 sees no 50Hz frame and twitches;
     * restarting after stop was also failing SoftAP jog on IDF 6.
     */
    s_attached = false;
    return ESP_OK;
}

esp_err_t servo_detach_pan(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Keep pan holding last angle; mark logical idle only. */
    s_attached = s_lift_driven;
    return ESP_OK;
}

bool servo_attached(void)
{
    return s_pan_driven || s_lift_driven;
}

esp_err_t servo_set_pulse_us(int pan_center, int pan_span, int lift_down, int lift_up)
{
    if (pan_span < 200 || lift_up <= lift_down) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pan_center_us = pan_center;
    s_pan_span_us = pan_span;
    s_lift_down_us = lift_down;
    s_lift_up_us = lift_up;
    return apply_outputs();
}
