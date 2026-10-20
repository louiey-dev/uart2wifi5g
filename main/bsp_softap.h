#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SoftAP, start WiFi AP mode, and prepare the UDP subsystem.
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t bsp_init_softap(void);

/**
 * @brief Send raw data (byte array or text string) via UDP to the configured destination.
 *
 * @param data Pointer to the buffer to send.
 * @param len Length in bytes of the buffer.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t bsp_send_udp(const void *data, size_t len);

/**
 * @brief Convenience helper to send a null-terminated text string via UDP.
 *
 * @param str Null-terminated string.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t bsp_send_udp_str(const char *str);

/**
 * @brief Set the UDP destination IP address and port at runtime.
 *
 * @param ip_str Destination IPv4 address string (e.g., "255.255.255.255" or "192.168.4.2").
 * @param port Destination UDP port number.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG on invalid input.
 */
esp_err_t bsp_set_udp_dest(const char *ip_str, uint16_t port);

#ifdef __cplusplus
}
#endif
