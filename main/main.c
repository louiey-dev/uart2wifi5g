#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "bsp_softap.h"
#include "bsp_uart_handler.h"

static const char *TAG = "main";

static void init_console(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "        ESP32 UART to WiFi UDP Bridge           ");
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "Console output: USB/UART default");
    ESP_LOGI(TAG, "External UART : TX = IO2, RX = IO3");
}

void app_main(void)
{
    // 1. Initialize console
    init_console();

    // 2. Initialize NVS (needed by WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 3. Initialize SoftAP and UDP
    ESP_LOGI(TAG, "Initializing SoftAP and UDP subsystem...");
    ESP_ERROR_CHECK(bsp_init_softap());

    // 4. Initialize UART for external device and launch RX handler task
    ESP_LOGI(TAG, "Initializing External UART and starting RX handler...");
    ESP_ERROR_CHECK(bsp_uart_init());

    ESP_LOGI(TAG, "System ready: bridge between UART (IO3) and WiFi UDP is running.");
}
