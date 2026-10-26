#include "mqtt_link.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "device_cfg.h"
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "motion.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "radar_link.h"
#include "servo.h"
#include "net_time.h"
#include "ui.h"
#include "psa/crypto.h"

static const char *TAG = "mqtt_link";

#define BIT_IP     BIT0
#define BIT_KICK   BIT1
#define BIT_INBOX  BIT2
#define BIT_MEDIA  BIT3
#define BIT_MOTION BIT4
#define TIME_OK_EPOCH 1700000000 /* 2023-11 — reject 1970 */
#define MEDIA_MAX_BYTES (48 * 1024)

static bool url_is_https(const char *url)
{
    return url && strncmp(url, "https://", 8) == 0;
}

static EventGroupHandle_t s_ev;
static mqtt_link_state_t s_state = MQTT_LINK_IDLE;
static char s_err[48];
static bool s_sntp_ok;

static esp_mqtt_client_handle_t s_mqtt;
static char s_uri[128];
static char s_lwt_topic[80];
static char s_sub_topic[80];
static char s_client_id[DEVICE_CFG_ID_MAX + 1];
static char s_user[DEVICE_CFG_ID_MAX + 1];
static char s_pass[DEVICE_CFG_WIFI_PASS_MAX + 1];
static const char s_lwt_msg[] = "{\"online\":false}";

static bool s_motion_is_jog;
static float s_motion_lift;
static float s_motion_pan;
static char s_motion_preset[24];
static int s_motion_repeat = 1;
static char s_last_msg_id[64];
static char s_displayed_id[64];

typedef struct {
    char *buf;
    int len;
    int cap;
} http_acc_t;

static void set_state(mqtt_link_state_t st, const char *err)
{
    s_state = st;
    if (err && err[0]) {
        snprintf(s_err, sizeof(s_err), "%s", err);
    } else {
        s_err[0] = 0;
    }
    /* The face follows Wi-Fi (net_time), not this optional server link. */
}

mqtt_link_state_t mqtt_link_state(void)
{
    return s_state;
}

const char *mqtt_link_state_str(void)
{
    switch (s_state) {
    case MQTT_LINK_WAIT_IP:
        return "wait_ip";
    case MQTT_LINK_SNTP:
        return "sntp";
    case MQTT_LINK_CLAIM:
        return "claim";
    case MQTT_LINK_CONNECTING:
        return "mqtt";
    case MQTT_LINK_UP:
        return "up";
    case MQTT_LINK_ERR:
        return "err";
    default:
        return "idle";
    }
}

const char *mqtt_link_last_error(void)
{
    return s_err;
}

void mqtt_link_kick(void)
{
    if (s_ev) {
        xEventGroupSetBits(s_ev, BIT_KICK);
    }
}

static void prefer_sta_route(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        (void)esp_netif_set_default_netif(sta);
    }
}

static void on_sta_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP && s_ev) {
        prefer_sta_route();
        xEventGroupSetBits(s_ev, BIT_IP);
    }
}

/* Clock, SNTP and weather live in net_time (Level 1); TLS here only needs the clock. */
static bool sntp_sync(void)
{
    s_sntp_ok = net_time_clock_ok();
    if (!s_sntp_ok && s_state != MQTT_LINK_UP && s_state != MQTT_LINK_CONNECTING) {
        set_state(MQTT_LINK_SNTP, "wait_clock");
    }
    return s_sntp_ok;
}

static bool bind_ok(const char *s)
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

static const char *cstr(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static int cint(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    return cJSON_IsNumber(it) ? (int)it->valuedouble : fallback;
}

static bool cbool(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it);
    }
    return fallback;
}

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    http_acc_t *a = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && a && evt->data && evt->data_len > 0) {
        int n = evt->data_len;
        if (a->len + n >= a->cap) {
            n = a->cap - a->len - 1;
        }
        if (n > 0) {
            memcpy(a->buf + a->len, evt->data, (size_t)n);
            a->len += n;
            a->buf[a->len] = 0;
        }
    }
    return ESP_OK;
}

static bool parse_claim_json(const char *body, device_cfg_t *out)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return false;
    }
    const char *device_id = cstr(root, "device_id");
    const char *pair_id = cstr(root, "pair_id");
    const char *http_base = cstr(root, "http_base");
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    const char *host = mqtt ? cstr(mqtt, "host") : NULL;
    const char *user = mqtt ? cstr(mqtt, "username") : NULL;
    const char *pass = mqtt ? cstr(mqtt, "password") : NULL;
    const char *tr = mqtt ? cstr(mqtt, "transport") : NULL;
    bool ok = device_id && device_id[0] && user && user[0] && pass && pass[0] && host && host[0];
    if (ok) {
        memset(out, 0, sizeof(*out));
        snprintf(out->device_id, sizeof(out->device_id), "%s", device_id);
        snprintf(out->pair_id, sizeof(out->pair_id), "%s", pair_id ? pair_id : "");
        snprintf(out->http_base, sizeof(out->http_base), "%s", http_base ? http_base : "");
        snprintf(out->mqtt_host, sizeof(out->mqtt_host), "%s", host);
        snprintf(out->mqtt_username, sizeof(out->mqtt_username), "%s", user);
        snprintf(out->mqtt_password, sizeof(out->mqtt_password), "%s", pass);
        snprintf(out->mqtt_transport, sizeof(out->mqtt_transport), "%s",
                 (tr && tr[0]) ? tr : "mqtt");
        out->mqtt_port = (uint16_t)cint(mqtt, "port", 1883);
        out->mqtt_tls = cbool(mqtt, "tls", strcmp(out->mqtt_transport, "mqtts") == 0);
        out->sntp_before_tls = cbool(mqtt, "sntp_before_tls", true);
        out->claimed = true;
    }
    cJSON_Delete(root);
    return ok;
}

static esp_err_t do_claim(const device_cfg_t *cfg)
{
    if (!bind_ok(cfg->bind_code)) {
        set_state(MQTT_LINK_ERR, "bad_bind");
        return ESP_ERR_INVALID_ARG;
    }

    set_state(MQTT_LINK_CLAIM, NULL);
    prefer_sta_route();
    const char *base = device_cfg_effective_claim_base(cfg);
    char url[160];
    size_t bl = strlen(base);
    if (bl > 0 && base[bl - 1] == '/') {
        snprintf(url, sizeof(url), "%sclaim", base);
    } else {
        snprintf(url, sizeof(url), "%s/claim", base);
    }

    char post[96];
    snprintf(post, sizeof(post), "{\"binding_code\":\"%s\"}", cfg->bind_code);

    char resp[1536];
    http_acc_t acc = {.buf = resp, .len = 0, .cap = (int)sizeof(resp)};
    resp[0] = 0;

    /* crt_bundle on an http:// URL makes esp_http_client_init return NULL (http_init). */
    const bool tls = url_is_https(url);
    esp_http_client_config_t http = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .event_handler = http_evt,
        .user_data = &acc,
        .transport_type = tls ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) {
        set_state(MQTT_LINK_ERR, "http_init");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post, (int)strlen(post));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "claim transport %s url=%s", esp_err_to_name(err), url);
        set_state(MQTT_LINK_ERR, "claim_net");
        return err;
    }
    if (status == 409) {
        set_state(MQTT_LINK_ERR, "claim_409");
        ESP_LOGW(TAG, "claim 409 pair full");
        return ESP_FAIL;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "claim HTTP %d body=%s", status, resp);
        set_state(MQTT_LINK_ERR, "claim_http");
        return ESP_FAIL;
    }

    device_cfg_t claimed;
    if (!parse_claim_json(resp, &claimed)) {
        set_state(MQTT_LINK_ERR, "claim_json");
        return ESP_FAIL;
    }
    err = device_cfg_save_claim(&claimed);
    if (err != ESP_OK) {
        set_state(MQTT_LINK_ERR, "claim_nvs");
        return err;
    }
    ESP_LOGI(TAG, "claimed device_id=%s host=%s:%u %s", claimed.device_id, claimed.mqtt_host,
             (unsigned)claimed.mqtt_port, claimed.mqtt_transport);
    return ESP_OK;
}

static const char *weather_label(const char *code)
{
    if (!code || !code[0] || !strcmp(code, "unknown")) {
        return NULL;
    }
    if (!strcmp(code, "sunny")) {
        return "晴";
    }
    if (!strcmp(code, "cloudy")) {
        return "阴";
    }
    if (!strcmp(code, "rain")) {
        return "雨";
    }
    if (!strcmp(code, "snow")) {
        return "雪";
    }
    return NULL;
}

static void rewrite_device_http_url(char *out, size_t cap, const char *in)
{
    if (!out || cap < 8) {
        return;
    }
    snprintf(out, cap, "%s", in ? in : "");
}

static char s_media_url[192];
static char s_media_sha[65];
static int s_media_bytes;
static char s_media_id[48];

static void queue_media(const char *url, const char *sha, int nbytes, const char *id)
{
    if (!url || !url[0]) {
        return;
    }
    rewrite_device_http_url(s_media_url, sizeof(s_media_url), url);
    snprintf(s_media_sha, sizeof(s_media_sha), "%s", sha ? sha : "");
    s_media_bytes = nbytes;
    snprintf(s_media_id, sizeof(s_media_id), "%s", id ? id : "");
    if (s_ev) {
        xEventGroupSetBits(s_ev, BIT_MEDIA);
    }
}

static void publish_ack(const char *kind, const char *id, const char *status, const char *error)
{
    if (!s_mqtt || !s_client_id[0] || !id || !id[0]) {
        return;
    }
    char topic[80];
    snprintf(topic, sizeof(topic), "dev/%s/ack", s_client_id);
    char body[192];
    snprintf(body, sizeof(body),
             "{\"id\":\"%s\",\"ts\":%ld,\"kind\":\"%s\",\"status\":\"%s\",\"error\":\"%s\"}",
             id, (long)time(NULL), kind ? kind : "msg", status ? status : "failed",
             error ? error : "");
    (void)esp_mqtt_client_publish(s_mqtt, topic, body, 0, 1, 0);
}

static void remember_msg_id(const char *id)
{
    if (!id || !id[0]) {
        return;
    }
    snprintf(s_last_msg_id, sizeof(s_last_msg_id), "%s", id);
}

static bool msg_already_shown(const char *id)
{
    return id && id[0] && s_displayed_id[0] && strcmp(id, s_displayed_id) == 0;
}

static void nvs_load_displayed(void)
{
    nvs_handle_t h;
    if (nvs_open("petmqtt", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_displayed_id);
    if (nvs_get_str(h, "last_msg", s_displayed_id, &len) != ESP_OK) {
        s_displayed_id[0] = 0;
    }
    nvs_close(h);
}

static void nvs_save_displayed(const char *id)
{
    if (!id || !id[0]) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("petmqtt", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_str(h, "last_msg", id) == ESP_OK) {
        (void)nvs_commit(h);
        snprintf(s_displayed_id, sizeof(s_displayed_id), "%s", id);
    }
    nvs_close(h);
}

void mqtt_link_notify_msg_displayed(void)
{
    if (!s_last_msg_id[0]) {
        return;
    }
    nvs_save_displayed(s_last_msg_id);
    publish_ack("msg", s_last_msg_id, "displayed", "");
}

static bool hex_sha256_eq(const uint8_t *data, int len, const char *expect)
{
    if (!expect || strlen(expect) != 64) {
        return true;
    }
    uint8_t hash[32];
    size_t olen = 0;
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    if (psa_hash_compute(PSA_ALG_SHA_256, data, (size_t)len, hash, sizeof(hash), &olen) != PSA_SUCCESS) {
        return false;
    }
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    return strcasecmp(hex, expect) == 0;
}

static void fetch_media(void)
{
    if (!s_media_url[0]) {
        return;
    }
    device_cfg_t cfg;
    if (device_cfg_load(&cfg) != ESP_OK || !cfg.mqtt_password[0]) {
        return;
    }
    prefer_sta_route();
    int cap = MEDIA_MAX_BYTES + 1;
    if (s_media_bytes > MEDIA_MAX_BYTES) {
        ESP_LOGW(TAG, "media too large %d", s_media_bytes);
        ui_clear_msg_image();
        ui_set_msg_text("图片太大");
        publish_ack("msg", s_media_id, "failed", "too_large");
        return;
    }
    uint8_t *buf = heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc((size_t)cap);
    }
    if (!buf) {
        ui_clear_msg_image();
        ui_set_msg_text("图片内存不足");
        publish_ack("msg", s_media_id, "failed", "nomem");
        return;
    }
    http_acc_t acc = {.buf = (char *)buf, .len = 0, .cap = cap};
    buf[0] = 0;
    const bool tls = url_is_https(s_media_url);
    char auth[80];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg.mqtt_password);
    esp_http_client_config_t http = {
        .url = s_media_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .event_handler = http_evt,
        .user_data = &acc,
        .transport_type = tls ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) {
        free(buf);
        ui_clear_msg_image();
        ui_set_msg_text("图片下载失败");
        publish_ack("msg", s_media_id, "failed", "http_init");
        return;
    }
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "User-Agent", "radar-desk-buddy/1.0");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300 || acc.len < 4) {
        ESP_LOGW(TAG, "media GET %s HTTP %d %s n=%d", s_media_url, status, esp_err_to_name(err),
                 acc.len);
        free(buf);
        ui_clear_msg_image();
        ui_set_msg_text("图片下载失败");
        publish_ack("msg", s_media_id, "failed", "http");
        return;
    }
    if (s_media_bytes > 0 && acc.len != s_media_bytes) {
        ESP_LOGW(TAG, "media bytes %d != %d", acc.len, s_media_bytes);
    }
    if (!hex_sha256_eq(buf, acc.len, s_media_sha)) {
        ESP_LOGW(TAG, "media sha mismatch");
        free(buf);
        ui_clear_msg_image();
        ui_set_msg_text("图片校验失败");
        publish_ack("msg", s_media_id, "failed", "sha256");
        return;
    }
    esp_err_t dec = ui_set_msg_jpeg(buf, acc.len);
    free(buf);
    if (dec != ESP_OK) {
        ui_clear_msg_image();
        ui_set_msg_text("图片无法显示");
        publish_ack("msg", s_media_id, "failed", "jpeg");
        return;
    }
    publish_ack("msg", s_media_id, "received", "");
    ESP_LOGI(TAG, "media jpeg ready %d B", acc.len);
}

static void apply_msg_obj(cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return;
    }
    const char *type = cstr(root, "type");
    const char *text = cstr(root, "text");
    const char *caption = cstr(root, "caption");
    const char *url = cstr(root, "url");
    const char *sha = cstr(root, "sha256");
    const char *id = cstr(root, "id");
    int nbytes = cint(root, "bytes", 0);
    const char *show = (text && text[0]) ? text : caption;
    remember_msg_id(id);

    if (type && (!strcmp(type, "image") || !strcmp(type, "anim") || !strcmp(type, "video"))) {
        ui_set_msg_text(show);
        ui_clear_msg_image();
        if (url && url[0] && type && !strcmp(type, "image")) {
            queue_media(url, sha, nbytes, id);
        } else if (!show || !show[0]) {
            ui_set_msg_text(!strcmp(type, "anim") ? "收到动图" : "收到短片");
        }
        return;
    }
    ui_clear_msg_image();
    if (show && show[0]) {
        ui_set_msg_text(show);
    }
}

static bool s_weather_from_mqtt;

#define PRESENCE_ENTER_MS 1000
#define PRESENCE_LEAVE_MS 20000

static bool s_stealth;
static bool s_presence_reported;
static bool s_presence_reported_valid;
static bool s_presence_raw;
static int64_t s_presence_raw_since_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void presence_reset(void)
{
    s_presence_reported_valid = false;
    s_presence_raw_since_ms = 0;
}

static void apply_weather_obj(cJSON *weather)
{
    if (!cJSON_IsObject(weather)) {
        return;
    }
    const char *code = cstr(weather, "code");
    const char *wlabel = weather_label(code);
    int temp = cint(weather, "temp_c", 0);
    if (wlabel) {
        s_weather_from_mqtt = true;
        net_time_set_weather_from_cloud(true);
        ui_set_weather(wlabel, temp, true);
    }
}


static void apply_peer_json(const char *json, int len)
{
    char *copy = malloc((size_t)len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, json, (size_t)len);
    copy[len] = 0;
    cJSON *root = cJSON_Parse(copy);
    free(copy);
    if (!root) {
        ESP_LOGW(TAG, "peer json bad");
        return;
    }

    bool online = cbool(root, "online", false);
    bool present = cbool(root, "present", false);
    ui_peer_status_t st = UI_PEER_OFFLINE;
    if (online) {
        st = present ? UI_PEER_PRESENT : UI_PEER_AWAY;
    }

    const char *wlabel = NULL;
    int temp = 0;
    bool wok = false;
    cJSON *weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    apply_weather_obj(weather);
    if (cJSON_IsObject(weather)) {
        const char *code = cstr(weather, "code");
        wlabel = weather_label(code);
        temp = cint(weather, "temp_c", 0);
        wok = wlabel != NULL;
    }
    ui_set_peer(st, wok ? wlabel : NULL, temp, wok);
    ESP_LOGI(TAG, "peer online=%d present=%d weather=%s", (int)online, (int)present,
             wok ? wlabel : "-");
    cJSON_Delete(root);
}

static void apply_motion_json(const char *json, int len)
{
    char *copy = malloc((size_t)len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, json, (size_t)len);
    copy[len] = 0;
    cJSON *root = cJSON_Parse(copy);
    free(copy);
    if (!root) {
        ESP_LOGW(TAG, "motion json bad");
        return;
    }

    const cJSON *jog = cJSON_GetObjectItemCaseSensitive(root, "jog");
    const char *preset = cstr(root, "preset");
    if (cJSON_IsObject(jog)) {
        s_motion_is_jog = true;
        s_motion_lift = (float)cint(jog, "lift_pct", 0);
        s_motion_pan = (float)cint(jog, "pan_deg", 0);
        ESP_LOGI(TAG, "queue motion jog lift=%.0f pan=%.0f", (double)s_motion_lift,
                 (double)s_motion_pan);
    } else if (preset && preset[0]) {
        s_motion_is_jog = false;
        snprintf(s_motion_preset, sizeof(s_motion_preset), "%s", preset);
        s_motion_repeat = cint(root, "repeat", 1);
        ESP_LOGI(TAG, "queue motion preset=%s repeat=%d", s_motion_preset, s_motion_repeat);
    } else {
        ESP_LOGW(TAG, "motion missing preset/jog");
        cJSON_Delete(root);
        return;
    }
    cJSON_Delete(root);
    if (s_ev) {
        xEventGroupSetBits(s_ev, BIT_MOTION);
    }
}

static void run_queued_motion(void)
{
    if (s_motion_is_jog) {
        (void)motion_cloud_jog(s_motion_lift, s_motion_pan);
    } else {
        (void)motion_cloud_preset(s_motion_preset, s_motion_repeat);
    }
}

static void apply_cmd_json(const char *json, int len)
{
    char *copy = malloc((size_t)len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, json, (size_t)len);
    copy[len] = 0;
    cJSON *root = cJSON_Parse(copy);
    free(copy);
    if (!root) {
        ESP_LOGW(TAG, "cmd json bad");
        return;
    }
    const char *type = cstr(root, "type");
    apply_weather_obj(cJSON_GetObjectItemCaseSensitive(root, "weather"));
    if (type && !strcmp(type, "set")) {
        const cJSON *me = cJSON_GetObjectItemCaseSensitive(root, "motion_enabled");
        if (cJSON_IsBool(me)) {
            /* Session only: do not persist. Power-on always resumes radar follow. */
            if (cJSON_IsTrue(me)) {
                motion_set_follow_enabled(true);
            } else {
                /* Keep pose; SoftAP Stop follow still slow-homes via set_follow(false). */
                motion_claim_servos();
            }
        }
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "stealth");
        if (cJSON_IsBool(st)) {
            s_stealth = cJSON_IsTrue(st);
            ESP_LOGI(TAG, "cmd stealth=%d", (int)s_stealth);
            if (s_stealth) {
                presence_reset();
            }
        }
    } else if (type && !strcmp(type, "home")) {
        /* Run on cloud_task via the motion queue — do not block the MQTT callback. */
        snprintf(s_motion_preset, sizeof(s_motion_preset), "hide");
        s_motion_repeat = 1;
        s_motion_is_jog = false;
        if (s_ev) {
            xEventGroupSetBits(s_ev, BIT_MOTION);
        }
    } else if (type && !strcmp(type, "clear_unread")) {
        ui_set_unread(0);
        if (s_last_msg_id[0]) {
            nvs_save_displayed(s_last_msg_id);
        }
    } else {
        ESP_LOGI(TAG, "cmd type=%s ignored this slice", type ? type : "?");
    }
    cJSON_Delete(root);
}

static void apply_inbox_json(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return;
    }
    int unread = cint(root, "unread", 0);
    cJSON *latest = cJSON_GetObjectItemCaseSensitive(root, "latest");
    if (cJSON_IsObject(latest)) {
        apply_msg_obj(latest);
        if (msg_already_shown(s_last_msg_id)) {
            unread = 0;
            publish_ack("msg", s_last_msg_id, "displayed", "");
        }
    }
    ui_set_unread(unread);
    ESP_LOGI(TAG, "inbox unread=%d", unread);
    cJSON_Delete(root);
}

static void fetch_inbox(void)
{
    device_cfg_t cfg;
    if (device_cfg_load(&cfg) != ESP_OK || !cfg.device_id[0] || !cfg.mqtt_password[0]) {
        return;
    }
    prefer_sta_route();
    const char *base = cfg.http_base[0] ? cfg.http_base : device_cfg_effective_claim_base(&cfg);
    char raw_url[192];
    size_t bl = strlen(base);
    if (bl > 0 && base[bl - 1] == '/') {
        snprintf(raw_url, sizeof(raw_url), "%sv1/dev/%s/inbox/latest", base, cfg.device_id);
    } else {
        snprintf(raw_url, sizeof(raw_url), "%s/v1/dev/%s/inbox/latest", base, cfg.device_id);
    }
    char url[192];
    rewrite_device_http_url(url, sizeof(url), raw_url);
    char auth[80];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg.mqtt_password);

    char resp[1024];
    http_acc_t acc = {.buf = resp, .len = 0, .cap = (int)sizeof(resp)};
    resp[0] = 0;
    const bool tls = url_is_https(url);
    esp_http_client_config_t http = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 12000,
        .event_handler = http_evt,
        .user_data = &acc,
        .transport_type = tls ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) {
        return;
    }
    esp_http_client_set_header(client, "Authorization", auth);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "inbox %s HTTP %d", esp_err_to_name(err), status);
        return;
    }
    apply_inbox_json(resp);
}

static void publish_presence(bool present)
{
    if (!s_mqtt || !s_client_id[0]) {
        return;
    }
    char topic[80];
    snprintf(topic, sizeof(topic), "dev/%s/event", s_client_id);
    char body[96];
    snprintf(body, sizeof(body), "{\"ts\":%ld,\"type\":\"presence\",\"present\":%s}",
             (long)time(NULL), present ? "true" : "false");
    int mid = esp_mqtt_client_publish(s_mqtt, topic, body, 0, 1, 0);
    ESP_LOGI(TAG, "pub presence present=%d mid=%d", (int)present, mid);
    s_presence_reported = present;
    s_presence_reported_valid = true;
}

void mqtt_link_notify_touch(void)
{
    if (s_state != MQTT_LINK_UP || !s_mqtt || !s_client_id[0] || s_stealth) {
        return;
    }
    static uint32_t seq;
    seq++;
    char topic[80];
    snprintf(topic, sizeof(topic), "dev/%s/event", s_client_id);
    char body[128];
    snprintf(body, sizeof(body), "{\"ts\":%ld,\"id\":\"t%lu\",\"type\":\"touch\"}",
             (long)time(NULL), (unsigned long)seq);
    int mid = esp_mqtt_client_publish(s_mqtt, topic, body, 0, 1, 0);
    ESP_LOGI(TAG, "pub touch mid=%d", mid);
}

static void presence_tick(void)
{
    if (s_state != MQTT_LINK_UP || !s_mqtt) {
        return;
    }
    if (s_stealth) {
        if (!s_presence_reported_valid || s_presence_reported) {
            publish_presence(false);
        }
        return;
    }

    bool raw = false;
    radar_target_t target;
    if (radar_link_get_target(&target) == ESP_OK) {
        raw = target.present;
    }
    int64_t now = now_ms();
    if (s_presence_raw_since_ms == 0 || raw != s_presence_raw) {
        s_presence_raw = raw;
        s_presence_raw_since_ms = now;
    }
    int64_t need_ms = raw ? PRESENCE_ENTER_MS : PRESENCE_LEAVE_MS;
    if (now - s_presence_raw_since_ms < need_ms) {
        return;
    }
    if (!s_presence_reported_valid || s_presence_reported != raw) {
        publish_presence(raw);
    }
}

static void publish_state(void)
{
    if (!s_mqtt || !s_client_id[0]) {
        return;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    char topic[80];
    snprintf(topic, sizeof(topic), "dev/%s/state", s_client_id);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"ts\":%ld,\"online\":true,\"unread\":0,\"fw\":\"%s\",\"rssi\":%d,"
             "\"motion_enabled\":%s,\"stealth\":%s,\"quiet_active\":false,"
             "\"lift_pct\":%.0f,\"pan_deg\":%.0f}",
             (long)time(NULL), app && app->version[0] ? app->version : "0.1.0", rssi,
             motion_follow_enabled() ? "true" : "false", s_stealth ? "true" : "false",
             (double)servo_get_lift_pct(), (double)servo_get_pan_deg());
    int mid = esp_mqtt_client_publish(s_mqtt, topic, body, 0, 1, 1);
    ESP_LOGI(TAG, "pub state mid=%d", mid);
}

static void on_mqtt(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        snprintf(s_sub_topic, sizeof(s_sub_topic), "dev/%s/#", s_client_id);
        esp_mqtt_client_subscribe(s_mqtt, s_sub_topic, 1);
        set_state(MQTT_LINK_UP, NULL);
        presence_reset();
        publish_state();
        if (s_ev) {
            xEventGroupSetBits(s_ev, BIT_INBOX);
        }
        ESP_LOGI(TAG, "mqtt up sub %s", s_sub_topic);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_state == MQTT_LINK_UP) {
            set_state(MQTT_LINK_CONNECTING, "mqtt_drop");
        }
        ESP_LOGW(TAG, "mqtt disconnected");
        break;
    case MQTT_EVENT_DATA: {
        const char *slash = NULL;
        if (e->topic && e->topic_len > 0) {
            for (int i = e->topic_len - 1; i >= 0; --i) {
                if (e->topic[i] == '/') {
                    slash = e->topic + i + 1;
                    break;
                }
            }
        }
        int kind_len = slash ? (int)((e->topic + e->topic_len) - slash) : 0;
        if (slash && e->data && e->data_len > 0) {
            if (kind_len == 4 && memcmp(slash, "peer", 4) == 0) {
                apply_peer_json(e->data, e->data_len);
            } else if (kind_len == 3 && memcmp(slash, "cmd", 3) == 0) {
                apply_cmd_json(e->data, e->data_len);
            } else if (kind_len == 3 && memcmp(slash, "msg", 3) == 0) {
                char *copy = malloc((size_t)e->data_len + 1);
                if (copy) {
                    memcpy(copy, e->data, (size_t)e->data_len);
                    copy[e->data_len] = 0;
                    cJSON *root = cJSON_Parse(copy);
                    free(copy);
                    if (root) {
                        apply_msg_obj(root);
                        ui_set_unread(msg_already_shown(s_last_msg_id) ? 0 : 1);
                        cJSON_Delete(root);
                    }
                }
            } else if (kind_len == 6 && memcmp(slash, "motion", 6) == 0) {
                apply_motion_json(e->data, e->data_len);
            } else {
                ESP_LOGI(TAG, "mqtt %.*s (%d B) ignored this slice", kind_len, slash, e->data_len);
            }
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        set_state(MQTT_LINK_ERR, "mqtt_err");
        break;
    default:
        break;
    }
}

static void mqtt_stop(void)
{
    if (!s_mqtt) {
        return;
    }
    esp_mqtt_client_stop(s_mqtt);
    esp_mqtt_client_destroy(s_mqtt);
    s_mqtt = NULL;
    presence_reset();
}

static esp_err_t mqtt_start(const device_cfg_t *cfg)
{
    if (!cfg->claimed || !cfg->mqtt_host[0] || !cfg->mqtt_username[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->mqtt_tls && cfg->sntp_before_tls && !s_sntp_ok && !net_time_clock_ok()) {
        set_state(MQTT_LINK_SNTP, "need_sntp");
        return ESP_ERR_INVALID_STATE;
    }

    mqtt_stop();

    snprintf(s_client_id, sizeof(s_client_id), "%s", cfg->device_id);
    snprintf(s_user, sizeof(s_user), "%s", cfg->mqtt_username);
    snprintf(s_pass, sizeof(s_pass), "%s", cfg->mqtt_password);
    snprintf(s_lwt_topic, sizeof(s_lwt_topic), "dev/%s/state", s_client_id);

    const bool tls = cfg->mqtt_tls || strcmp(cfg->mqtt_transport, "mqtts") == 0;
    uint16_t port = cfg->mqtt_port ? cfg->mqtt_port : (tls ? 8883 : 1883);
    snprintf(s_uri, sizeof(s_uri), "%s://%s:%u", tls ? "mqtts" : "mqtt", cfg->mqtt_host,
             (unsigned)port);

    set_state(MQTT_LINK_CONNECTING, NULL);

    esp_mqtt_client_config_t mqtt = {
        .broker.address.uri = s_uri,
        .credentials.client_id = s_client_id,
        .credentials.username = s_user,
        .credentials.authentication.password = s_pass,
        .session.keepalive = 30,
        .session.disable_clean_session = false,
        .session.last_will.topic = s_lwt_topic,
        .session.last_will.msg = s_lwt_msg,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };
    if (tls) {
        mqtt.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_mqtt = esp_mqtt_client_init(&mqtt);
    if (!s_mqtt) {
        set_state(MQTT_LINK_ERR, "mqtt_init");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    esp_err_t err = esp_mqtt_client_start(s_mqtt);
    if (err != ESP_OK) {
        mqtt_stop();
        set_state(MQTT_LINK_ERR, "mqtt_start");
        return err;
    }
    ESP_LOGI(TAG, "mqtt start %s as %s", s_uri, s_client_id);
    return ESP_OK;
}

static bool sta_has_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        return false;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

static void cloud_task(void *arg)
{
    (void)arg;
    set_state(MQTT_LINK_WAIT_IP, NULL);
    if (sta_has_ip() && s_ev) {
        xEventGroupSetBits(s_ev, BIT_IP);
    }

    xEventGroupWaitBits(s_ev, BIT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    prefer_sta_route();
    sntp_sync();

    char last_bind[DEVICE_CFG_BIND_MAX + 1] = {0};
    bool mqtt_armed = false;

    for (;;) {
        TickType_t wait = (s_state == MQTT_LINK_UP) ? pdMS_TO_TICKS(250) : pdMS_TO_TICKS(5000);
        EventBits_t bits =
            xEventGroupWaitBits(s_ev, BIT_KICK | BIT_INBOX | BIT_MEDIA | BIT_MOTION, pdTRUE,
                                pdFALSE, wait);
        presence_tick();

        if (!sta_has_ip()) {
            if (s_state == MQTT_LINK_UP || s_state == MQTT_LINK_CONNECTING) {
                mqtt_stop();
                mqtt_armed = false;
                set_state(MQTT_LINK_WAIT_IP, "no_ip");
            }
            continue;
        }
        prefer_sta_route();

        if ((bits & BIT_INBOX) && s_state == MQTT_LINK_UP) {
            fetch_inbox();
        }
        if ((bits & BIT_MEDIA) && s_state == MQTT_LINK_UP) {
            fetch_media();
        }
        if ((bits & BIT_MOTION) && s_state == MQTT_LINK_UP) {
            run_queued_motion();
            publish_state();
        }

        if (bits & BIT_KICK) {
            mqtt_stop();
            mqtt_armed = false;
        }

        if (!s_sntp_ok) {
            (void)sntp_sync();
        }

        device_cfg_t cfg;
        if (device_cfg_load(&cfg) != ESP_OK) {
            continue;
        }
        /* Level 3 is optional: without your own server URL this task just idles. */
        if (!device_cfg_effective_claim_base(&cfg)[0]) {
            if (s_state != MQTT_LINK_IDLE) {
                mqtt_stop();
                mqtt_armed = false;
                set_state(MQTT_LINK_IDLE, NULL);
            }
            continue;
        }

        if (cfg.bind_code[0] && last_bind[0] && strcmp(cfg.bind_code, last_bind) != 0) {
            ESP_LOGI(TAG, "bind changed, clear claim");
            device_cfg_clear_claim();
            mqtt_stop();
            mqtt_armed = false;
            cfg.claimed = false;
        }
        if (cfg.bind_code[0]) {
            snprintf(last_bind, sizeof(last_bind), "%s", cfg.bind_code);
        }

        if (!cfg.claimed) {
            if (!bind_ok(cfg.bind_code)) {
                if (s_state != MQTT_LINK_WAIT_IP && s_state != MQTT_LINK_SNTP) {
                    set_state(MQTT_LINK_WAIT_IP, "need_bind");
                }
                continue;
            }
            if (do_claim(&cfg) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(8000));
                continue;
            }
            if (device_cfg_load(&cfg) != ESP_OK || !cfg.claimed) {
                continue;
            }
            mqtt_armed = false;
        }

        if (cfg.claimed && !mqtt_armed) {
            if (mqtt_start(&cfg) == ESP_OK) {
                mqtt_armed = true;
            }
        }
    }
}

esp_err_t mqtt_link_start(void)
{
    if (s_ev) {
        return ESP_OK;
    }
    s_ev = xEventGroupCreate();
    if (!s_ev) {
        return ESP_ERR_NO_MEM;
    }
    nvs_load_displayed();
    set_state(MQTT_LINK_WAIT_IP, NULL);
    esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_sta_ip, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    BaseType_t ok = xTaskCreate(cloud_task, "cloud", 16384, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
