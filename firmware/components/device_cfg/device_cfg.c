#include "device_cfg.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "device_cfg";

static bool claim_base_is_set(const char *s)
{
    return s && (!strncmp(s, "http://", 7) || !strncmp(s, "https://", 8));
}

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = 0;
}

static uint16_t clamp_u16(int v, int lo, int hi)
{
    if (v < lo) {
        return (uint16_t)lo;
    }
    if (v > hi) {
        return (uint16_t)hi;
    }
    return (uint16_t)v;
}

static esp_err_t ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t open_rw(nvs_handle_t *h)
{
    ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs");
    return nvs_open(DEVICE_CFG_NS, NVS_READWRITE, h);
}

static esp_err_t get_str(nvs_handle_t h, const char *key, char *buf, size_t buflen)
{
    size_t len = buflen;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = 0;
        return ESP_OK;
    }
    return err;
}

esp_err_t device_cfg_load(device_cfg_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->mqtt_port = 1883;
    copy_str(out->mqtt_transport, sizeof(out->mqtt_transport), "mqtt");
    out->sntp_before_tls = true;
    out->follow_enabled = true;
    out->near_cm = DEVICE_CFG_NEAR_CM;
    out->near_exit_cm = DEVICE_CFG_NEAR_EXIT_CM;
    out->far_cm = DEVICE_CFG_FAR_CM;
    out->hide_s = DEVICE_CFG_HIDE_S;
    out->rise_ms = DEVICE_CFG_RISE_MS;
    out->pan_slew_dps = DEVICE_CFG_PAN_DPS;
    out->pan_max_deg = DEVICE_CFG_PAN_MAX;

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");

    esp_err_t err = ESP_OK;
    err = err ? err : get_str(h, "wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
    err = err ? err : get_str(h, "wifi_pass", out->wifi_pass, sizeof(out->wifi_pass));
    err = err ? err : get_str(h, "bind", out->bind_code, sizeof(out->bind_code));
    err = err ? err : get_str(h, "claim_base", out->claim_base, sizeof(out->claim_base));
    err = err ? err : get_str(h, "lang", out->lang, sizeof(out->lang));
    err = err ? err : get_str(h, "gender", out->gender, sizeof(out->gender));
    uint8_t flip = 0;
    if (nvs_get_u8(h, "scr_flip", &flip) == ESP_OK) {
        out->screen_flip = flip ? 1 : 0;
    }
    err = err ? err : get_str(h, "device_id", out->device_id, sizeof(out->device_id));
    err = err ? err : get_str(h, "pair_id", out->pair_id, sizeof(out->pair_id));
    err = err ? err : get_str(h, "http_base", out->http_base, sizeof(out->http_base));
    err = err ? err : get_str(h, "mqtt_host", out->mqtt_host, sizeof(out->mqtt_host));
    err = err ? err : get_str(h, "mqtt_user", out->mqtt_username, sizeof(out->mqtt_username));
    err = err ? err : get_str(h, "mqtt_pass", out->mqtt_password, sizeof(out->mqtt_password));
    err = err ? err : get_str(h, "mqtt_tr", out->mqtt_transport, sizeof(out->mqtt_transport));

    uint16_t port = 0;
    if (nvs_get_u16(h, "mqtt_port", &port) == ESP_OK) {
        out->mqtt_port = port;
    }
    uint8_t u8 = 0;
    if (nvs_get_u8(h, "mqtt_tls", &u8) == ESP_OK) {
        out->mqtt_tls = u8 != 0;
    }
    if (nvs_get_u8(h, "sntp_tls", &u8) == ESP_OK) {
        out->sntp_before_tls = u8 != 0;
    }
    /* `follow` is session-only; ignore any leftover NVS key. */
    uint16_t u16 = 0;
    if (nvs_get_u16(h, "near_cm", &u16) == ESP_OK) {
        out->near_cm = u16;
    }
    if (nvs_get_u16(h, "nexit_cm", &u16) == ESP_OK) {
        out->near_exit_cm = u16;
    }
    if (nvs_get_u16(h, "far_cm", &u16) == ESP_OK) {
        out->far_cm = u16;
    }
    if (nvs_get_u16(h, "hide_s", &u16) == ESP_OK) {
        out->hide_s = u16;
    }
    if (nvs_get_u16(h, "rise_ms", &u16) == ESP_OK) {
        out->rise_ms = u16;
    }
    if (nvs_get_u16(h, "pan_dps", &u16) == ESP_OK) {
        out->pan_slew_dps = u16;
    }
    if (nvs_get_u16(h, "pan_max", &u16) == ESP_OK) {
        out->pan_max_deg = u16;
    }

    err = err ? err : get_str(h, "city", out->city, sizeof(out->city));
    err = err ? err : get_str(h, "place", out->place_name, sizeof(out->place_name));
    err = err ? err : get_str(h, "tg_token", out->tg_token, sizeof(out->tg_token));
    err = err ? err : get_str(h, "tg_chat", out->tg_chat, sizeof(out->tg_chat));
    err = err ? err : get_str(h, "ap_pass", out->ap_pass, sizeof(out->ap_pass));
    int32_t i32 = 0;
    if (nvs_get_i32(h, "lat_e5", &i32) == ESP_OK) {
        out->lat = i32 / 100000.0f;
        out->place_ok = true;
    }
    if (nvs_get_i32(h, "lon_e5", &i32) == ESP_OK) {
        out->lon = i32 / 100000.0f;
    } else {
        out->place_ok = false;
    }
    int16_t i16 = 0;
    if (nvs_get_i16(h, "utc_off", &i16) == ESP_OK) {
        out->utc_offset_min = i16;
    }

    out->wifi_saved = out->wifi_ssid[0] != 0;
    out->claimed = out->device_id[0] != 0 && out->mqtt_username[0] != 0;

    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load partial err=%s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t device_cfg_save_wifi(const device_cfg_t *in)
{
    if (!in || !in->wifi_ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "wifi_ssid", in->wifi_ssid), TAG, "ssid");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "wifi_pass", in->wifi_pass), TAG, "pass");
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    ESP_LOGI(TAG, "wifi saved ssid=%s", in->wifi_ssid);
    return ESP_OK;
}

esp_err_t device_cfg_save_cloud(const char *bind, const char *claim_base)
{
    if (!bind || !bind[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claim_base_is_set(claim_base)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *base = claim_base;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "bind", bind), TAG, "bind");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "claim_base", base), TAG, "claim_base");
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    ESP_LOGI(TAG, "cloud saved bind=%s base=%s", bind, base);
    return ESP_OK;
}

esp_err_t device_cfg_save_prefs(const char *lang, const char *gender, bool screen_flip)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "lang", (lang && lang[0]) ? lang : "en"), TAG, "lang");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "gender", (gender && gender[0]) ? gender : "female"), TAG,
                        "gender");
    ESP_RETURN_ON_ERROR(nvs_set_u8(h, "scr_flip", screen_flip ? 1 : 0), TAG, "scr_flip");
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    ESP_LOGI(TAG, "prefs saved lang=%s gender=%s flip=%d", lang ? lang : "en",
             gender ? gender : "female", screen_flip ? 1 : 0);
    return ESP_OK;
}

esp_err_t device_cfg_erase_follow(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    esp_err_t err = nvs_erase_key(h, "follow");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "follow NVS key dropped (session-only)");
    }
    return err;
}

esp_err_t device_cfg_save_tune(const device_cfg_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t near_cm = clamp_u16((int)in->near_cm, 20, 250);
    uint16_t near_exit_cm = clamp_u16((int)in->near_exit_cm, (int)near_cm + 5, 300);
    uint16_t far_cm = clamp_u16((int)in->far_cm, (int)near_exit_cm + 20, 800);
    uint16_t hide_s = clamp_u16((int)in->hide_s, 3, 30);
    uint16_t rise_ms = clamp_u16((int)in->rise_ms, 100, 2000);
    uint16_t pan_slew = clamp_u16((int)in->pan_slew_dps, 20, 180);
    uint16_t pan_max = clamp_u16((int)in->pan_max_deg, 30, 90);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, "near_cm", near_cm), TAG, "near");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, "nexit_cm", near_exit_cm), TAG, "nexit");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, "far_cm", far_cm), TAG, "far");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, "hide_s", hide_s), TAG, "hide");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, "rise_ms", rise_ms), TAG, "rise");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, "pan_dps", pan_slew), TAG, "dps");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, "pan_max", pan_max), TAG, "pmax");
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    ESP_LOGI(TAG, "tune saved near=%ucm exit=%ucm far=%ucm hide=%us rise=%ums pan=%u°/s ±%u",
             (unsigned)near_cm, (unsigned)near_exit_cm, (unsigned)far_cm, (unsigned)hide_s,
             (unsigned)rise_ms, (unsigned)pan_slew, (unsigned)pan_max);
    return ESP_OK;
}

esp_err_t device_cfg_reset_tune(void)
{
    device_cfg_t cfg = {0};
    cfg.near_cm = DEVICE_CFG_NEAR_CM;
    cfg.near_exit_cm = DEVICE_CFG_NEAR_EXIT_CM;
    cfg.far_cm = DEVICE_CFG_FAR_CM;
    cfg.hide_s = DEVICE_CFG_HIDE_S;
    cfg.rise_ms = DEVICE_CFG_RISE_MS;
    cfg.pan_slew_dps = DEVICE_CFG_PAN_DPS;
    cfg.pan_max_deg = DEVICE_CFG_PAN_MAX;
    ESP_RETURN_ON_ERROR(device_cfg_save_tune(&cfg), TAG, "reset tune");
    (void)device_cfg_erase_follow();
    ESP_LOGI(TAG, "tune restored to factory");
    return ESP_OK;
}

esp_err_t device_cfg_save_claim(const device_cfg_t *in)
{
    if (!in || !in->device_id[0] || !in->mqtt_username[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "device_id", in->device_id), TAG, "device_id");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "pair_id", in->pair_id), TAG, "pair_id");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "http_base", in->http_base), TAG, "http_base");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "mqtt_host", in->mqtt_host), TAG, "mqtt_host");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, "mqtt_port", in->mqtt_port), TAG, "mqtt_port");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "mqtt_tr", in->mqtt_transport), TAG, "mqtt_tr");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "mqtt_user", in->mqtt_username), TAG, "mqtt_user");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "mqtt_pass", in->mqtt_password), TAG, "mqtt_pass");
    ESP_RETURN_ON_ERROR(nvs_set_u8(h, "mqtt_tls", in->mqtt_tls ? 1 : 0), TAG, "mqtt_tls");
    ESP_RETURN_ON_ERROR(nvs_set_u8(h, "sntp_tls", in->sntp_before_tls ? 1 : 0), TAG, "sntp");
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    ESP_LOGI(TAG, "claim saved device_id=%s host=%s:%u %s", in->device_id, in->mqtt_host,
             (unsigned)in->mqtt_port, in->mqtt_transport);
    return ESP_OK;
}

esp_err_t device_cfg_clear_claim(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    nvs_erase_key(h, "device_id");
    nvs_erase_key(h, "pair_id");
    nvs_erase_key(h, "http_base");
    nvs_erase_key(h, "mqtt_host");
    nvs_erase_key(h, "mqtt_port");
    nvs_erase_key(h, "mqtt_tr");
    nvs_erase_key(h, "mqtt_user");
    nvs_erase_key(h, "mqtt_pass");
    nvs_erase_key(h, "mqtt_tls");
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    ESP_LOGI(TAG, "claim cleared");
    return ESP_OK;
}

const char *device_cfg_effective_claim_base(const device_cfg_t *cfg)
{
    if (cfg && claim_base_is_set(cfg->claim_base)) {
        return cfg->claim_base;
    }
    return "";
}

esp_err_t device_cfg_save_city(const char *city, int utc_offset_min)
{
    device_cfg_t prev;
    (void)device_cfg_load(&prev);
    const char *c = city ? city : "";
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "city", c), TAG, "city");
    if (strcmp(prev.city, c) != 0) {
        nvs_erase_key(h, "lat_e5");
        nvs_erase_key(h, "lon_e5");
        nvs_erase_key(h, "place");
    }
    if (utc_offset_min >= -14 * 60 && utc_offset_min <= 14 * 60) {
        ESP_RETURN_ON_ERROR(nvs_set_i16(h, "utc_off", (int16_t)utc_offset_min), TAG, "utc");
    }
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    ESP_LOGI(TAG, "city saved \"%s\" utc_off=%d", c, utc_offset_min);
    return ESP_OK;
}

esp_err_t device_cfg_save_place(const char *place_name, float lat, float lon)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "place", place_name ? place_name : ""), TAG, "place");
    ESP_RETURN_ON_ERROR(nvs_set_i32(h, "lat_e5", (int32_t)(lat * 100000.0f)), TAG, "lat");
    ESP_RETURN_ON_ERROR(nvs_set_i32(h, "lon_e5", (int32_t)(lon * 100000.0f)), TAG, "lon");
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    return ESP_OK;
}

esp_err_t device_cfg_save_utc_offset(int utc_offset_min)
{
    if (utc_offset_min < -14 * 60 || utc_offset_min > 14 * 60) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_i16(h, "utc_off", (int16_t)utc_offset_min), TAG, "utc");
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    return ESP_OK;
}

esp_err_t device_cfg_save_tg_token(const char *token)
{
    device_cfg_t prev;
    (void)device_cfg_load(&prev);
    const char *t = token ? token : "";
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "tg_token", t), TAG, "tg_token");
    if (strcmp(prev.tg_token, t) != 0) {
        nvs_erase_key(h, "tg_chat");
    }
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    ESP_LOGI(TAG, "telegram token %s", t[0] ? "saved" : "cleared");
    return ESP_OK;
}

esp_err_t device_cfg_save_tg_chat(const char *chat_id)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(open_rw(&h), TAG, "open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "tg_chat", chat_id ? chat_id : ""), TAG, "tg_chat");
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
    nvs_close(h);
    return ESP_OK;
}

const char *device_cfg_ap_password(void)
{
    static char s_ap_pass[DEVICE_CFG_AP_PASS_LEN + 1];
    if (s_ap_pass[0]) {
        return s_ap_pass;
    }
    nvs_handle_t h;
    if (open_rw(&h) != ESP_OK) {
        return "deskbuddy";
    }
    if (get_str(h, "ap_pass", s_ap_pass, sizeof(s_ap_pass)) != ESP_OK ||
        strlen(s_ap_pass) != DEVICE_CFG_AP_PASS_LEN) {
        /* No 0/O/1/l/I so it is easy to read off the round screen. */
        static const char alphabet[] = "23456789abcdefghjkmnpqrstuvwxyz";
        for (int i = 0; i < DEVICE_CFG_AP_PASS_LEN; ++i) {
            s_ap_pass[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
        }
        s_ap_pass[DEVICE_CFG_AP_PASS_LEN] = 0;
        if (nvs_set_str(h, "ap_pass", s_ap_pass) == ESP_OK) {
            (void)nvs_commit(h);
        }
        ESP_LOGI(TAG, "SoftAP password created");
    }
    nvs_close(h);
    return s_ap_pass;
}
