#pragma once

/** Small blocking HTTP(S) helpers shared by net_time and telegram_link. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** GET into resp (NUL-terminated, truncated at cap-1). True on HTTP 2xx with a body. */
bool net_http_get(const char *url, char *resp, int cap, int timeout_ms);

/** Percent-encode UTF-8 text for a URL query value. Returns false if it did not fit. */
bool net_http_urlencode(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
