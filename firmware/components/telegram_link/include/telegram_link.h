#pragma once

/**
 * Level 2: talk to the desk buddy from Telegram, with no server of your own.
 * The ESP32 long-polls the Bot API (getUpdates) over HTTPS. The first chat that
 * sends /start becomes the only chat it listens to; other chats are ignored.
 *
 *   any text  → shown on the round screen (touch to open, touch again to close)
 *   /wave     → the figure waves        /peek → peeks up
 *   /unpair   → forget this chat (next /start pairs again)
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t telegram_link_start(void);

/** Portal saved a new token — re-check the bot now. */
void telegram_link_kick(void);

/** off | wait_wifi | wait_pair | paired | bad_token | err */
const char *telegram_link_state_str(void);

/** Bot username from getMe ("" until known), for the portal's t.me link. */
const char *telegram_link_bot_username(void);

/** MSG face closed — tell the paired chat it was seen. */
void telegram_link_notify_msg_displayed(void);

#ifdef __cplusplus
}
#endif
