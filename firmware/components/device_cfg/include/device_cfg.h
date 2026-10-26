#pragma once

/**
 * NVS-backed device / cloud provisioning config.
 * Filled by the SoftAP portal and, for Level 3, the server claim response.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CFG_NS            "petcfg"
#define DEVICE_CFG_WIFI_SSID_MAX 32
#define DEVICE_CFG_WIFI_PASS_MAX 64
#define DEVICE_CFG_BIND_MAX      32
#define DEVICE_CFG_ID_MAX        40
#define DEVICE_CFG_HOST_MAX      64
#define DEVICE_CFG_HTTP_MAX      96
#define DEVICE_CFG_CITY_MAX      48
#define DEVICE_CFG_TG_TOKEN_MAX  64
#define DEVICE_CFG_TG_CHAT_MAX   24
#define DEVICE_CFG_AP_PASS_LEN   10

/* Factory behavior (distance / timing). Follow / stealth / pan_sign are session, not NVS. */
#define DEVICE_CFG_NEAR_CM       50
#define DEVICE_CFG_NEAR_EXIT_CM  60
#define DEVICE_CFG_FAR_CM        500
#define DEVICE_CFG_HIDE_S        10
#define DEVICE_CFG_RISE_MS       300
#define DEVICE_CFG_PAN_DPS       90
#define DEVICE_CFG_PAN_MAX       80

typedef struct {
    char wifi_ssid[DEVICE_CFG_WIFI_SSID_MAX + 1];
    char wifi_pass[DEVICE_CFG_WIFI_PASS_MAX + 1];
    char bind_code[DEVICE_CFG_BIND_MAX + 1];
    char claim_base[DEVICE_CFG_HTTP_MAX + 1]; /* SoftAP / lab POST /claim origin */
    char lang[8];     /* e.g. zh-CN */
    char gender[8];   /* female | male */
    uint8_t screen_flip; /* 0 = default mount, 1 = rotate 180° for enclosure */
    bool follow_enabled; /* RAM/session only; always true after load (boot ON) */

    /* SoftAP-tunable bands / timing (centimetres + seconds). Missing NVS → defaults. */
    uint16_t near_cm;       /* NEAR enter; default DEVICE_CFG_NEAR_CM */
    uint16_t near_exit_cm;  /* NEAR exit hysteresis; default DEVICE_CFG_NEAR_EXIT_CM */
    uint16_t far_cm;        /* FAR / hide beyond; default DEVICE_CFG_FAR_CM */
    uint16_t hide_s;        /* VOID / toy hide after no target; default DEVICE_CFG_HIDE_S */
    uint16_t rise_ms;       /* present confirm before rise; default DEVICE_CFG_RISE_MS */
    uint16_t pan_slew_dps;  /* pan slew deg/s; default DEVICE_CFG_PAN_DPS */
    uint16_t pan_max_deg;   /* pan soft limit; default DEVICE_CFG_PAN_MAX */

    /* Place for local time + weather (Level 1). Lat/lon come from geocoding the city. */
    char city[DEVICE_CFG_CITY_MAX + 1];
    char place_name[DEVICE_CFG_CITY_MAX + 1]; /* resolved name, e.g. "Berlin, DE" */
    float lat;
    float lon;
    bool place_ok;           /* lat/lon resolved for `city` */
    int16_t utc_offset_min;  /* last known offset (browser at setup, then weather API) */

    /* Telegram bot (Level 2). Empty token → off. Chat id is learned on the first /start. */
    char tg_token[DEVICE_CFG_TG_TOKEN_MAX + 1];
    char tg_chat[DEVICE_CFG_TG_CHAT_MAX + 1];

    /* SoftAP password: random per device, created on first boot. */
    char ap_pass[DEVICE_CFG_AP_PASS_LEN + 1];

    bool wifi_saved;
    bool claimed;

    char device_id[DEVICE_CFG_ID_MAX + 1];
    char pair_id[DEVICE_CFG_ID_MAX + 1];
    char http_base[DEVICE_CFG_HTTP_MAX + 1];
    char mqtt_host[DEVICE_CFG_HOST_MAX + 1];
    uint16_t mqtt_port;
    char mqtt_transport[8]; /* mqtt | mqtts */
    char mqtt_username[DEVICE_CFG_ID_MAX + 1];
    char mqtt_password[DEVICE_CFG_WIFI_PASS_MAX + 1];
    bool mqtt_tls;
    bool sntp_before_tls;
} device_cfg_t;

/** Load from NVS (missing keys → empty / lab defaults for mqtt host empty). */
esp_err_t device_cfg_load(device_cfg_t *out);

/** Persist home Wi-Fi only (does not touch bind / claim_base). */
esp_err_t device_cfg_save_wifi(const device_cfg_t *in);

/** Persist bind code + claim HTTP origin of your own server (Level 3). Both required. */
esp_err_t device_cfg_save_cloud(const char *bind, const char *claim_base);

/** Persist city (clears resolved lat/lon when the city changes) + browser UTC offset. */
esp_err_t device_cfg_save_city(const char *city, int utc_offset_min);

/** Persist geocoding result for the current city. */
esp_err_t device_cfg_save_place(const char *place_name, float lat, float lon);

/** Persist the latest UTC offset reported by the weather API (DST follows automatically). */
esp_err_t device_cfg_save_utc_offset(int utc_offset_min);

/** Persist Telegram bot token. A new token forgets the paired chat. Empty → Telegram off. */
esp_err_t device_cfg_save_tg_token(const char *token);

/** Persist (or clear with "") the paired Telegram chat id. */
esp_err_t device_cfg_save_tg_chat(const char *chat_id);

/** SoftAP password; generated with the hardware RNG and stored on first call. */
const char *device_cfg_ap_password(void);

/** Persist SoftAP prefs: language + theme + screen orientation. */
esp_err_t device_cfg_save_prefs(const char *lang, const char *gender, bool screen_flip);

/** Drop legacy NVS `follow` key (session-only since 2026-09-07). */
esp_err_t device_cfg_erase_follow(void);

/** Persist distance / timing knobs from the SoftAP portal. Values are clamped. */
esp_err_t device_cfg_save_tune(const device_cfg_t *in);

/** Restore factory distance / timing (does not touch Wi-Fi, bind, claim, lang). */
esp_err_t device_cfg_reset_tune(void);

/** Persist claim response fields (MQTT credentials). */
esp_err_t device_cfg_save_claim(const device_cfg_t *in);

/** Clear claim credentials (keep Wi-Fi). */
esp_err_t device_cfg_clear_claim(void);

/** Claim HTTP origin saved in the portal, or "" when Level 3 is not configured. */
const char *device_cfg_effective_claim_base(const device_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
