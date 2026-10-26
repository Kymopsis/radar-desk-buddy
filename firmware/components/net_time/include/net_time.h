#pragma once

/**
 * Level 1 "no server" services: once home Wi-Fi has an IP, sync the clock (SNTP),
 * look up the city saved in the portal (Open-Meteo geocoding) and refresh the
 * weather every 15 minutes (Open-Meteo forecast). The local UTC offset comes from
 * the same weather call, so daylight saving time follows without a time-zone table.
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t net_time_start(void);

/** Home Wi-Fi has an IP. */
bool net_time_online(void);

/** Wall clock is set (SNTP done). TLS needs this. */
bool net_time_clock_ok(void);

/** Portal saved a new city — geocode + refresh weather now. */
void net_time_kick(void);

/** A self-hosted server (Level 3) pushes weather itself; stop polling Open-Meteo. */
void net_time_set_weather_from_cloud(bool on);

/** Last weather/geocode problem for the portal ("" when fine). */
const char *net_time_last_error(void);

#ifdef __cplusplus
}
#endif
