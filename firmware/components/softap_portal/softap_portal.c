#include "softap_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_cfg.h"
#include "display.h"
#include "dns_server.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motion.h"
#include "mqtt_link.h"
#include "net_time.h"
#include "nvs_flash.h"
#include "radar_link.h"
#include "servo.h"
#include "telegram_link.h"
#include "ui.h"

static const char *TAG = "softap";

static char s_ssid[32];
static char s_pass[DEVICE_CFG_AP_PASS_LEN + 1];
static const char *s_portal_uri = "http://" SOFTAP_PORTAL_HOST;
static httpd_handle_t s_httpd;
static dns_server_handle_t s_dns;
static bool s_sta_connecting;
static char s_sta_ip[16];
static char s_sta_ssid[33];
static int8_t s_sta_rssi;
typedef enum {
    STA_UI_DOWN = 0,
    STA_UI_CONNECTING,
    STA_UI_OK,
    STA_UI_FAIL,
} sta_ui_t;
static sta_ui_t s_sta_ui;
static int s_sta_fail_reason;
static int s_sta_retries;
static int s_sta_retry_max = 5; /* any disconnect reason — then stop so the portal can scan */
static bool s_sta_want; /* user/NVS asked us to stay on this SSID */
static bool s_sta_hold; /* pause reconnect so scan / another SSID can run */
static bool s_sta_started;
static bool s_scan_busy;
static bool s_sta_self_disc;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    wifi_auth_mode_t authmode;
    int8_t rssi;
} scan_hit_t;
static scan_hit_t s_scan_hits[24];
static int s_scan_n;

/* Single-page portal: welcome (lang+theme) → main (follow → wifi → calibrate).
 * Source of truth: portal_page.html — regenerate with gen_portal_inc.py */
#include "portal_page.inc"
static ui_lang_t lang_from_str(const char *s)
{
    if (!s || !s[0]) {
        return UI_LANG_EN;
    }
    if (!strcmp(s, "zh-CN")) {
        return UI_LANG_ZH_CN;
    }
    if (!strcmp(s, "zh-TW")) {
        return UI_LANG_ZH_TW;
    }
    if (!strcmp(s, "ja")) {
        return UI_LANG_JA;
    }
    if (!strcmp(s, "ko")) {
        return UI_LANG_KO;
    }
    if (!strcmp(s, "es")) {
        return UI_LANG_ES;
    }
    if (!strcmp(s, "fr")) {
        return UI_LANG_FR;
    }
    if (!strcmp(s, "de")) {
        return UI_LANG_DE;
    }
    if (!strcmp(s, "pt")) {
        return UI_LANG_PT;
    }
    if (!strcmp(s, "ru")) {
        return UI_LANG_RU;
    }
    return UI_LANG_EN;
}

static void apply_prefs_to_ui(const char *lang, const char *gender, bool screen_flip)
{
    ui_set_lang(lang_from_str(lang));
    if (gender && !strcmp(gender, "male")) {
        ui_set_gender(UI_GENDER_MALE);
    } else {
        ui_set_gender(UI_GENDER_FEMALE);
    }
    (void)display_set_rotate_180(screen_flip);
}

static void apply_tune_from_cfg(const device_cfg_t *cfg)
{
    if (!cfg) {
        return;
    }
    ui_set_distance_bands(cfg->near_cm / 100.0f, cfg->near_exit_cm / 100.0f, cfg->far_cm / 100.0f,
                          (uint32_t)cfg->hide_s * 1000u);
    motion_set_timing(cfg->rise_ms, (uint32_t)cfg->hide_s * 1000u);
    motion_set_pan_limits((float)cfg->pan_slew_dps, (float)cfg->pan_max_deg);
    motion_set_far_max_m(cfg->far_cm / 100.0f);
}

static const char *zone_json(void)
{
    switch (ui_current_zone()) {
    case UI_ZONE_NEAR:
        return "near";
    case UI_ZONE_FAR:
        return "far";
    default:
        return "void";
    }
}

static esp_err_t root_get(httpd_req_t *req)
{
    ui_poke_wake(120000);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

/** Copy s into out as a JSON string body (quotes, backslashes, control chars escaped). */
static void json_esc(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && o + 7 < cap; ++p) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = 0;
}

static esp_err_t status_get(httpd_req_t *req)
{
    ui_poke_wake(60000);
    radar_target_t t;
    radar_link_get_target(&t);
    device_cfg_t cfg;
    device_cfg_load(&cfg);

    wifi_ap_record_t apinfo;
    bool sta_link = esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK;
    if (sta_link) {
        s_sta_rssi = apinfo.rssi;
        strncpy(s_sta_ssid, (char *)apinfo.ssid, sizeof(s_sta_ssid) - 1);
        s_sta_ssid[sizeof(s_sta_ssid) - 1] = 0;
        if (s_sta_ip[0] && s_sta_ui != STA_UI_CONNECTING) {
            s_sta_ui = STA_UI_OK;
        }
    }

    const char *sta_s = "down";
    if (s_sta_ui == STA_UI_CONNECTING || s_sta_connecting) {
        sta_s = "connecting";
    } else if (s_sta_ui == STA_UI_OK && s_sta_ip[0]) {
        sta_s = "ok";
    } else if (s_sta_ui == STA_UI_FAIL) {
        sta_s = "fail";
    }

    char blips[420];
    size_t bo = 0;
    blips[0] = 0;
    bo += (size_t)snprintf(blips + bo, sizeof(blips) - bo, "[");
    for (int i = 0; i < t.blip_count && bo + 48 < sizeof(blips); ++i) {
        int n = snprintf(blips + bo, sizeof(blips) - bo,
                         "%s{\"az\":%.1f,\"r\":%.2f,\"id\":%ld}", i ? "," : "",
                         (double)t.blips[i].azimuth_deg, (double)t.blips[i].distance_m,
                         (long)t.blips[i].track_id);
        if (n > 0) {
            bo += (size_t)n;
        }
    }
    if (bo + 2 < sizeof(blips)) {
        blips[bo++] = ']';
        blips[bo] = 0;
    }

    const char *claim_base = device_cfg_effective_claim_base(&cfg);
    const char *cloud_err = mqtt_link_last_error();
    char city_j[2 * DEVICE_CFG_CITY_MAX + 1];
    char place_j[2 * DEVICE_CFG_CITY_MAX + 1];
    char base_j[2 * DEVICE_CFG_HTTP_MAX + 1];
    json_esc(city_j, sizeof(city_j), cfg.city);
    json_esc(place_j, sizeof(place_j), cfg.place_name);
    json_esc(base_j, sizeof(base_j), claim_base);
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "{\"follow\":%s,\"present\":%s,\"zone\":\"%s\",\"theme\":\"%s\",\"lang\":\"%s\",\"screen_flip\":%s,"
             "\"pan_deg\":%.1f,\"cmd_pan_deg\":%.1f,\"pan_sign\":%d,\"lift_pct\":%.0f,"
             "\"tune\":{\"near_m\":%.2f,\"near_exit_m\":%.2f,\"far_m\":%.2f,\"hide_s\":%u,"
             "\"rise_ms\":%u,\"pan_dps\":%u,\"pan_max\":%u},"
             "\"radar\":{\"az\":%.1f,\"dist_m\":%.2f,\"count\":%d,\"az_ok\":%s,\"uart\":%s,"
             "\"blips\":%s},"
             "\"wifi\":{\"sta\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"saved\":%s,\"reason\":%d,\"retry\":%d},"
             "\"cloud\":{\"bind\":\"%s\",\"claim_base\":\"%s\",\"claimed\":%s,"
             "\"device_id\":\"%s\",\"mqtt\":\"%s\",\"err\":\"%s\"},"
             "\"place\":{\"city\":\"%s\",\"name\":\"%s\",\"ok\":%s,\"err\":\"%s\"},"
             "\"telegram\":{\"state\":\"%s\",\"bot\":\"%s\",\"token_set\":%s,\"paired\":%s}}",
             motion_follow_enabled() ? "true" : "false", t.present ? "true" : "false", zone_json(),
             (cfg.gender[0] && !strcmp(cfg.gender, "male")) ? "male" : "female",
             cfg.lang[0] ? cfg.lang : "en", display_get_rotate_180() ? "true" : "false",
             (double)servo_get_pan_deg(), (double)motion_cmd_pan_deg(), motion_get_pan_sign(),
             (double)servo_get_lift_pct(),
             cfg.near_cm / 100.0, cfg.near_exit_cm / 100.0, cfg.far_cm / 100.0,
             (unsigned)cfg.hide_s, (unsigned)cfg.rise_ms, (unsigned)cfg.pan_slew_dps,
             (unsigned)cfg.pan_max_deg,
             (double)t.azimuth_deg, (double)t.distance_m,
             t.target_count, t.azimuth_ok ? "true" : "false", t.from_uart ? "true" : "false", blips,
             sta_s, s_sta_ssid[0] ? s_sta_ssid : (cfg.wifi_ssid[0] ? cfg.wifi_ssid : ""), s_sta_ip,
             (int)s_sta_rssi, cfg.wifi_saved ? "true" : "false", s_sta_fail_reason, s_sta_retries,
             cfg.bind_code, base_j, cfg.claimed ? "true" : "false", cfg.device_id,
             mqtt_link_state_str(), cloud_err ? cloud_err : "",
             city_j, place_j, cfg.place_ok ? "true" : "false", net_time_last_error(),
             telegram_link_state_str(), telegram_link_bot_username(),
             cfg.tg_token[0] ? "true" : "false", cfg.tg_chat[0] ? "true" : "false");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static bool query_has(httpd_req_t *req, const char *key, char *val, size_t val_len)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) {
        return false;
    }
    char *q = malloc(qlen);
    if (!q) {
        return false;
    }
    if (httpd_req_get_url_query_str(req, q, qlen) != ESP_OK) {
        free(q);
        return false;
    }
    bool ok = httpd_query_key_value(q, key, val, val_len) == ESP_OK;
    free(q);
    return ok;
}

static esp_err_t follow_get(httpd_req_t *req)
{
    char v[16];
    ui_poke_wake(120000);
    if (query_has(req, "on", v, sizeof(v))) {
        motion_set_follow_enabled(atoi(v) != 0);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, motion_follow_enabled() ? "ON" : "OFF");
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len);

static int form_int(const char *body, const char *key, int fallback)
{
    char v[24];
    if (!form_get(body, key, v, sizeof(v)) || !v[0]) {
        return fallback;
    }
    return atoi(v);
}

static float form_float(const char *body, const char *key, float fallback)
{
    char v[24];
    if (!form_get(body, key, v, sizeof(v)) || !v[0]) {
        return fallback;
    }
    return (float)atof(v);
}

static esp_err_t tune_post(httpd_req_t *req)
{
    char body[320];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        return ESP_FAIL;
    }
    body[n] = 0;
    ui_poke_wake(120000);

    device_cfg_t cfg;
    device_cfg_load(&cfg);
    float near_m = form_float(body, "near_m", cfg.near_cm / 100.0f);
    float near_exit_m = form_float(body, "near_exit_m", cfg.near_exit_cm / 100.0f);
    float far_m = form_float(body, "far_m", cfg.far_cm / 100.0f);
    cfg.near_cm = (uint16_t)(near_m * 100.0f + 0.5f);
    cfg.near_exit_cm = (uint16_t)(near_exit_m * 100.0f + 0.5f);
    cfg.far_cm = (uint16_t)(far_m * 100.0f + 0.5f);
    cfg.hide_s = (uint16_t)form_int(body, "hide_s", cfg.hide_s);
    cfg.rise_ms = (uint16_t)form_int(body, "rise_ms", cfg.rise_ms);
    cfg.pan_slew_dps = (uint16_t)form_int(body, "pan_dps", cfg.pan_slew_dps);
    cfg.pan_max_deg = (uint16_t)form_int(body, "pan_max", cfg.pan_max_deg);

    esp_err_t err = device_cfg_save_tune(&cfg);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    device_cfg_load(&cfg);
    apply_tune_from_cfg(&cfg);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t defaults_post(httpd_req_t *req)
{
    ui_poke_wake(120000);
    esp_err_t err = device_cfg_reset_tune();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    device_cfg_t cfg;
    device_cfg_load(&cfg);
    apply_tune_from_cfg(&cfg);
    motion_set_pan_sign(1);
    motion_set_follow_enabled(true);
    ui_set_quiet(false);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t servo_get(httpd_req_t *req)
{
    char v[32];
    esp_err_t err = ESP_OK;
    ui_poke_wake(120000);

    if (query_has(req, "home", v, sizeof(v))) {
        motion_claim_servos();
        err = servo_home(2200);
        motion_sync_pan_cmd(0.0f);
        motion_notify_manual();
    } else if (query_has(req, "attach", v, sizeof(v)) || query_has(req, "flip_pan", v, sizeof(v)) ||
               query_has(req, "pan_set", v, sizeof(v)) || query_has(req, "lift_set", v, sizeof(v)) ||
               query_has(req, "pan", v, sizeof(v)) || query_has(req, "lift", v, sizeof(v))) {
        motion_claim_servos();
        if (query_has(req, "attach", v, sizeof(v))) {
            err = servo_attach(atoi(v) != 0);
        } else if (query_has(req, "flip_pan", v, sizeof(v))) {
            motion_set_pan_sign(-motion_get_pan_sign());
        } else if (query_has(req, "pan_set", v, sizeof(v))) {
            err = servo_set_pan_deg((float)atof(v));
            motion_sync_pan_cmd(servo_get_pan_deg());
        } else if (query_has(req, "lift_set", v, sizeof(v))) {
            err = servo_set_lift_pct((float)atof(v));
        } else if (query_has(req, "pan", v, sizeof(v))) {
            err = servo_jog_pan((float)atof(v));
            motion_sync_pan_cmd(servo_get_pan_deg());
        } else if (query_has(req, "lift", v, sizeof(v))) {
            err = servo_jog_lift((float)atof(v));
        }
        motion_notify_manual();
    }

    httpd_resp_set_type(req, "text/plain");
    if (err != ESP_OK) {
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    return httpd_resp_sendstr(req, "ok");
}

static void url_decode_inplace(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < out_len) {
                out[i++] = *p++;
            }
            out[i] = 0;
            url_decode_inplace(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    out[0] = 0;
    return false;
}

static esp_err_t prefs_post(httpd_req_t *req)
{
    char body[128];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        return ESP_FAIL;
    }
    body[n] = 0;
    ui_poke_wake(120000);

    char lang[16] = {0};
    char gender[16] = {0};
    char flip_s[8] = {0};
    form_get(body, "lang", lang, sizeof(lang));
    form_get(body, "gender", gender, sizeof(gender));
    form_get(body, "flip", flip_s, sizeof(flip_s));
    if (!lang[0]) {
        strcpy(lang, "en");
    }
    if (!gender[0]) {
        strcpy(gender, "female");
    }
    const bool flip = (flip_s[0] == '1' || !strcmp(flip_s, "true") || !strcmp(flip_s, "on"));

    /* Always push to round LCD even if NVS write fails — live link first. */
    apply_prefs_to_ui(lang, gender, flip);
    esp_err_t err = device_cfg_save_prefs(lang, gender, flip);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static int ssid_cmp_rssi(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = a;
    const wifi_ap_record_t *rb = b;
    return (int)rb->rssi - (int)ra->rssi;
}

static bool channel_is_24g(uint8_t primary)
{
    return primary >= 1 && primary <= 14;
}

static bool bind_charset_ok(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }
    for (const char *p = s; *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static esp_err_t sta_apply_config(const char *ssid, const char *pass)
{
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass ? pass : "", sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* Do not lock BSSID/channel from a portal scan — that AP may be gone after reboot. */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    return esp_wifi_set_config(WIFI_IF_STA, &sta);
}

static esp_err_t sta_connect_now(void)
{
    if (!s_sta_started) {
        return ESP_OK; /* WIFI_EVENT_STA_START will connect */
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_sta_self_disc = true;
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    return esp_wifi_connect();
}

static esp_err_t sta_apply_and_connect(const char *ssid, const char *pass)
{
    esp_err_t err = sta_apply_config(ssid, pass);
    if (err != ESP_OK) {
        return err;
    }
    return sta_connect_now();
}

static void sta_pause_for_scan(void)
{
    s_sta_hold = true;
    s_sta_connecting = false;
    if (!s_sta_started || s_sta_ip[0]) {
        return;
    }
    s_sta_self_disc = true;
    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void sta_resume_after_scan(void)
{
    if (s_sta_hold || !s_sta_want || !s_sta_started || s_sta_ip[0]) {
        return;
    }
    if (s_sta_retries >= s_sta_retry_max) {
        return;
    }
    s_sta_connecting = true;
    s_sta_ui = STA_UI_CONNECTING;
    (void)esp_wifi_connect();
}

static esp_err_t send_scan_json(httpd_req_t *req)
{
    char *json = malloc(64 + (size_t)(s_scan_n > 0 ? s_scan_n : 1) * 64);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    size_t off = 0;
    off += (size_t)snprintf(json + off, 64, "{\"networks\":[");
    bool first = true;
    for (int i = 0; i < s_scan_n; i++) {
        char esc[36];
        size_t e = 0;
        for (const char *p = s_scan_hits[i].ssid; *p && e + 1 < sizeof(esc); p++) {
            if (*p == '"' || *p == '\\') {
                if (e + 2 >= sizeof(esc)) {
                    break;
                }
                esc[e++] = '\\';
            }
            esc[e++] = *p;
        }
        esc[e] = 0;
        int n = snprintf(json + off, 64, "%s{\"ssid\":\"%s\",\"rssi\":%d}", first ? "" : ",", esc,
                         (int)s_scan_hits[i].rssi);
        if (n < 0) {
            break;
        }
        off += (size_t)n;
        first = false;
    }
    snprintf(json + off, 8, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t send = httpd_resp_sendstr(req, json);
    free(json);
    return send;
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    ui_poke_wake(120000);
    char force_v[8] = {0};
    const bool force = query_has(req, "force", force_v, sizeof(force_v));

    /* Associated APSTA can scan. A connecting STA cannot — it eats the radio. */
    if (s_scan_busy) {
        return send_scan_json(req);
    }
    if (!force && s_sta_connecting && !s_sta_ip[0]) {
        return send_scan_json(req);
    }
    if (force && !s_sta_ip[0]) {
        sta_pause_for_scan();
        if (s_sta_ui == STA_UI_CONNECTING) {
            s_sta_ui = STA_UI_DOWN;
        }
    }

    s_scan_busy = true;
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {.active = {.min = 80, .max = 120}},
    };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK && force && !s_sta_ip[0]) {
        ESP_LOGW(TAG, "scan_start %s — disconnect and retry", esp_err_to_name(err));
        sta_pause_for_scan();
        err = esp_wifi_scan_start(&sc, true);
    }
    if (err != ESP_OK) {
        s_scan_busy = false;
        if (s_scan_n > 0) {
            ESP_LOGW(TAG, "scan_start %s — serving last list", esp_err_to_name(err));
            sta_resume_after_scan();
            return send_scan_json(req);
        }
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"networks\":[],\"error\":\"scan_start\"}");
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > 40) {
        ap_num = 40;
    }
    wifi_ap_record_t *recs = calloc(ap_num ? ap_num : 1, sizeof(*recs));
    if (!recs) {
        s_scan_busy = false;
        sta_resume_after_scan();
        return ESP_ERR_NO_MEM;
    }
    uint16_t got = ap_num;
    err = esp_wifi_scan_get_ap_records(&got, recs);
    if (err != ESP_OK) {
        free(recs);
        s_scan_busy = false;
        sta_resume_after_scan();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }
    qsort(recs, got, sizeof(*recs), ssid_cmp_rssi);

    s_scan_n = 0;
    wifi_ap_record_t *best = calloc(got ? got : 1, sizeof(*best));
    int nbest = 0;
    if (!best) {
        free(recs);
        s_scan_busy = false;
        sta_resume_after_scan();
        return ESP_ERR_NO_MEM;
    }
    for (uint16_t i = 0; i < got; i++) {
        if (recs[i].ssid[0] == 0) {
            continue;
        }
        int found = -1;
        for (int k = 0; k < nbest; k++) {
            if (strcmp((char *)best[k].ssid, (char *)recs[i].ssid) == 0) {
                found = k;
                break;
            }
        }
        if (found < 0) {
            best[nbest++] = recs[i];
        } else if (recs[i].rssi > best[found].rssi) {
            best[found] = recs[i];
        }
    }
    free(recs);

    for (int i = 0; i < nbest && s_scan_n < (int)(sizeof(s_scan_hits) / sizeof(s_scan_hits[0]));
         i++) {
        scan_hit_t *h = &s_scan_hits[s_scan_n++];
        memset(h, 0, sizeof(*h));
        strncpy(h->ssid, (char *)best[i].ssid, sizeof(h->ssid) - 1);
        memcpy(h->bssid, best[i].bssid, 6);
        h->channel = best[i].primary;
        h->authmode = best[i].authmode;
        h->rssi = best[i].rssi;
    }
    free(best);
    s_scan_busy = false;
    sta_resume_after_scan();
    return send_scan_json(req);
}

static esp_err_t wifi_connect_post(httpd_req_t *req)
{
    char body[256];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        return ESP_FAIL;
    }
    body[n] = 0;
    ui_poke_wake(180000);

    device_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    form_get(body, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    form_get(body, "pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
    if (!cfg.wifi_ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "ssid required");
    }

    esp_err_t err = device_cfg_save_wifi(&cfg);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    ui_set_provisioned(true);

    s_sta_want = true;
    s_sta_hold = false;
    s_sta_connecting = true;
    s_sta_ui = STA_UI_CONNECTING;
    s_sta_ip[0] = 0;
    s_sta_rssi = 0;
    s_sta_fail_reason = 0;
    s_sta_retries = 0;
    strncpy(s_sta_ssid, cfg.wifi_ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = 0;

    err = sta_apply_and_connect(cfg.wifi_ssid, cfg.wifi_pass);
    if (err != ESP_OK) {
        s_sta_connecting = false;
        s_sta_ui = STA_UI_FAIL;
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "STA connecting to %s", cfg.wifi_ssid);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t cloud_connect_post(httpd_req_t *req)
{
    char body[256];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        return ESP_FAIL;
    }
    body[n] = 0;
    ui_poke_wake(180000);

    device_cfg_t prev;
    memset(&prev, 0, sizeof(prev));
    (void)device_cfg_load(&prev);

    char bind[DEVICE_CFG_BIND_MAX + 1] = {0};
    char claim_base[DEVICE_CFG_HTTP_MAX + 1] = {0};
    form_get(body, "bind", bind, sizeof(bind));
    form_get(body, "claim_base", claim_base, sizeof(claim_base));
    if (!bind_charset_ok(bind)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "bind required");
    }

    if (prev.bind_code[0] && strcmp(bind, prev.bind_code) != 0) {
        (void)device_cfg_clear_claim();
    }

    esp_err_t err = device_cfg_save_cloud(bind, claim_base);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "server URL must start with http:// or https://");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "cloud connect bind=%s (wifi untouched)", bind);
    mqtt_link_kick();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t place_post(httpd_req_t *req)
{
    char body[256];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        return ESP_FAIL;
    }
    body[n] = 0;
    ui_poke_wake(120000);

    char city[3 * DEVICE_CFG_CITY_MAX + 1] = {0};
    form_get(body, "city", city, sizeof(city));
    city[DEVICE_CFG_CITY_MAX] = 0;
    /* Browser offset: minutes east of UTC (JS getTimezoneOffset() negated). */
    int utc_off = form_int(body, "utc_off", 10000);
    esp_err_t err = device_cfg_save_city(city, utc_off);
    if (err == ESP_OK) {
        net_time_kick();
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static esp_err_t telegram_post(httpd_req_t *req)
{
    char body[256];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        return ESP_FAIL;
    }
    body[n] = 0;
    ui_poke_wake(120000);

    char token[DEVICE_CFG_TG_TOKEN_MAX + 8] = {0};
    char unpair[8] = {0};
    esp_err_t err;
    if (form_get(body, "unpair", unpair, sizeof(unpair)) && unpair[0] == '1') {
        err = device_cfg_save_tg_chat("");
    } else {
        form_get(body, "token", token, sizeof(token));
        if (strlen(token) > DEVICE_CFG_TG_TOKEN_MAX) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "token too long");
        }
        err = device_cfg_save_tg_token(token);
    }
    if (err == ESP_OK) {
        telegram_link_kick();
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_sta_started = true;
        if (s_sta_want && !s_sta_hold) {
            s_sta_connecting = true;
            s_sta_ui = STA_UI_CONNECTING;
            ESP_LOGI(TAG, "STA start — connecting saved network");
            (void)esp_wifi_connect();
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        const int reason = disc ? (int)disc->reason : -1;
        s_sta_fail_reason = reason;
        s_sta_ip[0] = 0;

        if (s_scan_busy) {
            ESP_LOGW(TAG, "STA dropped during scan reason=%d — resume after scan", reason);
            if (s_sta_want) {
                s_sta_connecting = true;
                s_sta_ui = STA_UI_CONNECTING;
            }
            return;
        }
        if (s_sta_self_disc) {
            s_sta_self_disc = false;
            return;
        }

        ESP_LOGW(TAG, "STA disconnected reason=%d retry=%d hold=%d", reason, s_sta_retries,
                 (int)s_sta_hold);

        s_sta_retries++;
        if (s_sta_want && !s_sta_hold && s_sta_retries < s_sta_retry_max) {
            s_sta_connecting = true;
            s_sta_ui = STA_UI_CONNECTING;
            wifi_config_t sta = {0};
            if (esp_wifi_get_config(WIFI_IF_STA, &sta) == ESP_OK) {
                sta.sta.bssid_set = false;
                sta.sta.channel = 0;
                sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
                (void)esp_wifi_set_config(WIFI_IF_STA, &sta);
            }
            (void)esp_wifi_connect();
            return;
        }

        if (s_sta_want || s_sta_ui == STA_UI_CONNECTING || s_sta_ui == STA_UI_OK) {
            s_sta_ui = STA_UI_FAIL;
        }
        s_sta_connecting = false;
        s_sta_hold = true;
        ESP_LOGW(TAG, "STA give up — portal can scan other networks");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_sta_connecting = false;
        s_sta_ui = STA_UI_OK;
        s_sta_fail_reason = 0;
        s_sta_retries = 0;
        wifi_ap_record_t apinfo;
        if (esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK) {
            s_sta_rssi = apinfo.rssi;
            strncpy(s_sta_ssid, (char *)apinfo.ssid, sizeof(s_sta_ssid) - 1);
            s_sta_ssid[sizeof(s_sta_ssid) - 1] = 0;
            /* Align SoftAP channel with STA for better coexistence. */
            if (channel_is_24g(apinfo.primary)) {
                (void)esp_wifi_set_channel(apinfo.primary, WIFI_SECOND_CHAN_NONE);
            }
        }
        /* APSTA: default netif is often SoftAP — internet must go via STA. */
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            (void)esp_netif_set_default_netif(sta_netif);
        }
        ESP_LOGI(TAG, "STA got IP %s ssid=%s", s_sta_ip, s_sta_ssid);
    }
}

static esp_err_t start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/follow", .method = HTTP_GET, .handler = follow_get},
        {.uri = "/api/tune", .method = HTTP_POST, .handler = tune_post},
        {.uri = "/api/defaults", .method = HTTP_POST, .handler = defaults_post},
        {.uri = "/api/prefs", .method = HTTP_POST, .handler = prefs_post},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_post},
        {.uri = "/api/cloud/connect", .method = HTTP_POST, .handler = cloud_connect_post},
        {.uri = "/api/place", .method = HTTP_POST, .handler = place_post},
        {.uri = "/api/telegram", .method = HTTP_POST, .handler = telegram_post},
        {.uri = "/servo", .method = HTTP_GET, .handler = servo_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &uris[i]), TAG, "uri");
    }
    return ESP_OK;
}

esp_err_t softap_portal_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event");
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi init");

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "mac");
    snprintf(s_ssid, sizeof(s_ssid), "DeskBuddy-%02X%02X", mac[4], mac[5]);
    snprintf(s_pass, sizeof(s_pass), "%s", device_cfg_ap_password());

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_ssid, sizeof(ap.ap.ssid));
    strncpy((char *)ap.ap.password, s_pass, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(s_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    /* APSTA: SoftAP for phone + STA scan/connect for home Wi‑Fi. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "cfg");
    ESP_RETURN_ON_ERROR(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20), TAG, "bw");

    device_cfg_t cfg;
    if (device_cfg_load(&cfg) == ESP_OK) {
        apply_prefs_to_ui(cfg.lang[0] ? cfg.lang : "en", cfg.gender[0] ? cfg.gender : "female",
                          cfg.screen_flip != 0);
        if (cfg.wifi_saved && cfg.wifi_ssid[0]) {
            s_sta_want = true;
            s_sta_connecting = true;
            s_sta_ui = STA_UI_CONNECTING;
            s_sta_retries = 0;
            strncpy(s_sta_ssid, cfg.wifi_ssid, sizeof(s_sta_ssid) - 1);
            s_sta_ssid[sizeof(s_sta_ssid) - 1] = 0;
            /* Config before start so STA_START can connect from NVS. */
            if (sta_apply_config(cfg.wifi_ssid, cfg.wifi_pass) != ESP_OK) {
                s_sta_connecting = false;
                s_sta_ui = STA_UI_FAIL;
            }
        }
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_RETURN_ON_ERROR(start_httpd(), TAG, "httpd");

    /* DHCP Option 114 + captive DNS: phone opens http://mod.config */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(ap_netif));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
            ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, (void *)s_portal_uri,
            (uint32_t)strlen(s_portal_uri)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(ap_netif));
    }
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);
    if (!s_dns) {
        ESP_LOGW(TAG, "DNS redirect server failed to start");
    }

    ESP_LOGI(TAG, "SoftAP %s → http://%s/ (password on the screen; IP 192.168.4.1 still ok)",
             s_ssid, SOFTAP_PORTAL_HOST);
    return ESP_OK;
}

const char *softap_portal_ssid(void)
{
    return s_ssid;
}

const char *softap_portal_pass(void)
{
    return s_pass;
}

const char *softap_portal_host(void)
{
    return SOFTAP_PORTAL_HOST;
}

int softap_portal_sta_count(void)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return 0;
    }
    return list.num;
}
