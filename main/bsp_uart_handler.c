#include "bsp_uart_handler.h"
#include "bsp_softap.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_EXTERNAL_UART_NUM
#define CONFIG_EXTERNAL_UART_NUM UART_NUM_1
#endif

#ifndef CONFIG_EXTERNAL_UART_TX_PIN
#define CONFIG_EXTERNAL_UART_TX_PIN 8
#endif

#ifndef CONFIG_EXTERNAL_UART_RX_PIN
#define CONFIG_EXTERNAL_UART_RX_PIN 9
#endif

#ifndef CONFIG_EXTERNAL_UART_BAUD_RATE
#define CONFIG_EXTERNAL_UART_BAUD_RATE 115200
#endif

#ifndef CONFIG_EXTERNAL_UART_RX_BUF_SIZE
#define CONFIG_EXTERNAL_UART_RX_BUF_SIZE 2048
#endif

static const char *TAG = "bsp_uart";
static TaskHandle_t s_uart_rx_task_handle = NULL;
static QueueHandle_t s_uart_queue = NULL;

int bsp_uart_send(const void *data, size_t len) {
  if (data == NULL || len == 0) {
    return 0;
  }
  int written = uart_write_bytes(CONFIG_EXTERNAL_UART_NUM, data, len);
  uart_wait_tx_done(CONFIG_EXTERNAL_UART_NUM, pdMS_TO_TICKS(100));
  ESP_LOGI(TAG, "UART%d TX [%d/%zu bytes]:", CONFIG_EXTERNAL_UART_NUM, written,
           len);
  //   ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_INFO);
  return written;
}

void bsp_uart_rx_handler(void *pvParameters) {
  (void)pvParameters;
  uart_event_t event;
  uint8_t *rx_buf = (uint8_t *)malloc(CONFIG_EXTERNAL_UART_RX_BUF_SIZE + 1);
  if (rx_buf == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for UART RX buffer");
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "UART%d event listener started (TX: IO%d, RX: IO%d, Baud: %d)",
           CONFIG_EXTERNAL_UART_NUM, CONFIG_EXTERNAL_UART_TX_PIN,
           CONFIG_EXTERNAL_UART_RX_PIN, CONFIG_EXTERNAL_UART_BAUD_RATE);

  while (1) {
    if (xQueueReceive(s_uart_queue, &event, portMAX_DELAY) == pdTRUE) {
      switch (event.type) {
      case UART_DATA:
      case UART_BUFFER_FULL: {
        size_t buffered_size = 0;
        uart_get_buffered_data_len(CONFIG_EXTERNAL_UART_NUM, &buffered_size);
        while (buffered_size > 0) {
          int to_read = (buffered_size < CONFIG_EXTERNAL_UART_RX_BUF_SIZE)
                            ? (int)buffered_size
                            : CONFIG_EXTERNAL_UART_RX_BUF_SIZE;
          int rx_bytes = uart_read_bytes(CONFIG_EXTERNAL_UART_NUM, rx_buf,
                                         to_read, pdMS_TO_TICKS(50));
          if (rx_bytes > 0) {
            ESP_LOGI(TAG, "UART%d RX [%d bytes]:", CONFIG_EXTERNAL_UART_NUM,
                     rx_bytes);
            // ESP_LOG_BUFFER_HEX_LEVEL(TAG, rx_buf, rx_bytes, ESP_LOG_INFO);
            esp_err_t err = bsp_send_udp(rx_buf, rx_bytes);
            if (err != ESP_OK) {
              ESP_LOGW(TAG, "bsp_send_udp returned error: %s",
                       esp_err_to_name(err));
            }
          }
          uart_get_buffered_data_len(CONFIG_EXTERNAL_UART_NUM, &buffered_size);
        }
        break;
      }
      case UART_FRAME_ERR:
        ESP_LOGW(TAG,
                 "UART%d FRAME ERROR detected! Please verify baud rate (%d) "
                 "and wiring (TX->RX, RX->TX, GND).",
                 CONFIG_EXTERNAL_UART_NUM, CONFIG_EXTERNAL_UART_BAUD_RATE);
        break;
      case UART_PARITY_ERR:
        ESP_LOGW(TAG, "UART%d PARITY ERROR detected!",
                 CONFIG_EXTERNAL_UART_NUM);
        break;
      case UART_FIFO_OVF:
        ESP_LOGW(TAG, "UART%d HW FIFO overflow, flushing input",
                 CONFIG_EXTERNAL_UART_NUM);
        uart_flush_input(CONFIG_EXTERNAL_UART_NUM);
        xQueueReset(s_uart_queue);
        break;
      default:
        ESP_LOGD(TAG, "UART%d event: %d", CONFIG_EXTERNAL_UART_NUM, event.type);
        break;
      }
    }
  }

  free(rx_buf);
  vTaskDelete(NULL);
}

esp_err_t bsp_uart_init(void) {
  const uart_config_t uart_config = {
      .baud_rate = CONFIG_EXTERNAL_UART_BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_LOGI(TAG, "Initializing UART%d (TX: IO%d, RX: IO%d, Baud: %d)...",
           CONFIG_EXTERNAL_UART_NUM, CONFIG_EXTERNAL_UART_TX_PIN,
           CONFIG_EXTERNAL_UART_RX_PIN, CONFIG_EXTERNAL_UART_BAUD_RATE);

  gpio_reset_pin(CONFIG_EXTERNAL_UART_TX_PIN);
  gpio_reset_pin(CONFIG_EXTERNAL_UART_RX_PIN);
#if SOC_RTCIO_PIN_COUNT > 0
  if (rtc_gpio_is_valid_gpio(CONFIG_EXTERNAL_UART_TX_PIN)) {
    rtc_gpio_deinit(CONFIG_EXTERNAL_UART_TX_PIN);
  }
  if (rtc_gpio_is_valid_gpio(CONFIG_EXTERNAL_UART_RX_PIN)) {
    rtc_gpio_deinit(CONFIG_EXTERNAL_UART_RX_PIN);
  }
#endif

  esp_err_t ret = uart_param_config(CONFIG_EXTERNAL_UART_NUM, &uart_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = uart_set_pin(CONFIG_EXTERNAL_UART_NUM, CONFIG_EXTERNAL_UART_TX_PIN,
                     CONFIG_EXTERNAL_UART_RX_PIN, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
    return ret;
  }

  gpio_set_pull_mode(CONFIG_EXTERNAL_UART_RX_PIN, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(CONFIG_EXTERNAL_UART_TX_PIN, GPIO_PULLUP_ONLY);
  gpio_set_drive_capability(CONFIG_EXTERNAL_UART_TX_PIN, GPIO_DRIVE_CAP_3);

  ret = uart_driver_install(
      CONFIG_EXTERNAL_UART_NUM, CONFIG_EXTERNAL_UART_RX_BUF_SIZE * 2,
      CONFIG_EXTERNAL_UART_RX_BUF_SIZE * 2, 50, &s_uart_queue, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // Configure RX timeout threshold (10 symbol periods ~ 1ms at 115200 baud)
  uart_set_rx_timeout(CONFIG_EXTERNAL_UART_NUM, 10);

  BaseType_t task_ret = xTaskCreate(bsp_uart_rx_handler, "bsp_uart_rx", 4096,
                                    NULL, 10, &s_uart_rx_task_handle);
  if (task_ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create UART RX task");
    return ESP_FAIL;
  }

  ESP_LOGI(
      TAG,
      "UART%d initialized with event queue and RX task running successfully",
      CONFIG_EXTERNAL_UART_NUM);
  return ESP_OK;
}
