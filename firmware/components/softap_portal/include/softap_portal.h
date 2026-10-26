#pragma once

/**
 * SoftAP + captive-style HTTP for phase-1 bring-up.
 * AP SSID: DeskBuddy-XXXX; password is random per device (NVS) and shown on the screen.
 * Open http://mod.config/ (local DNS → SoftAP IP; 192.168.4.1 still works).
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Portal hostname served by SoftAP DNS (A → AP IPv4): the setup page address. */
#define SOFTAP_PORTAL_HOST "mod.config"

esp_err_t softap_portal_start(void);

const char *softap_portal_ssid(void);
const char *softap_portal_pass(void);
const char *softap_portal_host(void);

/** SoftAP associated STA count (keep UI awake while phone is on the portal). */
int softap_portal_sta_count(void);

#ifdef __cplusplus
}
#endif
