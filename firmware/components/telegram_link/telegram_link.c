#include "telegram_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_cfg.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "motion.h"
#include "net_http.h"
#include "net_time.h"
#include "ui.h"

static const char *TAG = "telegram";

#define API "https://api.telegram.org/bot"
#define POLL_S 25
#define RESP_CAP 6144
#define BIT_KICK BIT0
#define BIT_SEEN BIT1

static EventGroupHandle_t s_ev;
static const char *s_state = "off";
static char s_bot[48];
static char s_token_checked[DEVICE_CFG_TG_TOKEN_MAX + 1];
static bool s_waiting_seen;

const char *telegram_link_state_str(void)
{
    return s_state;
}

const char *telegram_link_bot_username(void)
{
    return s_bot;
}

void telegram_link_kick(void)
{
    if (s_ev) {
        xEventGroupSetBits(s_ev, BIT_KICK);
    }
}

void telegram_link_notify_msg_displayed(void)
{
    if (s_ev && s_waiting_seen) {
        xEventGroupSetBits(s_ev, BIT_SEEN);
    }
}

/** Bot tokens look like 123456789:AA... — reject anything else before it reaches a URL. */
static bool token_ok(const char *t)
{
    const char *colon = strchr(t, ':');
    if (!colon || colon == t || strlen(colon + 1) < 20) {
        return false;
    }
    for (const char *p = t; *p; ++p) {
        bool ok = (*p >= '0' && *p <= '9') || (p > colon && ((*p >= 'A' && *p <= 'Z') ||
                                                             (*p >= 'a' && *p <= 'z') ||
                                                             *p == '_' || *p == '-')) ||
                  p == colon;
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool api_get(const char *token, const char *method_and_query, char *resp, int cap,
                    int timeout_ms)
{
    char url[512];
    int n = snprintf(url, sizeof(url), API "%s/%s", token, method_and_query);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return false;
    }
    return net_http_get(url, resp, cap, timeout_ms);
}

static void send_text(const char *token, const char *chat, const char *text)
{
    char enc[600];
    if (!net_http_urlencode(text, enc, sizeof(enc))) {
        return;
    }
    char q[700];
    snprintf(q, sizeof(q), "sendMessage?chat_id=%s&text=%s", chat, enc);
    char resp[512];
    (void)api_get(token, q, resp, sizeof(resp), 10000);
}

static bool check_bot(const char *token)
{
    char resp[512];
    if (!api_get(token, "getMe", resp, sizeof(resp), 10000)) {
        return false;
    }
    cJSON *root = cJSON_Parse(resp);
    cJSON *res = root ? cJSON_GetObjectItemCaseSensitive(root, "result") : NULL;
    cJSON *user = res ? cJSON_GetObjectItemCaseSensitive(res, "username") : NULL;
    bool ok = cJSON_IsString(user);
    if (ok) {
        snprintf(s_bot, sizeof(s_bot), "%s", user->valuestring);
        ESP_LOGI(TAG, "bot @%s", s_bot);
    }
    cJSON_Delete(root);
    return ok;
}

static void handle_message(const char *token, device_cfg_t *cfg, const char *chat,
                           const char *text)
{
    const bool paired = cfg->tg_chat[0] != 0;
    const bool is_start = !strncmp(text, "/start", 6);

    if (!paired) {
        if (!is_start) {
            send_text(token, chat, "Send /start to pair this chat with your Desk Buddy.");
            return;
        }
        if (device_cfg_save_tg_chat(chat) == ESP_OK) {
            snprintf(cfg->tg_chat, sizeof(cfg->tg_chat), "%s", chat);
            s_state = "paired";
            send_text(token, chat,
                      "Paired. Send any text and it appears on the screen. "
                      "Commands: /wave /peek /unpair");
        }
        return;
    }
    if (strcmp(chat, cfg->tg_chat) != 0) {
        ESP_LOGW(TAG, "ignored message from another chat");
        return;
    }
    if (is_start) {
        send_text(token, chat, "Already paired with this chat.");
    } else if (!strncmp(text, "/unpair", 7)) {
        (void)device_cfg_save_tg_chat("");
        cfg->tg_chat[0] = 0;
        s_state = "wait_pair";
        send_text(token, chat, "Unpaired. Send /start from the chat you want to use.");
    } else if (!strncmp(text, "/wave", 5)) {
        (void)motion_cloud_preset("wave", 1);
    } else if (!strncmp(text, "/peek", 5)) {
        (void)motion_cloud_preset("peek", 1);
    } else if (text[0] == '/') {
        send_text(token, chat, "Commands: /wave /peek /unpair. Any other text goes to the screen.");
    } else {
        ui_clear_msg_image();
        ui_set_msg_text(text);
        ui_set_unread(1);
        ui_poke_wake(15000);
        s_waiting_seen = true;
        send_text(token, chat, "Delivered. It shows when someone walks up to the desk.");
    }
}

static void tg_task(void *arg)
{
    (void)arg;
    char *resp = heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = malloc(RESP_CAP);
    }
    if (!resp) {
        s_state = "err";
        vTaskDelete(NULL);
        return;
    }
    long long offset = 0;

    for (;;) {
        device_cfg_t cfg;
        if (device_cfg_load(&cfg) != ESP_OK || !cfg.tg_token[0]) {
            s_state = "off";
            s_bot[0] = 0;
            xEventGroupWaitBits(s_ev, BIT_KICK, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
            continue;
        }
        if (!token_ok(cfg.tg_token)) {
            s_state = "bad_token";
            xEventGroupWaitBits(s_ev, BIT_KICK, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
            continue;
        }
        if (!net_time_online() || !net_time_clock_ok()) {
            s_state = "wait_wifi";
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        if (strcmp(s_token_checked, cfg.tg_token) != 0) {
            if (!check_bot(cfg.tg_token)) {
                s_state = "bad_token";
                xEventGroupWaitBits(s_ev, BIT_KICK, pdTRUE, pdFALSE, pdMS_TO_TICKS(60000));
                continue;
            }
            snprintf(s_token_checked, sizeof(s_token_checked), "%s", cfg.tg_token);
            offset = 0;
        }
        s_state = cfg.tg_chat[0] ? "paired" : "wait_pair";

        if (xEventGroupGetBits(s_ev) & BIT_SEEN) {
            xEventGroupClearBits(s_ev, BIT_SEEN);
            s_waiting_seen = false;
            if (cfg.tg_chat[0]) {
                send_text(cfg.tg_token, cfg.tg_chat, "Seen on the screen.");
            }
        }

        char q[128];
        snprintf(q, sizeof(q), "getUpdates?timeout=%d&limit=1&offset=%lld", POLL_S, offset);
        if (!api_get(cfg.tg_token, q, resp, RESP_CAP, (POLL_S + 10) * 1000)) {
            s_state = "err";
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* Read update_id first so an oversized (truncated) update is still skipped. */
        const char *uid = strstr(resp, "\"update_id\":");
        if (!uid) {
            continue;
        }
        long long update_id = atoll(uid + 12);
        offset = update_id + 1;

        cJSON *root = cJSON_Parse(resp);
        cJSON *res = root ? cJSON_GetObjectItemCaseSensitive(root, "result") : NULL;
        cJSON *upd = cJSON_IsArray(res) ? cJSON_GetArrayItem(res, 0) : NULL;
        cJSON *msg = upd ? cJSON_GetObjectItemCaseSensitive(upd, "message") : NULL;
        cJSON *chat = msg ? cJSON_GetObjectItemCaseSensitive(msg, "chat") : NULL;
        cJSON *chat_id = chat ? cJSON_GetObjectItemCaseSensitive(chat, "id") : NULL;
        cJSON *text = msg ? cJSON_GetObjectItemCaseSensitive(msg, "text") : NULL;
        if (cJSON_IsNumber(chat_id) && cJSON_IsString(text)) {
            char chat_s[DEVICE_CFG_TG_CHAT_MAX + 1];
            snprintf(chat_s, sizeof(chat_s), "%.0f", chat_id->valuedouble);
            handle_message(cfg.tg_token, &cfg, chat_s, text->valuestring);
        }
        cJSON_Delete(root);
    }
}

esp_err_t telegram_link_start(void)
{
    if (s_ev) {
        return ESP_OK;
    }
    s_ev = xEventGroupCreate();
    if (!s_ev) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(tg_task, "telegram", 8192, NULL, 3, NULL) == pdPASS ? ESP_OK
                                                                         : ESP_ERR_NO_MEM;
}
