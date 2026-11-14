#pragma once

#include <stdbool.h>

/* Station-only Wi-Fi: no SoftAP, no provisioning portal. Credentials come from
 * NVS, so nothing is broadcast and there is nothing to configure over the air. */

/* Connect to the configured network. Blocks until connected or `timeout_ms`
 * elapses. Returns true once an IP address is in hand. */
bool wifi_connect(const char *ssid, const char *pass, int timeout_ms);

/* True once the station has an IP. */
bool wifi_is_connected(void);

/* Drop the connection and release the driver. */
void wifi_disconnect(void);