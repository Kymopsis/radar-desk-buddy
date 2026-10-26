#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Languages selectable at SoftAP provisioning (phone locale + user pick). */
typedef enum {
    UI_LANG_ZH_CN = 0,
    UI_LANG_ZH_TW,
    UI_LANG_EN,
    UI_LANG_JA,
    UI_LANG_KO,
    UI_LANG_ES,
    UI_LANG_FR,
    UI_LANG_DE,
    UI_LANG_PT,
    UI_LANG_RU,
    UI_LANG_COUNT,
} ui_lang_t;

typedef enum {
    UI_GENDER_FEMALE = 0, /* pink series */
    UI_GENDER_MALE,       /* blue series */
} ui_gender_t;

typedef enum {
    UI_PEER_OFFLINE = 0,
    UI_PEER_AWAY,
    UI_PEER_PRESENT,
} ui_peer_status_t;

/** Distance band (hysteresis applied in ui_feed_radar). */
typedef enum {
    UI_ZONE_VOID = 0,
    UI_ZONE_FAR,
    UI_ZONE_NEAR,
} ui_zone_t;

/**
 * Face priority (PROV > MSG > SLEEP > EYES > GREET > INFO).
 * Names match the contract; old SILENT/HOME removed.
 */
typedef enum {
    UI_SCREEN_PROV = 0,
    UI_SCREEN_SLEEP,
    UI_SCREEN_EYES,
    UI_SCREEN_GREET,
    UI_SCREEN_INFO,
    UI_SCREEN_MSG,
    UI_SCREEN_NO_RADAR, /* provisioned but UART silent */
} ui_screen_t;

typedef struct {
    ui_lang_t lang;
    ui_gender_t gender;
    ui_peer_status_t peer;
    ui_zone_t zone;
    bool quiet;             /* cmd mute window: mech off; FAR→SLEEP face */
    bool clock_ok;          /* SNTP done — never draw HH:MM without this */
    int clock_hour;         /* 0–23 when clock_ok */
    int clock_min;
    float pan_deg;          /* current pan for EYES pupils */
    int unread_count;
    int temp_c;
    bool weather_ok;        /* false → INFO skips weather row */
    const char *weather;    /* sunny|cloudy|rain|snow; glyphs not required — UI draws icons */
    const char *msg_text;   /* hot-cache text; media types later */
    bool provisioned;       /* NVS has home Wi‑Fi ssid (claim not required) */
    bool cloud_linked;      /* online (home Wi-Fi up) — screen leaves PROV; no server required */
} ui_model_t;

esp_err_t ui_init(void);
esp_err_t ui_set_model(const ui_model_t *model);
esp_err_t ui_set_lang(ui_lang_t lang);
esp_err_t ui_set_gender(ui_gender_t gender);
esp_err_t ui_set_provisioned(bool provisioned);

/** Online = home Wi-Fi has an IP (set by net_time). False → NEAR shows PROV/QR. */
esp_err_t ui_set_cloud_linked(bool linked);

/**
 * SoftAP / local follow arm. Mechanical follow does not hide PROV while the
 * device is offline (coming close still shows the setup QR).
 */
esp_err_t ui_set_follow_active(bool on);

/** UART frames fresh (`radar_target_t.from_uart`). Boot grace before offline. */
esp_err_t ui_set_radar_link(bool linked, int64_t now_ms);

esp_err_t ui_set_peer(ui_peer_status_t peer, const char *weather, int temp_c, bool weather_ok);
/** Standby weather/temp. Cloud cmd/peer may call this; unpaired devices still show it. */
esp_err_t ui_set_weather(const char *weather, int temp_c, bool weather_ok);
esp_err_t ui_set_unread(int count);
esp_err_t ui_set_msg_text(const char *text);
/** Decode baseline JPEG (≤48KB, typically 240×240) into the MSG hot-cache slot. */
esp_err_t ui_set_msg_jpeg(const uint8_t *jpg, int len);
void ui_clear_msg_image(void);
bool ui_msg_has_image(void);
esp_err_t ui_set_quiet(bool quiet);
esp_err_t ui_set_clock(bool ok, int hour, int minute);
esp_err_t ui_set_pan_deg(float pan_deg);

/**
 * Gaze for EYES: same angle as pan when follow is ON.
 * When follow OFF, call with valid=false so pupils stay centered.
 */
esp_err_t ui_set_gaze_az(float az_deg, bool valid);

/**
 * Feed radar sample → update zone hysteresis.
 * @param present     fused presence (OUT / UART)
 * @param distance_m  metres when distance_ok
 * @param distance_ok true if UART target range is fresh; false → present maps to NEAR (no FAR/EYES)
 */
esp_err_t ui_feed_radar(bool present, float distance_m, bool distance_ok, int64_t now_ms);

ui_zone_t ui_current_zone(void);

/** SoftAP live tune: NEAR enter/exit, FAR max, VOID timeout. Clamped. */
void ui_set_distance_bands(float near_enter_m, float near_exit_m, float far_max_m, uint32_t void_ms);
void ui_get_distance_bands(float *near_enter_m, float *near_exit_m, float *far_max_m, uint32_t *void_ms);

/**
 * SoftAP Wi-Fi QR payload (WIFI:T:WPA;S:…;P:…;;).
 * Call after SoftAP starts so the PROV screen is scannable.
 */
esp_err_t ui_set_wifi_qr(const char *ssid, const char *password);

esp_err_t ui_show(ui_screen_t screen); /* force; normally auto from model */
ui_screen_t ui_current_screen(void);
ui_lang_t ui_current_lang(void);
ui_gender_t ui_current_gender(void);
esp_err_t ui_tick(int64_t now_ms);
/** Local face change first. Returns true if this press should PUB event.touch. */
bool ui_on_touch(int64_t now_ms);

/** Fired when MSG face closes (touch or 12s auto). MQTT uses this to PUB ack displayed. */
typedef void (*ui_msg_closed_cb_t)(void);
void ui_set_msg_closed_cb(ui_msg_closed_cb_t cb);

/**
 * Keep panel awake (SoftAP client connected / HTTP activity).
 * Presence also wakes; VOID SLEEP dims, then turns the backlight off.
 */
void ui_set_keep_awake(bool on);
void ui_poke_wake(uint32_t duration_ms);
bool ui_display_awake(void);

#ifdef __cplusplus
}
#endif
