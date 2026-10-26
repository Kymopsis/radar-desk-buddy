#include "radar_link.h"

#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "radar_protocol.h"

static const char *TAG = "radar_link";

#define RADAR_UART_NUM UART_NUM_1
#define RADAR_UART_BUF 4096
#define RADAR_STALE_MS 1200
#define GPIO_ENTER_MS  300
#define GPIO_LEAVE_MS  2500

/* HLK stock UART is typically 115200; Andar demo boards often use 1382400. */
static const int s_baud_table[] = {115200, 1382400, 921600, 460800};
#define BAUD_COUNT (sizeof(s_baud_table) / sizeof(s_baud_table[0]))

static SemaphoreHandle_t s_lock;
static radar_target_t s_tgt;
static radar_fw_mode_t s_fw = RADAR_FW_UNKNOWN;
static uint32_t s_frames;
static uint32_t s_rx_bytes;
static uint32_t s_sof_hits;
static bool s_have_uart;
static int64_t s_last_frame_ms;
static int s_baud = 115200;
static int s_rx_gpio = BOARD_PIN_RADAR_RXD;
static radar_tf_decoder_t s_decoder;

static bool s_gpio_raw;
static bool s_gpio_debounced;
static bool s_gpio_seen_high; /* if never high, fall back to UART for lift */
static int64_t s_gpio_edge_ms;

static radar_fw_mode_t map_mode(radar_wire_mode_t mode)
{
    switch (mode) {
    case RADAR_WIRE_2D:
        return RADAR_FW_2D;
    case RADAR_WIRE_3D:
        return RADAR_FW_3D;
    case RADAR_WIRE_HLK:
        return RADAR_FW_HLK;
    default:
        return RADAR_FW_UNKNOWN;
    }
}

static void presence_gpio_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PIN_RADAR_PRESENCE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    s_gpio_raw = gpio_get_level(BOARD_PIN_RADAR_PRESENCE) != 0;
    s_gpio_debounced = s_gpio_raw;
    s_gpio_edge_ms = esp_timer_get_time() / 1000;
    if (s_gpio_raw) {
        s_gpio_seen_high = true;
    }
}

static void presence_gpio_poll(int64_t now)
{
    const bool raw = gpio_get_level(BOARD_PIN_RADAR_PRESENCE) != 0;
    if (raw) {
        s_gpio_seen_high = true;
    }
    if (raw != s_gpio_raw) {
        s_gpio_raw = raw;
        s_gpio_edge_ms = now;
        return;
    }
    const int64_t need = raw ? GPIO_ENTER_MS : GPIO_LEAVE_MS;
    if ((now - s_gpio_edge_ms) >= need) {
        s_gpio_debounced = raw;
    }
}

static void accept_event(const radar_wire_event_t *event)
{
    const int64_t now = esp_timer_get_time() / 1000;
    ++s_frames;
    const radar_fw_mode_t mode = map_mode(event->mode);
    if (mode != RADAR_FW_UNKNOWN) {
        s_fw = mode;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_have_uart = true;
        s_last_frame_ms = now;
        xSemaphoreGive(s_lock);
    }
    if (!event->has_target_sample) {
        return;
    }

    radar_target_t next = {
        .uart_present = event->sample.present,
        .azimuth_ok = event->sample.azimuth_ok,
        .speed_mps_valid = event->sample.speed_mps_valid,
        .x_m = event->sample.x_m,
        .y_m = event->sample.y_m,
        .z_m = event->sample.z_m,
        .azimuth_deg = event->sample.azimuth_deg,
        .elevation_deg = event->sample.elevation_deg,
        .distance_m = event->sample.distance_m,
        .distance_cm = event->sample.distance_m * 100.0f,
        .radial_speed_mps = event->sample.radial_speed_mps,
        .doppler_raw = event->sample.doppler_raw,
        .track_id = event->sample.track_id,
        .target_count = event->sample.target_count,
        .blip_count = 0,
        .fw_mode = mode == RADAR_FW_UNKNOWN ? s_fw : mode,
        .ts_ms = now,
        .from_uart = true,
    };
    int n = event->sample.blip_count;
    if (n > RADAR_LINK_MAX_BLIPS) {
        n = RADAR_LINK_MAX_BLIPS;
    }
    if (n > RADAR_MAX_BLIPS) {
        n = RADAR_MAX_BLIPS;
    }
    next.blip_count = n;
    for (int i = 0; i < n; ++i) {
        next.blips[i].azimuth_deg = event->sample.blips[i].azimuth_deg;
        next.blips[i].distance_m = event->sample.blips[i].distance_m;
        next.blips[i].x_m = event->sample.blips[i].x_m;
        next.blips[i].y_m = event->sample.blips[i].y_m;
        next.blips[i].track_id = event->sample.blips[i].track_id;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_tgt = next;
        s_have_uart = true;
        xSemaphoreGive(s_lock);
    }
}

static void radar_hw_reset(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PIN_RADAR_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_PIN_RADAR_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_PIN_RADAR_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(80));
}

static esp_err_t uart_apply(int baud)
{
    ESP_RETURN_ON_ERROR(uart_set_baudrate(RADAR_UART_NUM, baud), TAG, "baud");
    ESP_RETURN_ON_ERROR(uart_set_pin(RADAR_UART_NUM, BOARD_PIN_RADAR_TXD, BOARD_PIN_RADAR_RXD,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "pins");
    uart_flush_input(RADAR_UART_NUM);
    radar_tf_decoder_init(&s_decoder);
    s_baud = baud;
    s_rx_gpio = BOARD_PIN_RADAR_RXD;
    ESP_LOGW(TAG, "UART retune baud=%d RX=GPIO%d (presence stays GPIO%d)", baud, s_rx_gpio,
             (int)BOARD_PIN_RADAR_PRESENCE);
    return ESP_OK;
}

static void radar_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    radar_wire_event_t event;
    int64_t last_log_ms = 0;
    int64_t window_start_ms = esp_timer_get_time() / 1000;
    uint32_t window_bytes = 0;
    uint32_t window_frames = 0;
    int baud_idx = 0;

    while (true) {
        const int64_t now = esp_timer_get_time() / 1000;
        presence_gpio_poll(now);

        const int count = uart_read_bytes(RADAR_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(40));
        if (count > 0) {
            s_rx_bytes += (uint32_t)count;
            window_bytes += (uint32_t)count;
            for (int i = 0; i < count; ++i) {
                if (buf[i] == RADAR_TF_SOF) {
                    ++s_sof_hits;
                }
                if (radar_tf_decoder_feed(&s_decoder, buf[i], &event)) {
                    ++window_frames;
                    accept_event(&event);
                }
            }
        }

        if (now - last_log_ms >= 2000) {
            last_log_ms = now;
            radar_target_t snap = {0};
            radar_link_get_target(&snap);
            ESP_LOGI(TAG,
                     "rx=%lu sof=%lu frames=%lu baud=%d | uart=%d gpio=%d present=%d "
                     "fw=%d n=%d az_ok=%d xyz=(%.2f,%.2f,%.2f) az=%.1f",
                     (unsigned long)s_rx_bytes, (unsigned long)s_sof_hits,
                     (unsigned long)s_frames, s_baud, (int)snap.from_uart, (int)snap.gpio_present,
                     (int)snap.present, (int)snap.fw_mode, snap.target_count, (int)snap.azimuth_ok,
                     snap.x_m, snap.y_m, snap.z_m, snap.azimuth_deg);
        }

        /* No valid TinyFrame for 2.5s → hunt baud on TX0 only (keep GPIO16 for OUT). */
        if (now - window_start_ms >= 2500) {
            if (window_frames == 0) {
                baud_idx = (baud_idx + 1) % (int)BAUD_COUNT;
                uart_apply(s_baud_table[baud_idx]);
                if (baud_idx == 0) {
                    radar_hw_reset();
                    presence_gpio_init();
                }
            }
            window_start_ms = now;
            window_bytes = 0;
            window_frames = 0;
        }
    }
}

esp_err_t radar_link_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_tgt, 0, sizeof(s_tgt));
    radar_tf_decoder_init(&s_decoder);
    radar_hw_reset();
    presence_gpio_init();

    const uart_config_t cfg = {
        .baud_rate = s_baud_table[0],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    s_baud = s_baud_table[0];
    s_rx_gpio = BOARD_PIN_RADAR_RXD;
    ESP_RETURN_ON_ERROR(uart_driver_install(RADAR_UART_NUM, RADAR_UART_BUF, 0, 0, NULL, 0), TAG,
                        "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(RADAR_UART_NUM, &cfg), TAG, "uart cfg");
    ESP_RETURN_ON_ERROR(uart_set_pin(RADAR_UART_NUM, BOARD_PIN_RADAR_TXD, s_rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart pins");
    if (xTaskCreate(radar_rx_task, "radar_rx", 6144, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART1 baud=%d TX=%d RX=%d; presence OUT→GPIO%d", s_baud,
             (int)BOARD_PIN_RADAR_TXD, s_rx_gpio, (int)BOARD_PIN_RADAR_PRESENCE);
    return ESP_OK;
}

esp_err_t radar_link_get_target(radar_target_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    presence_gpio_poll(now);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_tgt;
    const bool uart = s_have_uart && now - s_last_frame_ms <= RADAR_STALE_MS;
    const bool fresh_target = uart && s_tgt.ts_ms != 0 && now - s_tgt.ts_ms <= RADAR_STALE_MS;
    const radar_fw_mode_t mode = s_fw;
    xSemaphoreGive(s_lock);

    if (!fresh_target) {
        memset(out, 0, sizeof(*out));
        out->from_uart = uart;
        out->ts_ms = now;
    }
    out->fw_mode = mode;
    out->from_uart = uart;
    out->gpio_present = s_gpio_debounced;
    out->uart_present = fresh_target && out->uart_present;
    /* Lift gate: prefer hardware OUT once it has ever gone high; else UART. */
    if (s_gpio_seen_high) {
        out->present = s_gpio_debounced;
    } else {
        out->present = out->uart_present;
    }
    if (!fresh_target) {
        out->azimuth_ok = false;
    }
    return ESP_OK;
}

radar_fw_mode_t radar_link_fw_mode(void)
{
    return s_fw;
}

uint32_t radar_link_frame_count(void)
{
    return s_frames;
}

int radar_link_baud_rate(void)
{
    return s_baud;
}
