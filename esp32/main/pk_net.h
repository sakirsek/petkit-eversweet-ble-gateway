/* pk_net.h - Wi-Fi, SNTP time sync, and Telegram HTTPS transport.
 *
 * Handles Wi-Fi connection, SNTP clock synchronization, and sending
 * HTML-formatted messages to Telegram Bot API with RFC 3986 URL encoding.
 */
#ifndef PK_NET_H
#define PK_NET_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t pk_net_wifi_sta_start(const char *ssid, const char *password, uint32_t timeout_ms);
void pk_net_wifi_sta_stop(void);

bool pk_net_sntp_sync(uint32_t timeout_ms);
/* Is the wall clock one we may hand to the core? False means still 1970. */
bool pk_net_time_valid(void);

/* Signal of the access point, sampled when the last session got its IP.
 * 0 means we have not associated yet. */
int pk_net_rssi(void);
/* Free internal heap sampled just after the last Telegram POST completed,
 * i.e. with the TLS session still standing - the tightest point of the
 * radio-on WiFi half of the cycle. 0 until the first send. */
uint32_t pk_net_heap_phase(void);

bool pk_net_telegram_send(const char *token, const char *chat_id, const char *html_text);

#endif /* PK_NET_H */
