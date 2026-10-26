#include "net_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "device_cfg.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "net_http.h"
#include "sdkconfig.h"
#include "ui.h"

static const char *TAG = "net_time";

#define BIT_IP   BIT0
#define BIT_KICK BIT1
#define TIME_OK_EPOCH 1700000000 /* 2023-11 — reject 1970 */
#define WEATHER_PERIOD_S 900

static EventGroupHandle_t s_ev;
static bool s_online;
static bool s_sntp_started;
static bool s_weather_from_cloud;
static time_t s_weather_due;
static char s_err[48];

static void set_err(const char *e)
{
    snprintf(s_err, sizeof(s_err), "%s", e ? e : "");
}

const char *net_time_last_error(void)
{
    return s_err;
}

bool net_time_online(void)
{
    return s_online;
}

bool net_time_clock_ok(void)
{
    return time(NULL) > TIME_OK_EPOCH;
}

void net_time_kick(void)
{
    if (s_ev) {
        xEventGroupSetBits(s_ev, BIT_KICK);
    }
}

void net_time_set_weather_from_cloud(bool on)
{
    s_weather_from_cloud = on;
}

static bool sta_has_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
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
        xEventGroupSetBits(s_ev, BIT_IP);
    }
}

/** POSIX TZ has the sign inverted: UTC+8 is "UTC-8". */
static void apply_utc_offset(int offset_min)
{
    char tz[24];
    int a = offset_min < 0 ? -offset_min : offset_min;
    snprintf(tz, sizeof(tz), "UTC%c%d:%02d", offset_min >= 0 ? '-' : '+', a / 60, a % 60);
    setenv("TZ", tz, 1);
    tzset();
}

static void clock_push(void)
{
    if (!net_time_clock_ok()) {
        ui_set_clock(false, 0, 0);
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    ui_set_clock(true, tm.tm_hour, tm.tm_min);
}

static void sntp_sync(void)
{
    if (!s_sntp_started) {
#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 3
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            3, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com", "time.cloudflare.com"));
#else
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
#endif
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "sntp init %s", esp_err_to_name(err));
            return;
        }
        s_sntp_started = true;
    }
    esp_err_t wait = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(12000));
    ESP_LOGI(TAG, "sntp %s", (wait == ESP_OK || net_time_clock_ok()) ? "ok" : "timeout");
    clock_push();
}

/** Same labels ui.c maps to icons. */
static const char *wmo_label(int code)
{
    if (code == 0 || code == 1) {
        return "sunny";
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95) {
        return "rain";
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return "snow";
    }
    return "cloudy";
}

static bool geocode(const device_cfg_t *cfg)
{
    char q[3 * DEVICE_CFG_CITY_MAX + 1];
    if (!net_http_urlencode(cfg->city, q, sizeof(q))) {
        set_err("city_too_long");
        return false;
    }
    char url[256];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&format=json", q);
    char *resp = malloc(2048);
    if (!resp) {
        return false;
    }
    bool ok = false;
    if (net_http_get(url, resp, 2048, 12000)) {
        cJSON *root = cJSON_Parse(resp);
        cJSON *res = root ? cJSON_GetObjectItemCaseSensitive(root, "results") : NULL;
        cJSON *first = cJSON_IsArray(res) ? cJSON_GetArrayItem(res, 0) : NULL;
        cJSON *jlat = first ? cJSON_GetObjectItemCaseSensitive(first, "latitude") : NULL;
        cJSON *jlon = first ? cJSON_GetObjectItemCaseSensitive(first, "longitude") : NULL;
        if (cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon)) {
            cJSON *jname = cJSON_GetObjectItemCaseSensitive(first, "name");
            cJSON *jcc = cJSON_GetObjectItemCaseSensitive(first, "country_code");
            char name[DEVICE_CFG_CITY_MAX + 1];
            int used = snprintf(name, sizeof(name), "%.40s",
                                cJSON_IsString(jname) ? jname->valuestring : cfg->city);
            if (cJSON_IsString(jcc) && used > 0 && used < 40) {
                snprintf(name + used, sizeof(name) - (size_t)used, ", %.2s", jcc->valuestring);
            }
            ok = device_cfg_save_place(name, (float)jlat->valuedouble,
                                       (float)jlon->valuedouble) == ESP_OK;
            ESP_LOGI(TAG, "city \"%s\" → %s %.2f,%.2f", cfg->city, name, jlat->valuedouble,
                     jlon->valuedouble);
        } else {
            set_err("city_not_found");
        }
        cJSON_Delete(root);
    } else {
        set_err("geocode_net");
    }
    free(resp);
    return ok;
}

static void fetch_weather(const device_cfg_t *cfg)
{
    char url[224];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
             "&current=temperature_2m,weather_code&timezone=auto",
             (double)cfg->lat, (double)cfg->lon);
    char *resp = malloc(1536);
    if (!resp) {
        return;
    }
    if (!net_http_get(url, resp, 1536, 12000)) {
        set_err("weather_net");
        free(resp);
        return;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        set_err("weather_json");
        return;
    }
    cJSON *off = cJSON_GetObjectItemCaseSensitive(root, "utc_offset_seconds");
    if (cJSON_IsNumber(off)) {
        int min = (int)off->valuedouble / 60;
        apply_utc_offset(min);
        if (min != cfg->utc_offset_min) {
            (void)device_cfg_save_utc_offset(min);
        }
    }
    cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON *code = cur ? cJSON_GetObjectItemCaseSensitive(cur, "weather_code") : NULL;
    cJSON *temp = cur ? cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m") : NULL;
    if (cJSON_IsNumber(code) && cJSON_IsNumber(temp) && !s_weather_from_cloud) {
        int t = (int)(temp->valuedouble + (temp->valuedouble >= 0 ? 0.5 : -0.5));
        ui_set_weather(wmo_label((int)code->valuedouble), t, true);
        set_err("");
        ESP_LOGI(TAG, "weather %s %dC", wmo_label((int)code->valuedouble), t);
    }
    cJSON_Delete(root);
    clock_push();
}

static void net_task(void *arg)
{
    (void)arg;
    device_cfg_t cfg;
    if (device_cfg_load(&cfg) == ESP_OK) {
        apply_utc_offset(cfg.utc_offset_min);
    }

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_IP | BIT_KICK, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(5000));
        bool online = sta_has_ip();
        if (online != s_online) {
            s_online = online;
            ui_set_cloud_linked(online);
            ESP_LOGI(TAG, "online=%d", (int)online);
        }
        clock_push();
        if (!online) {
            continue;
        }
        prefer_sta_route();
        if (!net_time_clock_ok() || (bits & BIT_IP)) {
            sntp_sync();
        }
        if (device_cfg_load(&cfg) != ESP_OK || !cfg.city[0]) {
            continue;
        }
        if (bits & BIT_KICK) {
            s_weather_due = 0;
        }
        if (!cfg.place_ok) {
            if (!geocode(&cfg) || device_cfg_load(&cfg) != ESP_OK || !cfg.place_ok) {
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
        }
        time_t now = time(NULL);
        if (s_weather_due == 0 || now >= s_weather_due) {
            fetch_weather(&cfg);
            s_weather_due = now + WEATHER_PERIOD_S;
        }
    }
}

esp_err_t net_time_start(void)
{
    if (s_ev) {
        return ESP_OK;
    }
    s_ev = xEventGroupCreate();
    if (!s_ev) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_sta_ip, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return xTaskCreate(net_task, "net_time", 8192, NULL, 4, NULL) == pdPASS ? ESP_OK
                                                                         : ESP_ERR_NO_MEM;
}
