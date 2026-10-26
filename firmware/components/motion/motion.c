#include "motion.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "device_cfg.h"
#include "radar_link.h"
#include "servo.h"

static const char *TAG = "motion";

#define ENTER_MS_DEFAULT  300
#define LEAVE_MS_DEFAULT  10000 /* target gone → hide after 10s */
#define PAN_DEAD_DEG      2.0f  /* ignore tiny az noise */
#define PAN_SOFT_MAX_DEFAULT 80.0f
#define PAN_FILT          0.35f /* EMA on radar az */
#define PAN_SLEW_DEFAULT  90.0f /* max pan speed deg/s — continuous feel */
#define PAN_LOOP_MS       40
#define LIFT_SETTLE_MS    500
#define RISE_LIFT_PCT     100.0f
#define HIDE_HOME_MS      2200  /* slow park via servo_home */
#define CLOUD_HOLD_MS     12000 /* remote then auto-resume follow */
#define CLOUD_RISE_MS     450
#define FAR_MAX_DEFAULT_M 5.0f

static motion_phase_t s_phase = MOTION_HIDDEN;
static bool s_follow_enabled;
static uint32_t s_enter_ms = ENTER_MS_DEFAULT;
static uint32_t s_leave_ms = LEAVE_MS_DEFAULT;
static float s_pan_slew_dps = PAN_SLEW_DEFAULT;
static float s_pan_soft_max = PAN_SOFT_MAX_DEFAULT;
static float s_far_max_m = FAR_MAX_DEFAULT_M;
static float s_cmd_pan;
static int s_pan_sign = 1;
static int64_t s_pause_until_ms;
static int64_t s_last_present_ms;
static int64_t s_present_since_ms;
static bool s_seeing;
static bool s_rise_asap; /* SoftAP「启动跟随」：跳过 ENTER 防抖立刻升起 */
static float s_az_filt;
static bool s_az_filt_init;
static int64_t s_last_pan_ms;
static volatile bool s_cloud_busy;

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float map_pan(float azimuth_deg)
{
    return clampf((float)s_pan_sign * azimuth_deg, -s_pan_soft_max, s_pan_soft_max);
}

static void rearm_phase_from_servos(void)
{
    s_cmd_pan = servo_get_pan_deg();
    s_az_filt_init = false;
    if (servo_get_lift_pct() < 50.0f) {
        s_phase = MOTION_HIDDEN;
    } else {
        s_phase = MOTION_TRACKING;
        s_last_pan_ms = esp_timer_get_time() / 1000;
    }
}

/** Seed presence gates from live radar so enable does not wait a full ENTER. */
static void seed_presence_from_radar(void)
{
    radar_target_t tgt;
    const int64_t now = esp_timer_get_time() / 1000;
    if (radar_link_get_target(&tgt) == ESP_OK && tgt.present) {
        s_seeing = true;
        s_present_since_ms = now - (int64_t)s_enter_ms; /* confirmed on next loop */
        s_last_present_ms = now;
        s_rise_asap = (servo_get_lift_pct() < 50.0f);
    } else {
        s_seeing = false;
        s_present_since_ms = 0;
        s_last_present_ms = 0;
        s_rise_asap = false;
    }
}

static void do_rise(void)
{
    s_phase = MOTION_RISING;
    s_rise_asap = false;
    ESP_LOGI(TAG, "rise");
    /* Center pan gently before lift if still off-axis. */
    if (fabsf(servo_get_pan_deg()) > 2.0f) {
        const float p0 = servo_get_pan_deg();
        const float l0 = servo_get_lift_pct();
        const int steps = 20;
        for (int i = 1; i <= steps; i++) {
            float t = (float)i / (float)steps;
            s_cmd_pan = p0 * (1.0f - t);
            servo_set_pan_deg(s_cmd_pan);
            servo_set_lift_pct(l0);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        s_cmd_pan = 0.0f;
        servo_set_pan_deg(0.0f);
    } else {
        s_cmd_pan = 0.0f;
    }
    servo_set_lift_pct(RISE_LIFT_PCT);
    vTaskDelay(pdMS_TO_TICKS(LIFT_SETTLE_MS));
    s_phase = MOTION_TRACKING;
    s_az_filt_init = false;
    s_last_pan_ms = esp_timer_get_time() / 1000;
}

static void do_hide(void)
{
    s_phase = MOTION_HIDING;
    s_rise_asap = false;
    ESP_LOGI(TAG, "hide: slow servo_home %dms", HIDE_HOME_MS);
    (void)servo_home(HIDE_HOME_MS);
    s_cmd_pan = 0.0f;
    servo_attach(false);
    s_phase = MOTION_HIDDEN;
    s_az_filt_init = false;
}

/* Continuous rate-limited pan toward filtered azimuth (keeps PWM on). */
static void pan_follow_smooth(float az_cmd, int64_t now)
{
    if (s_phase != MOTION_TRACKING || servo_get_lift_pct() < 95.0f) {
        return;
    }

    float dt_s = 0.04f;
    if (s_last_pan_ms > 0) {
        dt_s = (float)(now - s_last_pan_ms) / 1000.0f;
        if (dt_s < 0.01f) {
            dt_s = 0.01f;
        }
        if (dt_s > 0.12f) {
            dt_s = 0.12f;
        }
    }
    s_last_pan_ms = now;

    const float delta = az_cmd - s_cmd_pan;
    if (fabsf(delta) < PAN_DEAD_DEG) {
        return;
    }

    const float max_step = s_pan_slew_dps * dt_s;
    const float step = clampf(delta, -max_step, max_step);
    const float next = clampf(s_cmd_pan + step, -s_pan_soft_max, s_pan_soft_max);
    if (fabsf(next - s_cmd_pan) < 0.15f) {
        return;
    }
    s_cmd_pan = next;
    servo_set_pan_deg(s_cmd_pan);
}

static void motion_task(void *arg)
{
    (void)arg;
    int64_t last_log = 0;
    while (true) {
        const int64_t now = esp_timer_get_time() / 1000;
        radar_target_t tgt;
        if (radar_link_get_target(&tgt) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(PAN_LOOP_MS));
            continue;
        }

        if (now - last_log >= 2000) {
            last_log = now;
            ESP_LOGI(TAG,
                     "follow=%d phase=%s present=%d uart_p=%d y=%.2f az=%.1f cmd=%.1f",
                     (int)s_follow_enabled, motion_phase_name(), (int)tgt.present,
                     (int)tgt.uart_present, tgt.y_m, tgt.azimuth_deg, s_cmd_pan);
        }

        /* Follow off: SoftAP owns servos. Park only if still raised. */
        if (!s_follow_enabled) {
            s_rise_asap = false;
            if (s_phase == MOTION_TRACKING || s_phase == MOTION_RISING) {
                do_hide();
            } else if (s_phase == MOTION_HIDDEN && servo_get_lift_pct() > 5.0f) {
                /* Desync: SoftAP raised lift while follow off — leave it (manual). */
            }
            vTaskDelay(pdMS_TO_TICKS(PAN_LOOP_MS));
            continue;
        }

        bool present = tgt.present;
        if (tgt.from_uart && tgt.uart_present && tgt.distance_m > s_far_max_m) {
            present = false;
        }

        if (present) {
            if (!s_seeing) {
                s_present_since_ms = now;
                s_seeing = true;
            }
            s_last_present_ms = now;
        } else {
            s_seeing = false;
            s_rise_asap = false;
        }

        const bool confirmed =
            s_seeing && (now - s_present_since_ms) >= (int64_t)s_enter_ms;
        const bool want_up =
            s_last_present_ms > 0 && (now - s_last_present_ms) <= (int64_t)s_leave_ms;

        if (s_cloud_busy || now < s_pause_until_ms) {
            vTaskDelay(pdMS_TO_TICKS(PAN_LOOP_MS));
            continue;
        }

        if (s_phase == MOTION_RISING || s_phase == MOTION_HIDING) {
            vTaskDelay(pdMS_TO_TICKS(PAN_LOOP_MS));
            continue;
        }

        /* Recover if SoftAP parked hardware while phase was still TRACKING. */
        if (s_phase == MOTION_TRACKING && servo_get_lift_pct() < 50.0f) {
            ESP_LOGW(TAG, "phase desync (tracking but lift low) → hidden");
            s_phase = MOTION_HIDDEN;
            s_cmd_pan = servo_get_pan_deg();
            s_az_filt_init = false;
        }

        if (!want_up) {
            if (s_phase == MOTION_TRACKING) {
                do_hide();
            }
            vTaskDelay(pdMS_TO_TICKS(PAN_LOOP_MS));
            continue;
        }

        /* 「启动跟随」: rise as soon as present (skip ENTER). */
        if (s_phase == MOTION_HIDDEN && present && (confirmed || s_rise_asap)) {
            do_rise();
        }

        if (s_phase == MOTION_TRACKING && tgt.uart_present && tgt.azimuth_ok && tgt.from_uart) {
            const float mapped = map_pan(tgt.azimuth_deg);
            if (!s_az_filt_init) {
                s_az_filt = mapped;
                s_az_filt_init = true;
            } else {
                s_az_filt = PAN_FILT * mapped + (1.0f - PAN_FILT) * s_az_filt;
            }
            pan_follow_smooth(s_az_filt, now);
        }

        vTaskDelay(pdMS_TO_TICKS(PAN_LOOP_MS));
    }
}

esp_err_t motion_init(void)
{
    s_phase = MOTION_HIDDEN;
    s_follow_enabled = true;
    (void)device_cfg_erase_follow(); /* drop leftover NVS from older firmware */
    s_cmd_pan = 0.0f;
    s_pan_sign = 1;
    s_pause_until_ms = 0;
    s_last_present_ms = 0;
    s_present_since_ms = 0;
    s_seeing = false;
    s_rise_asap = false;
    s_az_filt_init = false;
    s_last_pan_ms = 0;
    s_cloud_busy = false;
    s_cmd_pan = servo_get_pan_deg();

    if (xTaskCreate(motion_task, "motion", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "follow %s at boot; leave=%ds; pan slew=%.0f°/s continuous",
             s_follow_enabled ? "ON" : "OFF", (int)(s_leave_ms / 1000), s_pan_slew_dps);
    return ESP_OK;
}

void motion_pause_auto(uint32_t duration_ms)
{
    s_pause_until_ms = esp_timer_get_time() / 1000 + (int64_t)duration_ms;
}

void motion_clear_pause(void)
{
    s_pause_until_ms = 0;
}

void motion_sync_pan_cmd(float deg)
{
    s_cmd_pan = deg;
}

void motion_notify_manual(void)
{
    s_cmd_pan = servo_get_pan_deg();
    s_az_filt_init = false;
    s_seeing = false;
    s_rise_asap = false;
    s_pause_until_ms = 0;
    /*
     * SoftAP owns the pose while follow is off. Keep phase HIDDEN even if lift
     * is at 100% — otherwise follow-off path sees TRACKING and runs do_hide
     * (slow home), which is the "升到上位又自己回零" bug.
     */
    if (!s_follow_enabled) {
        s_phase = MOTION_HIDDEN;
    } else {
        rearm_phase_from_servos();
    }
    ESP_LOGI(TAG, "manual override → phase=%s pan=%.1f lift=%.0f follow=%d", motion_phase_name(),
             s_cmd_pan, servo_get_lift_pct(), (int)s_follow_enabled);
}

void motion_claim_servos(void)
{
    /* Stop auto state machine without starting a second do_hide. */
    s_follow_enabled = false;
    s_pause_until_ms = 0;
    s_rise_asap = false;
    s_seeing = false;
    s_last_present_ms = 0;
    s_phase = MOTION_HIDDEN;
    s_cmd_pan = servo_get_pan_deg();
    s_az_filt_init = false;
    ESP_LOGI(TAG, "servos claimed by SoftAP (follow OFF, no auto-hide)");
}

void motion_set_follow_enabled(bool on)
{
    s_pause_until_ms = 0; /* always clear — start follow must never wait on jog pause */
    ESP_LOGI(TAG, "follow %s (session, not NVS)", on ? "ENABLED" : "DISABLED");
    if (on) {
        s_follow_enabled = true;
        rearm_phase_from_servos();
        seed_presence_from_radar();
        ESP_LOGI(TAG, "re-arm phase=%s lift=%.0f present=%d rise_asap=%d", motion_phase_name(),
                 servo_get_lift_pct(), (int)s_seeing, (int)s_rise_asap);
    } else {
        /* SoftAP Stop: leave TRACKING/RISING so motion_task runs slow do_hide.
         * Cloud takeover uses motion_claim_servos() (HIDDEN + keep pose). */
        s_follow_enabled = false;
        s_seeing = false;
        s_last_present_ms = 0;
        s_rise_asap = false;
        ESP_LOGI(TAG, "follow DISABLED (hide if raised)");
    }
}

bool motion_follow_enabled(void)
{
    return s_follow_enabled;
}

void motion_set_timing(uint32_t enter_ms, uint32_t leave_ms)
{
    if (enter_ms < 100) {
        enter_ms = 100;
    }
    if (enter_ms > 2000) {
        enter_ms = 2000;
    }
    if (leave_ms < 3000) {
        leave_ms = 3000;
    }
    if (leave_ms > 30000) {
        leave_ms = 30000;
    }
    s_enter_ms = enter_ms;
    s_leave_ms = leave_ms;
}

void motion_get_timing(uint32_t *enter_ms, uint32_t *leave_ms)
{
    if (enter_ms) {
        *enter_ms = s_enter_ms;
    }
    if (leave_ms) {
        *leave_ms = s_leave_ms;
    }
}

void motion_set_pan_limits(float slew_dps, float soft_max_deg)
{
    if (slew_dps < 20.0f) {
        slew_dps = 20.0f;
    }
    if (slew_dps > 180.0f) {
        slew_dps = 180.0f;
    }
    if (soft_max_deg < 30.0f) {
        soft_max_deg = 30.0f;
    }
    if (soft_max_deg > 90.0f) {
        soft_max_deg = 90.0f;
    }
    s_pan_slew_dps = slew_dps;
    s_pan_soft_max = soft_max_deg;
}

void motion_get_pan_limits(float *slew_dps, float *soft_max_deg)
{
    if (slew_dps) {
        *slew_dps = s_pan_slew_dps;
    }
    if (soft_max_deg) {
        *soft_max_deg = s_pan_soft_max;
    }
}

void motion_set_far_max_m(float metres)
{
    if (metres < 1.5f) {
        metres = 1.5f;
    }
    if (metres > 8.0f) {
        metres = 8.0f;
    }
    s_far_max_m = metres;
}

float motion_get_far_max_m(void)
{
    return s_far_max_m;
}

motion_phase_t motion_phase(void)
{
    return s_phase;
}

const char *motion_phase_name(void)
{
    switch (s_phase) {
    case MOTION_RISING:
        return "rising";
    case MOTION_TRACKING:
        return "tracking";
    case MOTION_HIDING:
        return "hiding";
    default:
        return "hidden";
    }
}

float motion_cmd_pan_deg(void)
{
    return s_cmd_pan;
}

void motion_set_pan_sign(int sign)
{
    s_pan_sign = (sign < 0) ? -1 : 1;
}

int motion_get_pan_sign(void)
{
    return s_pan_sign;
}

static void cloud_begin(uint32_t extra_ms)
{
    s_cloud_busy = true;
    (void)servo_attach(true);
    motion_pause_auto(CLOUD_HOLD_MS + extra_ms);
}

static void cloud_end(void)
{
    s_az_filt_init = false;
    if (s_follow_enabled) {
        rearm_phase_from_servos();
    } else {
        /* Keep follow-off from auto-parking a web pose. */
        s_phase = MOTION_HIDDEN;
        s_cmd_pan = servo_get_pan_deg();
    }
    motion_pause_auto(CLOUD_HOLD_MS);
    s_cloud_busy = false;
    ESP_LOGI(TAG, "cloud motion done phase=%s pan=%.1f lift=%.0f follow=%d",
             motion_phase_name(), s_cmd_pan, servo_get_lift_pct(), (int)s_follow_enabled);
}

static void cloud_ensure_risen(void)
{
    if (servo_get_lift_pct() < 50.0f) {
        ESP_LOGI(TAG, "cloud: rise before yaw");
        servo_set_lift_pct(RISE_LIFT_PCT);
        vTaskDelay(pdMS_TO_TICKS(CLOUD_RISE_MS));
    }
}

esp_err_t motion_cloud_jog(float lift_pct, float pan_deg)
{
    lift_pct = clampf(lift_pct, 0.0f, 100.0f);
    pan_deg = clampf(pan_deg, -90.0f, 90.0f);
    cloud_begin(0);

    const bool hidden = servo_get_lift_pct() < 40.0f;
    const bool want_pan = fabsf(pan_deg) > 2.0f;
    const bool want_up = lift_pct >= 40.0f;

    if (want_up) {
        if (hidden) {
            cloud_ensure_risen();
        }
        servo_set_lift_pct(lift_pct);
        s_cmd_pan = pan_deg;
        servo_set_pan_deg(pan_deg);
    } else if (want_pan && hidden) {
        /* Hatch scrape: never yaw while parked. Rise, then turn; leave lift up. */
        cloud_ensure_risen();
        s_cmd_pan = pan_deg;
        servo_set_pan_deg(pan_deg);
    } else {
        (void)servo_home(HIDE_HOME_MS);
        s_cmd_pan = 0.0f;
    }

    cloud_end();
    return ESP_OK;
}

esp_err_t motion_cloud_preset(const char *name, int repeat)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (repeat < 1) {
        repeat = 1;
    }
    if (repeat > 5) {
        repeat = 5;
    }

    cloud_begin((uint32_t)repeat * 2500u);

    if (strcmp(name, "rise") == 0) {
        cloud_ensure_risen();
        servo_set_lift_pct(RISE_LIFT_PCT);
    } else if (strcmp(name, "hide") == 0) {
        (void)servo_home(HIDE_HOME_MS);
        s_cmd_pan = 0.0f;
    } else if (strcmp(name, "peek") == 0) {
        cloud_ensure_risen();
        for (int i = 0; i < repeat; i++) {
            s_cmd_pan = 25.0f;
            servo_set_pan_deg(25.0f);
            vTaskDelay(pdMS_TO_TICKS(550));
            s_cmd_pan = 0.0f;
            servo_set_pan_deg(0.0f);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    } else if (strcmp(name, "look_around") == 0) {
        cloud_ensure_risen();
        for (int i = 0; i < repeat; i++) {
            s_cmd_pan = -40.0f;
            servo_set_pan_deg(-40.0f);
            vTaskDelay(pdMS_TO_TICKS(500));
            s_cmd_pan = 40.0f;
            servo_set_pan_deg(40.0f);
            vTaskDelay(pdMS_TO_TICKS(500));
            s_cmd_pan = 0.0f;
            servo_set_pan_deg(0.0f);
            vTaskDelay(pdMS_TO_TICKS(350));
        }
    } else if (strcmp(name, "wave") == 0) {
        cloud_ensure_risen();
        for (int i = 0; i < repeat; i++) {
            s_cmd_pan = 28.0f;
            servo_set_pan_deg(28.0f);
            vTaskDelay(pdMS_TO_TICKS(280));
            s_cmd_pan = -28.0f;
            servo_set_pan_deg(-28.0f);
            vTaskDelay(pdMS_TO_TICKS(280));
            s_cmd_pan = 28.0f;
            servo_set_pan_deg(28.0f);
            vTaskDelay(pdMS_TO_TICKS(280));
            s_cmd_pan = 0.0f;
            servo_set_pan_deg(0.0f);
            vTaskDelay(pdMS_TO_TICKS(220));
        }
    } else {
        ESP_LOGW(TAG, "unknown cloud preset %s", name);
        cloud_end();
        return ESP_ERR_NOT_FOUND;
    }

    cloud_end();
    return ESP_OK;
}
