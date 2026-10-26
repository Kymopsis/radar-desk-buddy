/**
 * Radar Desk Buddy
 *   Level 1: radar follow + eyes + figure, local time and weather (net_time).
 *   Level 2: Telegram bot messages on the screen (telegram_link), no server needed.
 *   Level 3: optional self-hosted server over MQTT (mqtt_link), idle unless configured.
 */

#include "board_pins.h"
#include "device_cfg.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motion.h"
#include "mqtt_link.h"
#include "net_time.h"
#include "radar_link.h"
#include "servo.h"
#include "softap_portal.h"
#include "telegram_link.h"
#include "ui.h"

static const char *TAG = "app";

static ui_lang_t lang_from_cfg(const char *s)
{
    if (!s || !s[0]) {
        return UI_LANG_EN;
    }
    if (!strncmp(s, "zh-CN", 5) || !strncmp(s, "zh_CN", 5)) {
        return UI_LANG_ZH_CN;
    }
    if (!strncmp(s, "zh-TW", 5) || !strncmp(s, "zh_TW", 5)) {
        return UI_LANG_ZH_TW;
    }
    if (!strncmp(s, "ja", 2)) {
        return UI_LANG_JA;
    }
    if (!strncmp(s, "ko", 2)) {
        return UI_LANG_KO;
    }
    if (!strncmp(s, "es", 2)) {
        return UI_LANG_ES;
    }
    if (!strncmp(s, "fr", 2)) {
        return UI_LANG_FR;
    }
    if (!strncmp(s, "de", 2)) {
        return UI_LANG_DE;
    }
    if (!strncmp(s, "pt", 2)) {
        return UI_LANG_PT;
    }
    if (!strncmp(s, "ru", 2)) {
        return UI_LANG_RU;
    }
    return UI_LANG_EN;
}

static void touch_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PIN_TTP223,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

static void apply_cfg_to_ui(const device_cfg_t *cfg)
{
    ui_set_lang(lang_from_cfg(cfg->lang));
    if (cfg->gender[0] == 'm' || cfg->gender[0] == 'M') {
        ui_set_gender(UI_GENDER_MALE);
    } else if (cfg->gender[0]) {
        ui_set_gender(UI_GENDER_FEMALE);
    }
    if (cfg->screen_flip) {
        display_set_rotate_180(true);
    }
    /* SCREEN: NVS has home Wi‑Fi → leave PROV (claim not required). */
    ui_set_provisioned(cfg->wifi_saved && cfg->wifi_ssid[0] != '\0');
}

static void on_msg_closed(void)
{
    mqtt_link_notify_msg_displayed();
    telegram_link_notify_msg_displayed();
}

static void ui_task(void *arg)
{
    (void)arg;
    bool prev_touch = false;
    int64_t last_edge_ms = 0;

    /* No demo peer/unread/weather — net_time / telegram / mqtt fill these in. */
    ui_set_peer(UI_PEER_OFFLINE, NULL, 0, false);
    ui_set_unread(0);
    ui_set_clock(false, 0, 0);

    while (1) {
        int64_t now = esp_timer_get_time() / 1000;

        radar_target_t tgt;
        if (radar_link_get_target(&tgt) == ESP_OK) {
            ui_set_radar_link(tgt.from_uart, now);
            if (tgt.from_uart) {
                bool dist_ok = tgt.uart_present && tgt.distance_m >= 0.05f;
                ui_feed_radar(tgt.present, tgt.distance_m, dist_ok, now);
            } else {
                ui_feed_radar(false, 0.0f, false, now);
            }
            /* Eyes ↔ pan: only when follow ON; off → gaze locked center. */
            const bool follow = motion_follow_enabled();
            ui_set_follow_active(follow);
            if (follow) {
                ui_set_pan_deg(servo_get_pan_deg());
                ui_set_gaze_az(servo_get_pan_deg(), true);
            } else {
                ui_set_pan_deg(0.0f);
                ui_set_gaze_az(0.0f, false);
            }
        } else {
            ui_set_follow_active(motion_follow_enabled());
        }

        ui_set_keep_awake(softap_portal_sta_count() > 0);

        bool touched = gpio_get_level(BOARD_PIN_TTP223) == 1;
        if (touched && !prev_touch && (now - last_edge_ms) > 250) {
            last_edge_ms = now;
            if (ui_on_touch(now)) {
                mqtt_link_notify_touch();
            }
        }
        prev_touch = touched;

        ui_tick(now);
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "radar-desk-buddy: UI + SoftAP + radar follow + time/weather + telegram");
    ESP_ERROR_CHECK(display_init());
    touch_init();
    ESP_ERROR_CHECK(ui_init());

    device_cfg_t cfg;
    bool have_cfg = device_cfg_load(&cfg) == ESP_OK;
    if (have_cfg) {
        apply_cfg_to_ui(&cfg);
    }

    ESP_ERROR_CHECK(servo_init());
    ESP_ERROR_CHECK(radar_link_init());
    ESP_ERROR_CHECK(motion_init());
    if (have_cfg) {
        ui_set_distance_bands(cfg.near_cm / 100.0f, cfg.near_exit_cm / 100.0f, cfg.far_cm / 100.0f,
                              (uint32_t)cfg.hide_s * 1000u);
        motion_set_timing(cfg.rise_ms, (uint32_t)cfg.hide_s * 1000u);
        motion_set_pan_limits((float)cfg.pan_slew_dps, (float)cfg.pan_max_deg);
        motion_set_far_max_m(cfg.far_cm / 100.0f);
    }
    ui_set_follow_active(motion_follow_enabled());
    ESP_ERROR_CHECK(softap_portal_start());
    ESP_ERROR_CHECK(net_time_start());
    ESP_ERROR_CHECK(telegram_link_start());
    ESP_ERROR_CHECK(mqtt_link_start());
    ui_set_msg_closed_cb(on_msg_closed);
    ESP_ERROR_CHECK(ui_set_wifi_qr(softap_portal_ssid(), softap_portal_pass()));
    ESP_LOGI(TAG, "SoftAP %s → http://%s/", softap_portal_ssid(), softap_portal_host());

    xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
}
