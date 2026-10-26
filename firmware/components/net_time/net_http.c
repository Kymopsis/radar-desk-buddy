#include "net_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "net_http";

typedef struct {
    char *buf;
    int len;
    int cap;
} http_acc_t;

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

bool net_http_get(const char *url, char *resp, int cap, int timeout_ms)
{
    if (!url || !resp || cap < 8) {
        return false;
    }
    http_acc_t acc = {.buf = resp, .len = 0, .cap = cap};
    resp[0] = 0;
    const bool tls = strncmp(url, "https://", 8) == 0;
    esp_http_client_config_t http = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : 12000,
        .buffer_size = 2048,
        .event_handler = http_evt,
        .user_data = &acc,
        .transport_type = tls ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) {
        ESP_LOGW(TAG, "init failed");
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "radar-desk-buddy/1.0");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300) {
        /* Never log the URL: Telegram URLs carry the bot token. */
        ESP_LOGW(TAG, "GET failed HTTP %d %s", status, esp_err_to_name(err));
        return false;
    }
    return acc.len > 0;
}

bool net_http_urlencode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!out || cap == 0) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; ++p) {
        bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                     (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (plain) {
            if (o + 1 >= cap) {
                out[o] = 0;
                return false;
            }
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= cap) {
                out[o] = 0;
                return false;
            }
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = 0;
    return true;
}
