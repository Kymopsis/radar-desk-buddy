#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board_pins.h"
#include "display.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "qrcode.h"
#include "rom/tjpgd.h"
#include "ui_font_16.h"
#include "ui_font_32_digits.h"
#include "ui_i18n.h"

#ifndef SOFTAP_PORTAL_HOST
#define SOFTAP_PORTAL_HOST "mod.config"
#endif

static const char *TAG = "ui";

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COL_OK    RGB565(120, 190, 140)
#define COL_WARN  RGB565(240, 150, 90)
#define COL_BAD   RGB565(230, 100, 120)
#define COL_FLASH RGB565(255, 255, 255)
#define COL_CHIP  RGB565(255, 250, 252)
#define COL_QR_BG RGB565(255, 255, 255)
#define COL_QR_FG RGB565(0, 0, 0)
#define COL_SCLERA RGB565(245, 245, 250)
#define COL_IRIS   RGB565(70, 130, 200)
#define COL_PUPIL  RGB565(12, 12, 18)
#define COL_HL     RGB565(255, 255, 255)

/* Distance band defaults; SoftAP can retune at runtime. */
#define ZONE_NEAR_ENTER_DEFAULT 0.5f
#define ZONE_NEAR_EXIT_DEFAULT  0.6f
#define ZONE_FAR_MAX_DEFAULT    5.0f
#define ZONE_MIN_M              0.15f
#define ZONE_ENTER_MS           300
#define ZONE_VOID_MS_DEFAULT    10000
#define FORCE_NEAR_MS           8000
#define SHOW_SETUP_MS           30000 /* touch on the clock face → setup QR */
#define MSG_AUTO_CLOSE_MS       12000
#define RADAR_LINK_GRACE_MS     2500  /* baud hunt before "offline" */

static float s_near_enter_m = ZONE_NEAR_ENTER_DEFAULT;
static float s_near_exit_m = ZONE_NEAR_EXIT_DEFAULT;
static float s_far_max_m = ZONE_FAR_MAX_DEFAULT;
static int64_t s_void_ms = ZONE_VOID_MS_DEFAULT;

typedef struct {
    uint16_t bg;
    uint16_t bg_dim;
    uint16_t fg;
    uint16_t muted;
    uint16_t accent;
    uint16_t accent_dk;
    uint16_t paw;
    uint16_t band;
} ui_theme_t;

static const ui_theme_t s_theme_female = {
    .bg = RGB565(255, 236, 242),
    .bg_dim = RGB565(245, 220, 228),
    .fg = RGB565(90, 55, 70),
    .muted = RGB565(180, 130, 145),
    .accent = RGB565(255, 140, 170),
    .accent_dk = RGB565(230, 100, 140),
    .paw = RGB565(255, 180, 200),
    .band = RGB565(255, 210, 225),
};

static const ui_theme_t s_theme_male = {
    .bg = RGB565(230, 242, 255),
    .bg_dim = RGB565(210, 228, 245),
    .fg = RGB565(40, 60, 95),
    .muted = RGB565(110, 140, 175),
    .accent = RGB565(110, 175, 240),
    .accent_dk = RGB565(60, 130, 210),
    .paw = RGB565(150, 195, 245),
    .band = RGB565(190, 220, 250),
};

static uint16_t *s_fb;
static ui_model_t s_model;
static ui_screen_t s_screen = UI_SCREEN_PROV;
static int64_t s_msg_until_ms;
static int64_t s_flash_until_ms;
static int64_t s_boot_ms;
static int64_t s_force_near_until_ms;
static bool s_viewing_msg;
static bool s_follow_active; /* SoftAP armed follow (motion); face no longer gated by this when unprovisioned */
static bool s_radar_link;    /* UART frames fresh */
static bool s_radar_ever;    /* saw at least one frame since boot */
static bool s_dirty = true;
static bool s_force_screen;

#define BL_BRIGHT_PCT         85
#define BL_DIM_PCT            28
#define BL_OFF_PCT            0
#define BL_ABSENT_OFF_MS      12000
#define BL_BOOT_GRACE_MS      8000
#define BL_TOUCH_WAKE_MS      15000
static bool s_keep_awake;
static int64_t s_wake_until_ms;
static int64_t s_void_since_ms;
static bool s_bl_on = true;
static uint8_t s_bl_pct = 85;

/* Zone hysteresis */
static int64_t s_void_candidate_ms;
static int64_t s_enter_candidate_ms;
static ui_zone_t s_enter_want = UI_ZONE_VOID;

static char s_weather_buf[24];
static char s_msg_buf[160];
static uint16_t *s_msg_rgb;
static bool s_msg_has_image;
static ui_msg_closed_cb_t s_msg_closed_cb;

static const ui_theme_t *theme(void)
{
    return (s_model.gender == UI_GENDER_MALE) ? &s_theme_male : &s_theme_female;
}

static inline uint16_t be565(uint16_t c)
{
    return (uint16_t)((c << 8) | (c >> 8));
}

static uint16_t mix565(uint16_t a, uint16_t b, int t)
{
    if (t < 0) {
        t = 0;
    }
    if (t > 255) {
        t = 255;
    }
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + ((br - ar) * t) / 255;
    int g = ag + ((bg - ag) * t) / 255;
    int bl = ab + ((bb - ab) * t) / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static inline void put_px(int x, int y, uint16_t c)
{
    if ((unsigned)x >= BOARD_LCD_H_RES || (unsigned)y >= BOARD_LCD_V_RES) {
        return;
    }
    s_fb[y * BOARD_LCD_H_RES + x] = be565(c);
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            put_px(x, y, c);
        }
    }
}

static void fill_circle(int cx, int cy, int r, uint16_t c)
{
    int r2 = r * r;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            if (x * x + y * y <= r2) {
                put_px(cx + x, cy + y, c);
            }
        }
    }
}

static void fill_ellipse(int cx, int cy, int rx, int ry, uint16_t c)
{
    if (rx <= 0 || ry <= 0) {
        return;
    }
    for (int y = -ry; y <= ry; ++y) {
        for (int x = -rx; x <= rx; ++x) {
            float nx = (float)x / (float)rx;
            float ny = (float)y / (float)ry;
            if (nx * nx + ny * ny <= 1.0f) {
                put_px(cx + x, cy + y, c);
            }
        }
    }
}

static void draw_paw(int cx, int cy, int s, uint16_t col)
{
    fill_circle(cx, cy + s / 3, s / 2 + 1, col);
    fill_circle(cx - s / 2, cy - s / 3, s / 4 + 1, col);
    fill_circle(cx, cy - s / 2 - 1, s / 4 + 1, col);
    fill_circle(cx + s / 2, cy - s / 3, s / 4 + 1, col);
}

static void draw_series_bg(bool dim, bool paws)
{
    const ui_theme_t *t = theme();
    uint16_t bg = dim ? t->bg_dim : t->bg;
    uint16_t v = be565(bg);
    for (int i = 0; i < BOARD_LCD_H_RES * BOARD_LCD_V_RES; ++i) {
        s_fb[i] = v;
    }
    for (int y = 0; y < 50; ++y) {
        for (int x = 0; x < BOARD_LCD_H_RES; ++x) {
            int dx = x - 120;
            int dy = y - 20;
            if (dx * dx + dy * dy < 130 * 130) {
                put_px(x, y, t->band);
            }
        }
    }
    if (!paws) {
        return;
    }
    draw_paw(40, 200, 14, t->paw);
    draw_paw(200, 48, 12, t->paw);
    draw_paw(210, 190, 10, t->paw);
}

static const char *utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (*p == 0) {
        *cp = 0;
        return s;
    }
    if (*p < 0x80) {
        *cp = *p;
        return s + 1;
    }
    if ((*p & 0xE0) == 0xC0 && p[1]) {
        *cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
        return s + 2;
    }
    if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
        *cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return s + 3;
    }
    if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return s + 4;
    }
    *cp = '?';
    return s + 1;
}

static int text_width16(const char *s)
{
    int w = 0;
    uint32_t cp;
    while (*s) {
        s = utf8_next(s, &cp);
        if (!cp) {
            break;
        }
        w += ui_font16_advance(cp);
    }
    return w;
}

static void draw_char16(int x, int y, uint32_t cp, uint16_t color)
{
    const uint8_t *bits = ui_font16_bits(cp);
    if (!bits) {
        return;
    }
    for (int row = 0; row < UI_FONT16_H; ++row) {
        for (int col = 0; col < UI_FONT16_W; ++col) {
            int i = row * UI_FONT16_W + col;
            if (bits[i / 8] & (1 << (7 - (i % 8)))) {
                put_px(x + col, y + row, color);
            }
        }
    }
}

static void draw_text16(int x, int y, const char *s, uint16_t color)
{
    uint32_t cp;
    while (*s) {
        s = utf8_next(s, &cp);
        if (!cp) {
            break;
        }
        draw_char16(x, y, cp, color);
        x += ui_font16_advance(cp);
    }
}

static void draw_text16_centered(int cy, const char *s, uint16_t color)
{
    int w = text_width16(s);
    draw_text16((BOARD_LCD_H_RES - w) / 2, cy, s, color);
}

static void draw_char32(int x, int y, uint32_t cp, uint16_t color)
{
    int gw = 0;
    const uint8_t *bits = ui_font32_bits(cp, &gw);
    if (!bits || gw <= 0) {
        return;
    }
    int h = ui_font32_height();
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < gw; ++col) {
            int i = row * gw + col;
            if (bits[i / 8] & (1 << (7 - (i % 8)))) {
                put_px(x + col, y + row, color);
            }
        }
    }
}

static int digits32_width(const char *buf)
{
    int total_w = 0;
    for (const char *p = buf; *p; ++p) {
        int gw = 0;
        ui_font32_bits((uint32_t)(unsigned char)*p, &gw);
        total_w += gw;
    }
    return total_w;
}

static void draw_digits32(int cx, int cy, const char *buf, uint16_t color)
{
    int x = cx - digits32_width(buf) / 2;
    int y = cy - ui_font32_height() / 2;
    for (const char *p = buf; *p; ++p) {
        int gw = 0;
        ui_font32_bits((uint32_t)(unsigned char)*p, &gw);
        draw_char32(x, y, (uint32_t)(unsigned char)*p, color);
        x += gw;
    }
}

static void draw_clock(int cy, int hour, int minute, uint16_t color)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    draw_digits32(BOARD_LCD_H_RES / 2, cy, buf, color);
}

typedef enum {
    WX_NONE = 0,
    WX_SUNNY,
    WX_CLOUDY,
    WX_RAIN,
    WX_SNOW,
} weather_kind_t;

/* Accept contract codes; Chinese leftovers from older MQTT still map. */
static weather_kind_t weather_kind(void)
{
    if (!s_model.weather_ok || !s_model.weather || !s_model.weather[0]) {
        return WX_NONE;
    }
    const char *w = s_model.weather;
    if (!strcmp(w, "sunny")) {
        return WX_SUNNY;
    }
    if (!strcmp(w, "cloudy")) {
        return WX_CLOUDY;
    }
    if (!strcmp(w, "rain")) {
        return WX_RAIN;
    }
    if (!strcmp(w, "snow")) {
        return WX_SNOW;
    }
    return WX_NONE;
}

static void draw_sun(int cx, int cy, int r, uint16_t sun, uint16_t ray)
{
    fill_circle(cx, cy, r, sun);
    fill_circle(cx - r / 4, cy - r / 5, r / 5, mix565(sun, RGB565(255, 255, 255), 90));
    for (int i = 0; i < 8; ++i) {
        float ang = (float)i * (3.1415926f / 4.0f);
        int x0 = cx + (int)lroundf(cosf(ang) * (float)(r + 4));
        int y0 = cy + (int)lroundf(sinf(ang) * (float)(r + 4));
        int x1 = cx + (int)lroundf(cosf(ang) * (float)(r + 14));
        int y1 = cy + (int)lroundf(sinf(ang) * (float)(r + 14));
        for (int s = 0; s <= 8; ++s) {
            int x = x0 + (x1 - x0) * s / 8;
            int y = y0 + (y1 - y0) * s / 8;
            fill_circle(x, y, 2, ray);
        }
    }
}

static void draw_wx_cloud(int cx, int cy, uint16_t fill, uint16_t shade)
{
    uint16_t blush = theme()->paw;
    fill_ellipse(cx - 2, cy + 12, 42, 18, shade);
    fill_circle(cx - 26, cy + 2, 20, fill);
    fill_circle(cx + 24, cy + 4, 18, fill);
    fill_circle(cx, cy - 10, 24, fill);
    fill_rect(cx - 34, cy, cx + 34, cy + 16, fill);
    fill_circle(cx - 12, cy - 6, 8, mix565(fill, RGB565(255, 255, 255), 80));
    fill_circle(cx - 18, cy + 6, 5, blush);
    fill_circle(cx + 18, cy + 8, 5, blush);
}

static void draw_drop(int x, int y, uint16_t c)
{
    fill_circle(x, y + 5, 5, c);
    fill_ellipse(x, y, 3, 7, c);
}

static void draw_flake(int x, int y, uint16_t c)
{
    fill_rect(x - 6, y - 1, x + 6, y + 1, c);
    fill_rect(x - 1, y - 6, x + 1, y + 6, c);
    fill_circle(x, y, 2, c);
}

static void draw_weather_icon(int cx, int cy)
{
    switch (weather_kind()) {
    case WX_SUNNY:
        draw_sun(cx, cy, 24, RGB565(255, 196, 64), RGB565(255, 168, 48));
        break;
    case WX_CLOUDY:
        draw_wx_cloud(cx, cy + 4, RGB565(248, 250, 255), RGB565(186, 198, 220));
        break;
    case WX_RAIN:
        draw_wx_cloud(cx, cy - 10, RGB565(232, 238, 252), RGB565(140, 160, 200));
        draw_drop(cx - 16, cy + 22, RGB565(64, 140, 220));
        draw_drop(cx, cy + 28, RGB565(80, 160, 236));
        draw_drop(cx + 16, cy + 22, RGB565(64, 140, 220));
        break;
    case WX_SNOW:
        draw_wx_cloud(cx, cy - 10, RGB565(246, 250, 255), RGB565(200, 214, 232));
        draw_flake(cx - 18, cy + 24, RGB565(230, 240, 255));
        draw_flake(cx, cy + 30, RGB565(255, 255, 255));
        draw_flake(cx + 18, cy + 24, RGB565(230, 240, 255));
        break;
    case WX_NONE:
    default:
        break;
    }
}

static void draw_temp_big(int cy, uint16_t color)
{
    int t = s_model.temp_c;
    if (t < -9) {
        t = -9;
    }
    if (t > 99) {
        t = 99;
    }
    bool neg = t < 0;
    if (neg) {
        t = -t;
    }
    uint8_t u = (uint8_t)t;
    char num[4];
    snprintf(num, sizeof(num), "%u", (unsigned)u);
    int w = digits32_width(num);
    int minus_w = neg ? 14 : 0;
    int unit_w = 16;
    int gap = 4;
    int total = minus_w + w + gap + unit_w;
    int left = BOARD_LCD_H_RES / 2 - total / 2;
    int y = cy - ui_font32_height() / 2;
    if (neg) {
        fill_rect(left, cy - 2, left + 10, cy + 2, color);
    }
    int dx = left + minus_w;
    for (const char *p = num; *p; ++p) {
        int gw = 0;
        ui_font32_bits((uint32_t)(unsigned char)*p, &gw);
        draw_char32(dx, y, (uint32_t)(unsigned char)*p, color);
        dx += gw;
    }
    draw_char16(dx + gap, y + 12, 0x2103, color); /* ℃ */
}

static void draw_up_arrow(int cx, int cy, uint16_t col, int phase)
{
    int s = 14 + (phase % 24 < 12 ? phase % 12 : 12 - (phase % 12));
    for (int i = 0; i < s; ++i) {
        put_px(cx, cy - i, col);
        put_px(cx - 1, cy - i, col);
        put_px(cx + 1, cy - i, col);
    }
    for (int i = 0; i < 10; ++i) {
        put_px(cx - i, cy - s + i, col);
        put_px(cx + i, cy - s + i, col);
        put_px(cx - i, cy - s + i + 1, col);
        put_px(cx + i, cy - s + i + 1, col);
    }
}

static void draw_rounded_chip(int cy, const char *label, uint16_t dot)
{
    int w = text_width16(label) + 28;
    if (w > 200) {
        w = 200;
    }
    int h = 28;
    int x0 = (BOARD_LCD_H_RES - w) / 2;
    int y0 = cy;
    fill_circle(x0 + h / 2, y0 + h / 2, h / 2, COL_CHIP);
    fill_circle(x0 + w - h / 2, y0 + h / 2, h / 2, COL_CHIP);
    fill_rect(x0 + h / 2, y0, x0 + w - h / 2, y0 + h, COL_CHIP);
    fill_circle(x0 + 14, y0 + h / 2, 5, dot);
    draw_text16(x0 + 24, y0 + 6, label, theme()->fg);
}

static void wifi_escape(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 2 < out_len; ++i) {
        char c = in[i];
        if (c == '\\' || c == ';' || c == ',' || c == '"') {
            out[j++] = '\\';
        }
        out[j++] = c;
    }
    out[j] = '\0';
}

static char s_wifi_payload[96];
static char s_wifi_ssid_label[24];
static char s_wifi_pass_label[20];
static bool s_wifi_qr_ready;

#define QR_CACHE_MAX 48
static uint8_t s_qr_bits[(QR_CACHE_MAX * QR_CACHE_MAX + 7) / 8];
static int s_qr_size;
static bool s_qr_cached;

typedef struct {
    int size;
} qr_cache_ctx_t;

static void qr_bit_set(int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= s_qr_size || y >= s_qr_size) {
        return;
    }
    unsigned idx = (unsigned)y * (unsigned)s_qr_size + (unsigned)x;
    if (on) {
        s_qr_bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
    } else {
        s_qr_bits[idx / 8] &= (uint8_t)~(1u << (idx % 8));
    }
}

static bool qr_bit_get(int x, int y)
{
    unsigned idx = (unsigned)y * (unsigned)s_qr_size + (unsigned)x;
    return (s_qr_bits[idx / 8] >> (idx % 8)) & 1u;
}

static void qr_cache_modules(esp_qrcode_handle_t qrcode, void *user_data)
{
    qr_cache_ctx_t *ctx = (qr_cache_ctx_t *)user_data;
    int n = esp_qrcode_get_size(qrcode);
    if (n <= 0 || n > QR_CACHE_MAX || !ctx) {
        if (ctx) {
            ctx->size = 0;
        }
        return;
    }
    s_qr_size = n;
    memset(s_qr_bits, 0, sizeof(s_qr_bits));
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            qr_bit_set(x, y, esp_qrcode_get_module(qrcode, x, y));
        }
    }
    ctx->size = n;
}

static bool ensure_qr_cache(void)
{
    if (s_qr_cached && s_qr_size > 0) {
        return true;
    }
    if (!s_wifi_qr_ready || s_wifi_payload[0] == '\0') {
        return false;
    }
    qr_cache_ctx_t ctx = {0};
    esp_qrcode_config_t cfg = {
        .display_func_with_cb = qr_cache_modules,
        .max_qrcode_version = 6,
        .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
        .user_data = &ctx,
    };
    if (esp_qrcode_generate(&cfg, s_wifi_payload) != ESP_OK || ctx.size <= 0) {
        ESP_LOGW(TAG, "qr encode fail");
        s_qr_cached = false;
        return false;
    }
    s_qr_cached = true;
    ESP_LOGI(TAG, "WIFI QR cached size=%d", s_qr_size);
    return true;
}

static void fill_round_rect(int x0, int y0, int x1, int y1, int r, uint16_t c)
{
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    if (r < 0) {
        r = 0;
    }
    int w = x1 - x0;
    int h = y1 - y0;
    if (r * 2 > w) {
        r = w / 2;
    }
    if (r * 2 > h) {
        r = h / 2;
    }
    fill_rect(x0 + r, y0, x1 - r, y1, c);
    fill_rect(x0, y0 + r, x1, y1 - r, c);
    fill_circle(x0 + r, y0 + r, r, c);
    fill_circle(x1 - r, y0 + r, r, c);
    fill_circle(x0 + r, y1 - r, r, c);
    fill_circle(x1 - r, y1 - r, r, c);
}

static void draw_wifi_glyph(int cx, int cy, uint16_t c)
{
    fill_circle(cx, cy + 7, 2, c);
    for (int r = 5; r <= 13; r += 4) {
        for (int x = -r; x <= r; ++x) {
            int y2 = r * r - x * x;
            if (y2 < 0) {
                continue;
            }
            int y = 0;
            while ((y + 1) * (y + 1) <= y2) {
                y++;
            }
            int yy = cy + 7 - y;
            if (yy <= cy + 3) {
                put_px(cx + x, yy, c);
            }
        }
    }
}

static void draw_heart_dot(int cx, int cy, uint16_t c)
{
    fill_circle(cx - 2, cy, 2, c);
    fill_circle(cx + 2, cy, 2, c);
    put_px(cx, cy + 3, c);
    put_px(cx - 1, cy + 2, c);
    put_px(cx + 1, cy + 2, c);
}

static void draw_cloud(int cx, int cy, uint16_t c)
{
    fill_circle(cx - 18, cy, 10, c);
    fill_circle(cx - 4, cy - 4, 12, c);
    fill_circle(cx + 12, cy, 11, c);
    fill_circle(cx + 22, cy + 2, 8, c);
    fill_rect(cx - 26, cy, cx + 28, cy + 10, c);
}

static void draw_prov_pointer(const ui_theme_t *t, int phase)
{
    const int bx = 120;
    const int by = 226;
    const int bob = (phase % 18 < 9) ? 0 : 1;
    uint16_t face = RGB565(255, 255, 255);
    uint16_t blush = RGB565(255, 168, 188);
    uint16_t eye = RGB565(45, 40, 55);
    uint16_t cloud = RGB565(255, 252, 255);

    draw_cloud(bx, by + 2, cloud);
    fill_circle(bx, by - 10 + bob, 20, face);
    fill_circle(bx - 13, by - 28 + bob, 8, face);
    fill_circle(bx + 13, by - 28 + bob, 8, face);
    fill_circle(bx - 13, by - 28 + bob, 4, t->accent);
    fill_circle(bx + 13, by - 28 + bob, 4, t->accent);
    fill_circle(bx - 7, by - 12 + bob, 4, eye);
    fill_circle(bx + 7, by - 12 + bob, 4, eye);
    put_px(bx - 6, by - 13 + bob, face);
    put_px(bx + 8, by - 13 + bob, face);
    fill_circle(bx - 12, by - 5 + bob, 3, blush);
    fill_circle(bx + 12, by - 5 + bob, 3, blush);
    fill_circle(bx, by - 4 + bob, 2, t->accent_dk);
    for (int i = 0; i < 16; ++i) {
        fill_circle(bx - 34, by - 4 - i + bob, 3, face);
        fill_circle(bx + 34, by - 4 - i + bob, 3, face);
    }
    fill_circle(bx - 34, by - 22 + bob, 6, face);
    fill_circle(bx + 34, by - 22 + bob, 6, face);
    fill_circle(bx - 34, by - 30 + bob, 3, t->accent_dk);
    fill_circle(bx + 34, by - 30 + bob, 3, t->accent_dk);
}

static void draw_mini_badge(int cx, int cy, char kind, const ui_theme_t *t)
{
    fill_circle(cx, cy, 8, t->accent);
    fill_circle(cx, cy, 6, COL_CHIP);
    if (kind == 'w') {
        fill_circle(cx, cy + 2, 1, t->accent_dk);
        put_px(cx - 2, cy - 1, t->accent_dk);
        put_px(cx + 2, cy - 1, t->accent_dk);
    } else if (kind == 'l') {
        fill_rect(cx - 2, cy - 1, cx + 2, cy + 3, t->accent_dk);
        fill_circle(cx, cy - 2, 3, t->accent_dk);
        fill_circle(cx, cy - 2, 1, COL_CHIP);
    } else {
        fill_circle(cx, cy, 3, t->accent_dk);
        fill_circle(cx, cy, 1, COL_CHIP);
    }
}

static void draw_prov_info_row(int y, char kind, const char *label, const char *value, const ui_theme_t *t)
{
    draw_mini_badge(42, y + 7, kind, t);
    char line[36];
    if (value && value[0]) {
        snprintf(line, sizeof(line), "%s %s", label, value);
    } else {
        snprintf(line, sizeof(line), "%s", label);
    }
    draw_text16(56, y, line, t->fg);
}

static void draw_wifi_qr(int cx, int cy, int box)
{
    if (!ensure_qr_cache()) {
        fill_round_rect(cx - box / 2 - 6, cy - box / 2 - 6, cx + box / 2 + 6, cy + box / 2 + 6, 10, COL_QR_BG);
        draw_text16_centered(cy - 8, "...", theme()->muted);
        return;
    }
    int n = s_qr_size;
    int modules = n + 4;
    int cell = box / modules;
    if (cell < 2) {
        cell = 2;
    }
    int side = cell * modules;
    int pad = 6;
    int x0 = cx - side / 2;
    int y0 = cy - side / 2;
    fill_round_rect(x0 - pad, y0 - pad, x0 + side - 1 + pad, y0 + side - 1 + pad, 10, COL_QR_BG);
    int ox = x0 + 2 * cell;
    int oy = y0 + 2 * cell;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            if (qr_bit_get(x, y)) {
                fill_rect(ox + x * cell, oy + y * cell,
                          ox + (x + 1) * cell - 1, oy + (y + 1) * cell - 1, COL_QR_FG);
            }
        }
    }
}

static const char *peer_label(void)
{
    if (!s_model.cloud_linked) {
        return ui_tr(s_model.lang, UI_STR_CLOUD_OFF);
    }
    switch (s_model.peer) {
    case UI_PEER_PRESENT:
        return ui_tr(s_model.lang, UI_STR_PEER_HERE);
    case UI_PEER_AWAY:
        return ui_tr(s_model.lang, UI_STR_PEER_AWAY);
    default:
        return ui_tr(s_model.lang, UI_STR_PEER_OFFLINE);
    }
}

static uint16_t peer_dot(void)
{
    switch (s_model.peer) {
    case UI_PEER_PRESENT:
        return COL_OK;
    case UI_PEER_AWAY:
        return COL_WARN;
    default:
        return COL_BAD;
    }
}

/* Pairing is optional: never draw the peer status. Being online is enough. */
static void draw_status_chip(int y)
{
    if (!s_model.cloud_linked) {
        draw_rounded_chip(y, peer_label(), peer_dot());
    }
}

static void draw_wrapped_centered(int y0, const char *s, uint16_t color, int max_glyphs, int max_rows)
{
    const char *p = s;
    int row = 0;
    while (*p && row < max_rows) {
        char line[80];
        int n = 0;
        int bytes = 0;
        const char *q = p;
        while (*q && n < max_glyphs) {
            uint32_t cp;
            const char *nxt = utf8_next(q, &cp);
            int adv = (int)(nxt - q);
            if (bytes + adv >= (int)sizeof(line) - 1) {
                break;
            }
            memcpy(line + bytes, q, (size_t)adv);
            bytes += adv;
            q = nxt;
            n++;
        }
        line[bytes] = 0;
        draw_text16_centered(y0 + row * 20, line, color);
        p = q;
        row++;
    }
}

/* Usable width of the round face on [y0, y1], minus a bezel inset. */
static int circle_span_w(int y0, int y1)
{
    const int r = 114;
    const int inset = 14;
    int wmin = BOARD_LCD_H_RES;
    if (y0 > y1) {
        int tmp = y0;
        y0 = y1;
        y1 = tmp;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > BOARD_LCD_V_RES - 1) {
        y1 = BOARD_LCD_V_RES - 1;
    }
    for (int y = y0; y <= y1; ++y) {
        int dy = y - 120;
        int inner = r * r - dy * dy;
        if (inner <= 0) {
            return 0;
        }
        int half = (int)lroundf(sqrtf((float)inner));
        int w = 2 * half - 2 * inset;
        if (w < wmin) {
            wmin = w;
        }
    }
    return wmin < 0 ? 0 : wmin;
}

static int scaled_advance(uint32_t cp, int scale)
{
    return ui_font16_advance(cp) * scale;
}

static void draw_char16_scaled(int x, int y, uint32_t cp, uint16_t color, int scale)
{
    const uint8_t *bits = ui_font16_bits(cp);
    if (!bits || scale < 1) {
        return;
    }
    for (int row = 0; row < UI_FONT16_H; ++row) {
        for (int col = 0; col < UI_FONT16_W; ++col) {
            int i = row * UI_FONT16_W + col;
            if ((bits[i / 8] & (1 << (7 - (i % 8)))) == 0) {
                continue;
            }
            if (scale == 1) {
                put_px(x + col, y + row, color);
            } else {
                int x0 = x + col * scale;
                int y0 = y + row * scale;
                fill_rect(x0, y0, x0 + scale - 1, y0 + scale - 1, color);
            }
        }
    }
}

static int text_width_scaled(const char *s, int scale)
{
    int w = 0;
    uint32_t cp;
    while (*s) {
        s = utf8_next(s, &cp);
        if (!cp) {
            break;
        }
        w += scaled_advance(cp, scale);
    }
    return w;
}

static void draw_text_scaled(int x, int y, const char *s, uint16_t color, int scale)
{
    uint32_t cp;
    while (*s) {
        s = utf8_next(s, &cp);
        if (!cp) {
            break;
        }
        draw_char16_scaled(x, y, cp, color, scale);
        x += scaled_advance(cp, scale);
    }
}

#define MSG_WRAP_LINES 8
#define MSG_WRAP_BYTES 80

static bool wrap_msg_scale(const char *s, int scale, int line_h, int y0, int max_rows,
                           char lines[][MSG_WRAP_BYTES], int *out_n)
{
    const char *p = s;
    int n = 0;
    int glyph_h = UI_FONT16_H * scale;
    while (*p && n < max_rows) {
        int y = y0 + n * line_h;
        int max_w = circle_span_w(y, y + glyph_h - 1);
        if (max_w < 8) {
            return false;
        }
        int bytes = 0;
        int w = 0;
        int space_bytes = -1; /* last break point inside this line (Latin word wrap) */
        const char *space_q = NULL;
        const char *q = p;
        while (*q) {
            uint32_t cp;
            const char *nxt = utf8_next(q, &cp);
            if (!cp) {
                break;
            }
            if (cp == '\n') {
                q = nxt;
                break;
            }
            int add = (int)(nxt - q);
            int adv = scaled_advance(cp, scale);
            if (bytes + add >= MSG_WRAP_BYTES - 1) {
                break;
            }
            if (w + adv > max_w) {
                if (bytes == 0) {
                    return false;
                }
                /* Break after the last space so English words stay whole. */
                if (cp != ' ' && space_bytes > 0) {
                    bytes = space_bytes;
                    q = space_q;
                }
                while (*q == ' ') {
                    q++;
                }
                break;
            }
            if (cp == ' ') {
                space_bytes = bytes;
                space_q = nxt;
            }
            memcpy(lines[n] + bytes, q, (size_t)add);
            bytes += add;
            w += adv;
            q = nxt;
        }
        if (bytes == 0) {
            return false;
        }
        lines[n][bytes] = 0;
        p = q;
        n++;
    }
    *out_n = n;
    return *p == 0;
}

static void trim_line_ellipsis(char *line, int scale, int max_w)
{
    const int dots_w = scaled_advance('.', scale) * 2;
    while (line[0] && text_width_scaled(line, scale) + dots_w > max_w) {
        int cut = (int)strlen(line);
        while (cut > 0 && ((unsigned char)line[cut - 1] & 0xC0) == 0x80) {
            cut--;
        }
        if (cut > 0) {
            cut--;
        }
        line[cut] = 0;
    }
    size_t len = strlen(line);
    if (len + 3 < MSG_WRAP_BYTES) {
        memcpy(line + len, "..", 3);
    }
}

/* Pick 48 / 32 / 16 px so the block stays inside the circular chord. */
static void draw_msg_text_adaptive(int y_top, int y_bot, const char *s, uint16_t color)
{
    if (!s || !s[0]) {
        return;
    }
    const int scales[] = {3, 2, 1};
    char lines[MSG_WRAP_LINES][MSG_WRAP_BYTES];
    int n_lines = 0;
    int use_scale = 1;
    int use_lh = 20;
    int use_y0 = y_top;
    int area = y_bot - y_top;
    if (area < UI_FONT16_H) {
        return;
    }
    bool fit = false;
    for (int i = 0; i < 3 && !fit; ++i) {
        int sc = scales[i];
        int lh = sc * UI_FONT16_H + (sc >= 2 ? 8 : 4);
        int max_rows = area / lh;
        if (max_rows > MSG_WRAP_LINES) {
            max_rows = MSG_WRAP_LINES;
        }
        if (max_rows < 1) {
            continue;
        }
        int y0 = y_top;
        int got = 0;
        bool ok = false;
        for (int pass = 0; pass < 3; ++pass) {
            if (!wrap_msg_scale(s, sc, lh, y0, max_rows, lines, &got) || got <= 0) {
                ok = false;
                break;
            }
            int y1 = y_top + (area - got * lh) / 2;
            if (y1 < y_top) {
                y1 = y_top;
            }
            if (y1 + got * lh > y_bot) {
                ok = false;
                break;
            }
            if (y1 == y0) {
                ok = true;
                break;
            }
            y0 = y1;
        }
        if (!ok) {
            if (i == 2 && got > 0) {
                n_lines = got;
                use_scale = sc;
                use_lh = lh;
                use_y0 = y0;
            }
            continue;
        }
        n_lines = got;
        use_scale = sc;
        use_lh = lh;
        use_y0 = y0;
        fit = true;
    }
    if (n_lines <= 0) {
        draw_text16_centered(y_top, s, color);
        return;
    }
    if (!fit) {
        int y = use_y0 + (n_lines - 1) * use_lh;
        trim_line_ellipsis(lines[n_lines - 1], use_scale,
                           circle_span_w(y, y + use_scale * UI_FONT16_H - 1));
    }
    for (int r = 0; r < n_lines; ++r) {
        int w = text_width_scaled(lines[r], use_scale);
        int x = (BOARD_LCD_H_RES - w) / 2;
        draw_text_scaled(x, use_y0 + r * use_lh, lines[r], color, use_scale);
    }
}

static void draw_one_eye(int cx, int cy, int look_x, int look_y, int open_pct)
{
    const int rx = 44;
    const int ry = 50;
    int open_ry = (ry * open_pct) / 100;
    if (open_ry < 3) {
        fill_rect(cx - rx + 4, cy - 2, cx + rx - 4, cy + 2, COL_SCLERA);
        return;
    }
    fill_ellipse(cx, cy, rx, open_ry, COL_SCLERA);
    int ix = cx + look_x;
    int iy = cy + look_y;
    /* Allow iris near sclera rim so sideways look reads from across the room. */
    const int lim_x = 28;
    const int lim_y = 14;
    if (ix < cx - lim_x) {
        ix = cx - lim_x;
    }
    if (ix > cx + lim_x) {
        ix = cx + lim_x;
    }
    if (iy < cy - lim_y) {
        iy = cy - lim_y;
    }
    if (iy > cy + lim_y) {
        iy = cy + lim_y;
    }
    fill_circle(ix, iy, 16, COL_IRIS);
    fill_circle(ix, iy, 9, COL_PUPIL);
    fill_circle(ix - 4, iy - 4, 3, COL_HL);
    if (open_pct < 95) {
        int cover = ry - open_ry;
        fill_rect(cx - rx - 2, cy - ry - 2, cx + rx + 2, cy - ry + cover + 4, RGB565(18, 22, 36));
    }
}

static float s_gaze_smooth;
static bool s_gaze_ok;
static int64_t s_next_blink_ms;
static int s_blink_phase; /* 0 open; 1..16 blink */

static void draw_eyes_face(int64_t now_ms, bool unread_layer)
{
    const ui_theme_t *t = theme();
    /* Follow ON: pupils track pan (servo). Follow OFF: locked center. */
    float pan = 0.0f;
    if (s_follow_active && s_gaze_ok) {
        pan = s_gaze_smooth;
    } else if (s_follow_active) {
        pan = s_model.pan_deg;
    }
    /* Exaggerate: ±40° pan → full ±28 px (readable from far). Soft clip beyond. */
    float gain = pan * 0.85f; /* ~34 px/40° before clamp */
    if (gain > 28.0f) {
        gain = 28.0f;
    }
    if (gain < -28.0f) {
        gain = -28.0f;
    }
    int look_x = (int)lroundf(gain);
    int look_y = s_follow_active ? (int)lroundf(sinf((float)now_ms / 900.0f) * 3.0f) : 0;

    int open = 100;
    if (s_blink_phase > 0) {
        if (s_blink_phase <= 8) {
            open = 100 - (s_blink_phase * 100) / 8;
        } else {
            open = ((s_blink_phase - 8) * 100) / 8;
        }
    }

    draw_one_eye(78, 118, look_x, look_y, open);
    draw_one_eye(162, 118, look_x, look_y, open);

    if (unread_layer && s_model.unread_count > 0) {
        int show = s_model.unread_count;
        if (show > 9) {
            show = 9;
        }
        char n[8];
        snprintf(n, sizeof(n), "%d", show);
        fill_circle(120, 42, 22, t->accent);
        draw_text16_centered(34, n, COL_CHIP);
        draw_up_arrow(120, 186, t->accent_dk, (int)(now_ms / 45));
        draw_wrapped_centered(206, ui_tr(s_model.lang, UI_STR_TOUCH_HEAD), t->fg, 12, 1);
    }
}

static ui_zone_t effective_zone(int64_t now_ms)
{
    if (now_ms < s_force_near_until_ms) {
        return UI_ZONE_NEAR;
    }
    return s_model.zone;
}

/**
 * Face priority:
 *   !radar → NO_RADAR (the only always-on warning)
 *   VOID → SLEEP (nobody around: screen off)
 *   FAR + follow ON → EYES (follow from afar, online or not)
 *   FAR + follow OFF → SLEEP
 *   NEAR → content: offline PROV/QR; unread GREET; otherwise INFO; open message MSG
 */
static bool radar_ok(int64_t now_ms)
{
    if ((now_ms - s_boot_ms) < RADAR_LINK_GRACE_MS) {
        return true; /* baud hunt — don't alarm yet */
    }
    return s_radar_link;
}

static void close_msg(void);
static int64_t s_show_setup_until_ms;

static ui_screen_t auto_screen(int64_t now_ms)
{
    const bool rok = radar_ok(now_ms);

    /* Radar down is the only always-on warning — not gated by near field. */
    if (!rok) {
        return UI_SCREEN_NO_RADAR;
    }

    ui_zone_t z = effective_zone(now_ms);

    if (s_viewing_msg) {
        if (z == UI_ZONE_NEAR) {
            return UI_SCREEN_MSG;
        }
        close_msg();
    }

    if (z == UI_ZONE_VOID || (s_model.quiet && z == UI_ZONE_FAR) ||
        (z == UI_ZONE_FAR && !s_follow_active)) {
        return UI_SCREEN_SLEEP;
    }
    if (z == UI_ZONE_FAR) {
        return UI_SCREEN_EYES;
    }

    /* NEAR: light up content. */
    if (!s_model.cloud_linked || now_ms < s_show_setup_until_ms) {
        return UI_SCREEN_PROV;
    }
    if (s_model.unread_count > 0) {
        return UI_SCREEN_GREET;
    }
    return UI_SCREEN_INFO;
}

static void set_backlight_pct(uint8_t pct)
{
    if (s_bl_on && s_bl_pct == pct) {
        return;
    }
    s_bl_pct = pct;
    s_bl_on = (pct > 0);
    display_set_backlight(pct);
}

static void apply_backlight_policy(int64_t now_ms)
{
    ui_zone_t z = effective_zone(now_ms);
    bool void_zone = (z == UI_ZONE_VOID);

    if (!void_zone) {
        s_void_since_ms = 0;
    } else if (s_void_since_ms == 0) {
        s_void_since_ms = now_ms;
    }

    /* SoftAP client / touch / no-radar / boot grace → bright. */
    if (s_keep_awake || s_screen == UI_SCREEN_NO_RADAR || now_ms < s_wake_until_ms ||
        (now_ms - s_boot_ms) < BL_BOOT_GRACE_MS) {
        set_backlight_pct(BL_BRIGHT_PCT);
        return;
    }

    if (s_screen == UI_SCREEN_PROV || s_screen == UI_SCREEN_GREET || s_screen == UI_SCREEN_MSG ||
        s_screen == UI_SCREEN_INFO || s_screen == UI_SCREEN_EYES || z == UI_ZONE_FAR ||
        z == UI_ZONE_NEAR) {
        set_backlight_pct(BL_BRIGHT_PCT);
        return;
    }

    /* SLEEP / VOID: dim then off after 12s */
    if (void_zone && !s_keep_awake && (now_ms - s_void_since_ms) >= BL_ABSENT_OFF_MS) {
        set_backlight_pct(BL_OFF_PCT);
        return;
    }
    set_backlight_pct(BL_DIM_PCT);
}

static void render(int64_t now_ms)
{
    const ui_theme_t *t = theme();
    bool dim = (s_screen == UI_SCREEN_SLEEP);
    bool eyes_bg = (s_screen == UI_SCREEN_EYES);

    if (eyes_bg) {
        uint16_t v = be565(RGB565(18, 22, 36));
        for (int i = 0; i < BOARD_LCD_H_RES * BOARD_LCD_V_RES; ++i) {
            s_fb[i] = v;
        }
    } else {
        bool paws = (s_screen != UI_SCREEN_INFO && s_screen != UI_SCREEN_SLEEP);
        draw_series_bg(dim, paws);
    }

    if (now_ms < s_flash_until_ms) {
        for (int i = 0; i < 5; ++i) {
            fill_circle(120, 120, 118 - i, COL_FLASH);
        }
        if (!eyes_bg) {
            fill_circle(120, 120, 113, dim ? t->bg_dim : t->bg);
            if (s_screen != UI_SCREEN_INFO && s_screen != UI_SCREEN_SLEEP) {
                draw_paw(40, 200, 14, t->paw);
                draw_paw(200, 48, 12, t->paw);
            }
        } else {
            fill_circle(120, 120, 113, RGB565(18, 22, 36));
        }
    }

    switch (s_screen) {
    case UI_SCREEN_PROV: {
        bool show_radar_warn = !radar_ok(now_ms);
        bool tight = show_radar_warn || !s_model.cloud_linked;
        draw_wifi_glyph(120, 12, t->accent_dk);
        draw_heart_dot(88, 14, t->accent);
        draw_heart_dot(152, 14, t->accent);
        draw_text16_centered(18, ui_tr(s_model.lang, UI_STR_PROV_SCAN), t->accent_dk);
        fill_round_rect(62, 34, 178, tight ? 114 : 128, 14, t->band);
        draw_wifi_qr(120, tight ? 72 : 80, tight ? 64 : 76);
        draw_prov_info_row(tight ? 118 : 132, 'w', "SSID", s_wifi_ssid_label, t);
        draw_prov_info_row(tight ? 136 : 148, 'l', "PWD", s_wifi_pass_label, t);
        draw_prov_info_row(tight ? 154 : 164, 'i', "URL", SOFTAP_PORTAL_HOST, t);
        if (show_radar_warn) {
            fill_round_rect(28, 176, 212, 208, 10, COL_BAD);
            draw_text16_centered(186, ui_tr(s_model.lang, UI_STR_RADAR_OFF), COL_CHIP);
        } else if (!s_model.cloud_linked) {
            fill_round_rect(28, 176, 212, 208, 10, COL_BAD);
            draw_text16_centered(186, ui_tr(s_model.lang, UI_STR_CLOUD_OFF), COL_CHIP);
        } else {
            draw_prov_pointer(t, (int)(now_ms / 90));
        }
        break;
    }

    case UI_SCREEN_NO_RADAR: {
        fill_circle(120, 92, 28, COL_BAD);
        fill_circle(120, 92, 18, t->bg);
        fill_rect(118, 80, 122, 96, COL_BAD);
        fill_circle(120, 102, 3, COL_BAD);
        draw_text16_centered(136, ui_tr(s_model.lang, UI_STR_RADAR_OFF), t->accent_dk);
        break;
    }

    case UI_SCREEN_SLEEP:
        if (s_model.weather_ok) {
            draw_weather_icon(120, 88);
        }
        if (s_model.clock_ok) {
            draw_clock(s_model.weather_ok ? 168 : 130, s_model.clock_hour, s_model.clock_min,
                       t->muted);
        }
        break;

    case UI_SCREEN_EYES:
        draw_eyes_face(now_ms, s_model.unread_count > 0);
        break;

    case UI_SCREEN_GREET: {
        /* No clock here — INFO owns the time face. Keep one idea per band. */
        draw_text16_centered(40, ui_tr(s_model.lang, UI_STR_NEW_MSG), t->accent_dk);
        if (s_model.unread_count > 0) {
            char n[8];
            int show = s_model.unread_count;
            if (show > 9) {
                show = 9;
            }
            snprintf(n, sizeof(n), "%d", show);
            fill_circle(120, 108, 28, t->accent);
            fill_circle(120, 108, 22, t->accent_dk);
            draw_digits32(120, 108, n, COL_CHIP);
        }
        draw_up_arrow(120, 168, t->accent_dk, (int)(now_ms / 45));
        draw_wrapped_centered(196, ui_tr(s_model.lang, UI_STR_TOUCH_HEAD), t->fg, 12, 1);
        break;
    }

    case UI_SCREEN_MSG:
        if (s_msg_has_image && s_msg_rgb) {
            memcpy(s_fb, s_msg_rgb, BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t));
            if (s_model.msg_text && s_model.msg_text[0]) {
                draw_wrapped_centered(188, s_model.msg_text, COL_CHIP, 12, 2);
            }
            draw_wrapped_centered(216, ui_tr(s_model.lang, UI_STR_TOUCH_CLOSE), COL_CHIP, 12, 1);
        } else {
            draw_text16_centered(24, ui_tr(s_model.lang, UI_STR_MSG_TITLE), t->accent_dk);
            if (s_model.msg_text && s_model.msg_text[0]) {
                draw_msg_text_adaptive(48, 196, s_model.msg_text, t->fg);
            } else {
                draw_paw(120, 120, 22, t->accent);
            }
            draw_wrapped_centered(208, ui_tr(s_model.lang, UI_STR_TOUCH_CLOSE), t->muted, 12, 1);
        }
        break;

    case UI_SCREEN_INFO:
    default: {
        if (s_model.clock_ok) {
            draw_clock(42, s_model.clock_hour, s_model.clock_min, t->fg);
        }
        if (s_model.weather_ok) {
            draw_weather_icon(120, 116);
            draw_temp_big(190, t->accent_dk);
        }
        draw_status_chip(210);
        break;
    }
    }

    display_flush(s_fb, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    s_dirty = false;
}

static void pick_and_render(int64_t now_ms)
{
    if (!s_force_screen) {
        ui_screen_t next = auto_screen(now_ms);
        if (next != s_screen) {
            s_screen = next;
            s_dirty = true;
        }
    }
    apply_backlight_policy(now_ms);
    render(now_ms);
}

static void open_msg(int64_t now_ms)
{
    s_viewing_msg = true;
    s_msg_until_ms = now_ms + MSG_AUTO_CLOSE_MS;
    s_screen = UI_SCREEN_MSG;
    s_dirty = true;
}

static void close_msg(void)
{
    s_viewing_msg = false;
    s_msg_until_ms = 0;
    s_force_screen = false;
    /* Single-slot hot cache: closing clears current id → unread 0 until MQTT. */
    s_model.unread_count = 0;
    s_dirty = true;
    if (s_msg_closed_cb) {
        s_msg_closed_cb();
    }
}

void ui_set_msg_closed_cb(ui_msg_closed_cb_t cb)
{
    s_msg_closed_cb = cb;
}

esp_err_t ui_init(void)
{
    if (s_fb) {
        return ESP_OK;
    }
    s_fb = heap_caps_malloc(BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        s_fb = heap_caps_malloc(BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "fb");

    memset(&s_model, 0, sizeof(s_model));
    s_model.lang = UI_LANG_EN;
    s_model.gender = UI_GENDER_FEMALE;
    s_model.peer = UI_PEER_OFFLINE;
    s_model.zone = UI_ZONE_VOID;
    s_model.provisioned = false;
    s_model.cloud_linked = false;
    s_model.weather = s_weather_buf;
    s_model.msg_text = s_msg_buf;
    s_boot_ms = esp_timer_get_time() / 1000;
    s_wake_until_ms = s_boot_ms + BL_BOOT_GRACE_MS;
    s_screen = UI_SCREEN_PROV;
    s_gaze_smooth = 0.0f;
    s_gaze_ok = false;
    s_next_blink_ms = s_boot_ms + 1800;
    s_blink_phase = 0;
    pick_and_render(s_boot_ms);
    ESP_LOGI(TAG, "UI ready lang=%s", ui_lang_name(s_model.lang));
    return ESP_OK;
}

esp_err_t ui_set_model(const ui_model_t *model)
{
    if (!model) {
        return ESP_ERR_INVALID_ARG;
    }
    s_model = *model;
    if (model->weather) {
        snprintf(s_weather_buf, sizeof(s_weather_buf), "%s", model->weather);
        s_model.weather = s_weather_buf;
    }
    if (model->msg_text) {
        snprintf(s_msg_buf, sizeof(s_msg_buf), "%s", model->msg_text);
        s_model.msg_text = s_msg_buf;
    }
    s_dirty = true;
    return ESP_OK;
}

esp_err_t ui_set_lang(ui_lang_t lang)
{
    if (lang >= UI_LANG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_model.lang = lang;
    s_dirty = true;
    return ESP_OK;
}

esp_err_t ui_set_gender(ui_gender_t gender)
{
    s_model.gender = (gender == UI_GENDER_MALE) ? UI_GENDER_MALE : UI_GENDER_FEMALE;
    s_dirty = true;
    return ESP_OK;
}

esp_err_t ui_set_provisioned(bool provisioned)
{
    if (s_model.provisioned == provisioned) {
        return ESP_OK;
    }
    s_model.provisioned = provisioned;
    s_force_screen = false;
    s_dirty = true;
    ESP_LOGI(TAG, "provisioned=%d", (int)provisioned);
    return ESP_OK;
}

esp_err_t ui_set_cloud_linked(bool linked)
{
    if (s_model.cloud_linked == linked) {
        return ESP_OK;
    }
    s_model.cloud_linked = linked;
    s_force_screen = false;
    s_dirty = true;
    ESP_LOGI(TAG, "cloud_linked=%d", (int)linked);
    return ESP_OK;
}

esp_err_t ui_set_follow_active(bool on)
{
    if (s_follow_active == on) {
        return ESP_OK;
    }
    s_follow_active = on;
    s_force_screen = false;
    s_dirty = true;
    ESP_LOGI(TAG, "follow_active=%d", (int)on);
    return ESP_OK;
}

esp_err_t ui_set_radar_link(bool linked, int64_t now_ms)
{
    (void)now_ms;
    if (linked) {
        s_radar_ever = true;
    }
    if (s_radar_link == linked) {
        return ESP_OK;
    }
    s_radar_link = linked;
    s_dirty = true;
    ESP_LOGI(TAG, "radar_link=%d (ever=%d)", (int)linked, (int)s_radar_ever);
    return ESP_OK;
}

esp_err_t ui_set_peer(ui_peer_status_t peer, const char *weather, int temp_c, bool weather_ok)
{
    s_model.peer = peer;
    if (weather_ok) {
        (void)ui_set_weather(weather, temp_c, true);
    }
    s_dirty = true;
    return ESP_OK;
}

esp_err_t ui_set_weather(const char *weather, int temp_c, bool weather_ok)
{
    s_model.temp_c = temp_c;
    s_model.weather_ok = weather_ok;
    if (weather && weather_ok) {
        snprintf(s_weather_buf, sizeof(s_weather_buf), "%s", weather);
    } else {
        s_weather_buf[0] = '\0';
    }
    s_model.weather = s_weather_buf;
    s_dirty = true;
    return ESP_OK;
}

esp_err_t ui_set_unread(int count)
{
    if (count < 0) {
        count = 0;
    }
    if (s_model.unread_count == count) {
        return ESP_OK;
    }
    s_model.unread_count = count;
    s_dirty = true;
    return ESP_OK;
}

esp_err_t ui_set_msg_text(const char *text)
{
    if (text) {
        snprintf(s_msg_buf, sizeof(s_msg_buf), "%s", text);
    } else {
        s_msg_buf[0] = '\0';
    }
    s_model.msg_text = s_msg_buf;
    s_dirty = true;
    return ESP_OK;
}

typedef struct {
    const uint8_t *jpg;
    int len;
    int pos;
    uint16_t *rgb;
} ui_jpeg_io_t;

static UINT ui_jpeg_in(JDEC *jd, BYTE *buff, UINT n)
{
    ui_jpeg_io_t *io = jd->device;
    int left = io->len - io->pos;
    if (left < 0) {
        left = 0;
    }
    if ((int)n > left) {
        n = (UINT)left;
    }
    if (buff && n) {
        memcpy(buff, io->jpg + io->pos, n);
    }
    io->pos += (int)n;
    return n;
}

static UINT ui_jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    ui_jpeg_io_t *io = jd->device;
    const uint8_t *src = bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        if ((unsigned)y >= BOARD_LCD_V_RES) {
            break;
        }
        for (int x = rect->left; x <= rect->right; x++) {
            uint8_t r = *src++;
            uint8_t g = *src++;
            uint8_t b = *src++;
            if ((unsigned)x < BOARD_LCD_H_RES) {
                uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                io->rgb[y * BOARD_LCD_H_RES + x] = be565(c);
            }
        }
    }
    return 1;
}

void ui_clear_msg_image(void)
{
    s_msg_has_image = false;
    s_dirty = true;
}

bool ui_msg_has_image(void)
{
    return s_msg_has_image;
}

esp_err_t ui_set_msg_jpeg(const uint8_t *jpg, int len)
{
    if (!jpg || len < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_msg_rgb) {
        s_msg_rgb = heap_caps_malloc(BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_msg_rgb) {
            s_msg_rgb = heap_caps_malloc(BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (!s_msg_rgb) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t pool[4096];
    JDEC jd;
    ui_jpeg_io_t io = {.jpg = jpg, .len = len, .pos = 0, .rgb = s_msg_rgb};
    JRESULT st = jd_prepare(&jd, ui_jpeg_in, pool, sizeof(pool), &io);
    if (st != JDR_OK) {
        ESP_LOGW(TAG, "jpeg prepare %d", (int)st);
        s_msg_has_image = false;
        return ESP_FAIL;
    }
    BYTE scale = 0;
    if (jd.width > BOARD_LCD_H_RES * 2 || jd.height > BOARD_LCD_V_RES * 2) {
        scale = 1;
    }
    st = jd_decomp(&jd, ui_jpeg_out, scale);
    if (st != JDR_OK) {
        ESP_LOGW(TAG, "jpeg decomp %d", (int)st);
        s_msg_has_image = false;
        return ESP_FAIL;
    }
    s_msg_has_image = true;
    s_dirty = true;
    ESP_LOGI(TAG, "jpeg %ux%u scale=%u", (unsigned)jd.width, (unsigned)jd.height, (unsigned)scale);
    return ESP_OK;
}

esp_err_t ui_set_quiet(bool quiet)
{
    s_model.quiet = quiet;
    s_dirty = true;
    return ESP_OK;
}

esp_err_t ui_set_clock(bool ok, int hour, int minute)
{
    s_model.clock_ok = ok;
    if (ok) {
        if (hour < 0) {
            hour = 0;
        }
        if (hour > 23) {
            hour = 23;
        }
        if (minute < 0) {
            minute = 0;
        }
        if (minute > 59) {
            minute = 59;
        }
        s_model.clock_hour = hour;
        s_model.clock_min = minute;
    }
    s_dirty = true;
    return ESP_OK;
}

esp_err_t ui_set_pan_deg(float pan_deg)
{
    if (fabsf(s_model.pan_deg - pan_deg) < 0.25f) {
        return ESP_OK;
    }
    s_model.pan_deg = pan_deg;
    if (s_screen == UI_SCREEN_EYES && !s_gaze_ok) {
        s_dirty = true;
    }
    return ESP_OK;
}

esp_err_t ui_set_gaze_az(float az_deg, bool valid)
{
    if (!valid) {
        s_gaze_ok = false;
        s_gaze_smooth = 0.0f;
        if (s_screen == UI_SCREEN_EYES) {
            s_dirty = true;
        }
        return ESP_OK;
    }
    s_gaze_ok = true;
    /* Light EMA so pupils track pan smoothly without overshoot. */
    s_gaze_smooth = 0.55f * az_deg + 0.45f * s_gaze_smooth;
    if (s_screen == UI_SCREEN_EYES) {
        s_dirty = true;
    }
    return ESP_OK;
}

esp_err_t ui_feed_radar(bool present, float distance_m, bool distance_ok, int64_t now_ms)
{
    ui_zone_t prev = s_model.zone;
    bool in_band = false;
    ui_zone_t want = UI_ZONE_VOID;

    if (present && !distance_ok) {
        /* No metre gate → NEAR info face; never FAR/EYES (SCREEN 24G rule). */
        in_band = true;
        want = UI_ZONE_NEAR;
    } else if (present && distance_ok && distance_m >= ZONE_MIN_M && distance_m <= s_far_max_m) {
        in_band = true;
        if (distance_m <= s_near_enter_m) {
            want = UI_ZONE_NEAR;
        } else if (distance_m >= s_near_exit_m) {
            want = UI_ZONE_FAR;
        } else {
            /* Hysteresis band: keep NEAR/FAR if already there. */
            want = (s_model.zone == UI_ZONE_NEAR) ? UI_ZONE_NEAR : UI_ZONE_FAR;
        }
    }

    if (!in_band) {
        s_enter_candidate_ms = 0;
        if (s_void_candidate_ms == 0) {
            s_void_candidate_ms = now_ms;
        }
        if ((now_ms - s_void_candidate_ms) >= s_void_ms) {
            s_model.zone = UI_ZONE_VOID;
        }
    } else {
        s_void_candidate_ms = 0;
        if (s_model.zone == UI_ZONE_VOID) {
            if (want != s_enter_want) {
                s_enter_want = want;
                s_enter_candidate_ms = now_ms;
            }
            if (s_enter_candidate_ms == 0) {
                s_enter_candidate_ms = now_ms;
            }
            if ((now_ms - s_enter_candidate_ms) >= ZONE_ENTER_MS) {
                s_model.zone = want;
            }
        } else if (s_model.zone == UI_ZONE_FAR && want == UI_ZONE_NEAR) {
            s_model.zone = UI_ZONE_NEAR;
        } else if (s_model.zone == UI_ZONE_NEAR && want == UI_ZONE_FAR) {
            s_model.zone = UI_ZONE_FAR;
        } else if (want == UI_ZONE_FAR || want == UI_ZONE_NEAR) {
            s_model.zone = want;
        }
    }

    if (s_model.zone != prev) {
        ESP_LOGI(TAG, "zone %d→%d d=%s%.2f present=%d", (int)prev, (int)s_model.zone,
                 distance_ok ? "" : "?", (double)distance_m, (int)present);
        s_dirty = true;
    }
    return ESP_OK;
}

ui_zone_t ui_current_zone(void)
{
    return s_model.zone;
}

void ui_set_distance_bands(float near_enter_m, float near_exit_m, float far_max_m, uint32_t void_ms)
{
    if (near_enter_m < 0.20f) {
        near_enter_m = 0.20f;
    }
    if (near_enter_m > 2.50f) {
        near_enter_m = 2.50f;
    }
    if (near_exit_m < near_enter_m + 0.05f) {
        near_exit_m = near_enter_m + 0.05f;
    }
    if (near_exit_m > 3.00f) {
        near_exit_m = 3.00f;
    }
    if (far_max_m < near_exit_m + 0.20f) {
        far_max_m = near_exit_m + 0.20f;
    }
    if (far_max_m > 8.00f) {
        far_max_m = 8.00f;
    }
    if (void_ms < 3000) {
        void_ms = 3000;
    }
    if (void_ms > 30000) {
        void_ms = 30000;
    }
    s_near_enter_m = near_enter_m;
    s_near_exit_m = near_exit_m;
    s_far_max_m = far_max_m;
    s_void_ms = (int64_t)void_ms;
    s_dirty = true;
    ESP_LOGI(TAG, "bands near=%.2f exit=%.2f far=%.2f void=%ums", (double)s_near_enter_m,
             (double)s_near_exit_m, (double)s_far_max_m, (unsigned)void_ms);
}

void ui_get_distance_bands(float *near_enter_m, float *near_exit_m, float *far_max_m, uint32_t *void_ms)
{
    if (near_enter_m) {
        *near_enter_m = s_near_enter_m;
    }
    if (near_exit_m) {
        *near_exit_m = s_near_exit_m;
    }
    if (far_max_m) {
        *far_max_m = s_far_max_m;
    }
    if (void_ms) {
        *void_ms = (uint32_t)s_void_ms;
    }
}

esp_err_t ui_set_wifi_qr(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }
    char esc_s[40];
    char esc_p[40];
    wifi_escape(ssid, esc_s, sizeof(esc_s));
    wifi_escape(password, esc_p, sizeof(esc_p));
    int n = snprintf(s_wifi_payload, sizeof(s_wifi_payload), "WIFI:T:WPA;S:%s;P:%s;;", esc_s, esc_p);
    if (n < 0 || n >= (int)sizeof(s_wifi_payload)) {
        s_wifi_qr_ready = false;
        s_qr_cached = false;
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(s_wifi_ssid_label, sizeof(s_wifi_ssid_label), "%s", ssid);
    snprintf(s_wifi_pass_label, sizeof(s_wifi_pass_label), "%s", password);
    s_wifi_qr_ready = true;
    s_qr_cached = false;
    s_dirty = true;
    ESP_LOGI(TAG, "WIFI QR ready (%d B) ssid=%s", n, ssid);
    return ESP_OK;
}

esp_err_t ui_show(ui_screen_t screen)
{
    s_screen = screen;
    s_force_screen = (screen == UI_SCREEN_PROV);
    s_viewing_msg = (screen == UI_SCREEN_MSG);
    if (screen == UI_SCREEN_PROV) {
        ui_poke_wake(BL_BOOT_GRACE_MS);
    }
    s_dirty = true;
    return ESP_OK;
}

void ui_set_keep_awake(bool on)
{
    s_keep_awake = on;
}

void ui_poke_wake(uint32_t duration_ms)
{
    const int64_t now = esp_timer_get_time() / 1000;
    const int64_t until = now + (int64_t)duration_ms;
    if (until > s_wake_until_ms) {
        s_wake_until_ms = until;
    }
    s_void_since_ms = 0;
}

bool ui_display_awake(void)
{
    return s_bl_on;
}

ui_screen_t ui_current_screen(void)
{
    return s_screen;
}

ui_lang_t ui_current_lang(void)
{
    return s_model.lang;
}

ui_gender_t ui_current_gender(void)
{
    return s_model.gender;
}

esp_err_t ui_tick(int64_t now_ms)
{
    if (!s_fb) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_viewing_msg && s_msg_until_ms && now_ms >= s_msg_until_ms) {
        close_msg();
    }
    /* Blink state for EYES */
    if (s_screen == UI_SCREEN_EYES) {
        if (s_blink_phase > 0) {
            s_blink_phase++;
            if (s_blink_phase >= 16) {
                s_blink_phase = 0;
                s_next_blink_ms = now_ms + 1600 + (int64_t)(esp_timer_get_time() % 2200);
            }
            s_dirty = true;
        } else if (now_ms >= s_next_blink_ms) {
            s_blink_phase = 1;
            s_dirty = true;
        }
    }
    static int64_t s_last_sec;
    int64_t sec = now_ms / 1000;
    bool anim = (s_screen == UI_SCREEN_GREET) || (s_screen == UI_SCREEN_PROV) ||
                (s_screen == UI_SCREEN_NO_RADAR) || (s_screen == UI_SCREEN_EYES) ||
                (now_ms < s_flash_until_ms);
    bool bl_tick = (sec != s_last_sec);
    bool eyes_pan = (s_screen == UI_SCREEN_EYES);
    if (s_dirty || anim || bl_tick || eyes_pan) {
        s_last_sec = sec;
        pick_and_render(now_ms);
    }
    return ESP_OK;
}

bool ui_on_touch(int64_t now_ms)
{
    ui_poke_wake(BL_TOUCH_WAKE_MS);
    s_flash_until_ms = now_ms + 120;
    s_dirty = true;
    apply_backlight_policy(now_ms);
    render(now_ms); /* <100ms local ack */

    if (s_screen == UI_SCREEN_PROV && now_ms < s_show_setup_until_ms) {
        s_show_setup_until_ms = 0;
        s_dirty = true;
        pick_and_render(now_ms);
        return false;
    }
    if (s_screen == UI_SCREEN_PROV || s_screen == UI_SCREEN_NO_RADAR) {
        return false;
    }
    if (s_screen == UI_SCREEN_MSG) {
        close_msg();
        pick_and_render(now_ms);
        return false; /* closing a message is not a "pat" event */
    }
    if (s_screen == UI_SCREEN_SLEEP) {
        s_force_near_until_ms = now_ms + FORCE_NEAR_MS;
        s_dirty = true;
        pick_and_render(now_ms);
        return true;
    }
    if (s_screen == UI_SCREEN_EYES) {
        if (s_model.unread_count > 0) {
            open_msg(now_ms);
            apply_backlight_policy(now_ms);
            render(now_ms);
        } else {
            s_force_near_until_ms = now_ms + FORCE_NEAR_MS;
            s_dirty = true;
            pick_and_render(now_ms);
        }
        return true;
    }
    if (s_screen == UI_SCREEN_GREET ||
        (s_screen == UI_SCREEN_INFO && s_model.unread_count > 0)) {
        open_msg(now_ms);
        apply_backlight_policy(now_ms);
        render(now_ms);
        return true;
    }
    /* INFO unread=0: show the setup QR (hotspot password) for a while. */
    s_show_setup_until_ms = now_ms + SHOW_SETUP_MS;
    s_dirty = true;
    pick_and_render(now_ms);
    return true;
}
