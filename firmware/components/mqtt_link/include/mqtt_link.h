#pragma once

/**
 * STA IP → SNTP → POST /claim → MQTT (LWT + SUB own tree + PUB state).
 * After MQTT up: GET inbox/latest; peer → weather/clock chip; msg text cached;
 * cmd set/home/clear_unread; motion jog/preset (pause follow ~12s then resume).
 * ota still logged only.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MQTT_LINK_IDLE = 0,
    MQTT_LINK_WAIT_IP,
    MQTT_LINK_SNTP,
    MQTT_LINK_CLAIM,
    MQTT_LINK_CONNECTING,
    MQTT_LINK_UP,
    MQTT_LINK_ERR,
} mqtt_link_state_t;

esp_err_t mqtt_link_start(void);

/** SoftAP saved a new bind / claim_base — retry claim / reconnect. */
void mqtt_link_kick(void);

mqtt_link_state_t mqtt_link_state(void);
const char *mqtt_link_state_str(void);
const char *mqtt_link_last_error(void);

/** SCREEN: not MSG-close / PROV. No-op if MQTT down or stealth. */
void mqtt_link_notify_touch(void);

/** MSG face closed (touch or auto). PUB ack displayed + remember id in NVS. */
void mqtt_link_notify_msg_displayed(void);

#ifdef __cplusplus
}
#endif
