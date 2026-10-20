#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize UART1 communicating with external device and spawn RX task.
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t bsp_uart_init(void);

/**
 * @brief Task handler receiving incoming UART data from external device and sending to host via UDP.
 *
 * @param pvParameters FreeRTOS task parameter (can be NULL).
 */
void bsp_uart_rx_handler(void *pvParameters);

/**
 * @brief Send data to external device via UART.
 *
 * @param data Pointer to buffer to send.
 * @param len Number of bytes to send.
 * @return Number of bytes written, or negative on error.
 */
int bsp_uart_send(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
