#include "bsp_softap.h"
#include "bsp_uart_handler.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#ifndef CONFIG_ESP_WIFI_SSID
#define CONFIG_ESP_WIFI_SSID "myssid"
#endif

#ifndef CONFIG_ESP_WIFI_PASSWORD
#define CONFIG_ESP_WIFI_PASSWORD "mypassword"
#endif

#ifndef CONFIG_ESP_WIFI_CHANNEL
#define CONFIG_ESP_WIFI_CHANNEL 36
#endif

#if !defined(CONFIG_ESP_WIFI_BAND_5G) && !defined(CONFIG_ESP_WIFI_BAND_2G) &&  \
    !defined(CONFIG_ESP_WIFI_BAND_AUTO)
#define CONFIG_ESP_WIFI_BAND_5G 1
#endif

#ifndef CONFIG_ESP_MAX_STA_CONN
#define CONFIG_ESP_MAX_STA_CONN 4
#endif

#ifndef CONFIG_UDP_DEST_IP
#define CONFIG_UDP_DEST_IP "192.168.4.2"
#endif

#ifndef CONFIG_UDP_DEST_PORT
#define CONFIG_UDP_DEST_PORT 3333
#endif

#if CONFIG_ESP_GTK_REKEYING_ENABLE
#define GTK_REKEY_INTERVAL CONFIG_ESP_GTK_REKEY_INTERVAL
#else
#define GTK_REKEY_INTERVAL 0
#endif

static const char *TAG = "bsp_softap";

static int s_udp_sock = -1;
static struct sockaddr_in s_dest_addr;
static struct sockaddr_in s_subnet_bcast_addr;
static struct sockaddr_in s_sta_unicast_addr;
static struct sockaddr_in s_active_client_addr;
static bool s_has_active_client = false;
static int s_connected_stations = 0;
static SemaphoreHandle_t s_udp_mutex = NULL;
static esp_netif_t *s_ap_netif = NULL;
static TaskHandle_t s_udp_rx_task_handle = NULL;

// TCP Server support for Tera Term
static int s_tcp_listen_sock = -1;
static int s_tcp_client_sock = -1;
static TaskHandle_t s_tcp_server_task_handle = NULL;

static int create_and_bind_udp_socket(void) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
    return -1;
  }

  int broadcast_enable = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable,
                 sizeof(broadcast_enable)) < 0) {
    ESP_LOGW(TAG, "Failed to set SO_BROADCAST option: errno %d", errno);
  }

  int reuse_enable = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_enable,
                 sizeof(reuse_enable)) < 0) {
    ESP_LOGW(TAG, "Failed to set SO_REUSEADDR option: errno %d", errno);
  }

  // Bind socket to local port CONFIG_UDP_DEST_PORT
  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(CONFIG_UDP_DEST_PORT);
  if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind UDP socket to port %d: errno %d",
             CONFIG_UDP_DEST_PORT, errno);
  } else {
    ESP_LOGI(TAG, "UDP socket bound successfully to port %d",
             CONFIG_UDP_DEST_PORT);
  }

  // Set send and receive timeouts
  struct timeval snd_to = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

  struct timeval rcv_to = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));

  return sock;
}

static void bsp_udp_rx_task(void *pvParameters) {
  (void)pvParameters;
  char rx_buffer[512];
  struct sockaddr_in source_addr;
  socklen_t socklen = sizeof(source_addr);

  ESP_LOGI(TAG, "UDP background listener active on port %d",
           CONFIG_UDP_DEST_PORT);

  while (1) {
    if (s_udp_sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    int len = recvfrom(s_udp_sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                       (struct sockaddr *)&source_addr, &socklen);
    if (len > 0) {
      rx_buffer[len] = '\0';
      char sender_ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &source_addr.sin_addr, sender_ip, sizeof(sender_ip));
      uint16_t sender_port = ntohs(source_addr.sin_port);

      ESP_LOGI(TAG, "Received UDP packet from %s:%u (%d bytes):",
               sender_ip, sender_port, len);
      ESP_LOG_BUFFER_HEX_LEVEL(TAG, rx_buffer, len, ESP_LOG_INFO);

      if (xSemaphoreTake(s_udp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_active_client_addr = source_addr;
        s_has_active_client = true;
        xSemaphoreGive(s_udp_mutex);
        ESP_LOGI(TAG, "Saved %s:%u as primary UDP client destination",
                 sender_ip, sender_port);
      }

      // Forward incoming UDP payload to external UART device
      if (bsp_uart_send(rx_buffer, len) < 0) {
        ESP_LOGE(TAG, "uart send to device error");
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

static void bsp_tcp_server_task(void *pvParameters) {
  (void)pvParameters;
  s_tcp_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (s_tcp_listen_sock < 0) {
    ESP_LOGE(TAG, "Unable to create TCP socket: errno %d", errno);
    vTaskDelete(NULL);
    return;
  }

  int opt = 1;
  setsockopt(s_tcp_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in server_addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
      .sin_port = htons(CONFIG_UDP_DEST_PORT),
  };

  if (bind(s_tcp_listen_sock, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "TCP socket bind failed: errno %d", errno);
    close(s_tcp_listen_sock);
    s_tcp_listen_sock = -1;
    vTaskDelete(NULL);
    return;
  }

  if (listen(s_tcp_listen_sock, 1) < 0) {
    ESP_LOGE(TAG, "TCP listen failed: errno %d", errno);
    close(s_tcp_listen_sock);
    s_tcp_listen_sock = -1;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG,
           "TCP server running on port %d (ready for Tera Term connection at "
           "192.168.4.1:%d)",
           CONFIG_UDP_DEST_PORT, CONFIG_UDP_DEST_PORT);

  while (1) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_sock =
        accept(s_tcp_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
    if (client_sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    ESP_LOGI(TAG, "TCP connected from %s:%d", client_ip,
             ntohs(client_addr.sin_port));

    if (s_tcp_client_sock >= 0) {
      close(s_tcp_client_sock);
    }
    s_tcp_client_sock = client_sock;

    // Receive loop: forward any typed characters from Tera Term to UART
    char rx_buf[128];
    while (1) {
      int len = recv(client_sock, rx_buf, sizeof(rx_buf), 0);
      if (len <= 0) {
        ESP_LOGI(TAG, "Tera Term disconnected");
        break;
      }
      bsp_uart_send(rx_buf, len);
    }

    close(client_sock);
    if (s_tcp_client_sock == client_sock) {
      s_tcp_client_sock = -1;
    }
  }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    s_connected_stations++;
    ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d (total connected: %d)",
             MAC2STR(event->mac), event->aid, s_connected_stations);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    if (s_connected_stations > 0) {
      s_connected_stations--;
    }
    ESP_LOGI(
        TAG, "Station " MACSTR " left, AID=%d, reason=%d (total connected: %d)",
        MAC2STR(event->mac), event->aid, event->reason, s_connected_stations);
  }
}

esp_err_t bsp_set_udp_dest(const char *ip_str, uint16_t port) {
  if (ip_str == NULL || port == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (s_udp_mutex &&
      xSemaphoreTake(s_udp_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    s_dest_addr.sin_family = AF_INET;
    s_dest_addr.sin_port = htons(port);
    int ret = inet_pton(AF_INET, ip_str, &s_dest_addr.sin_addr.s_addr);
    xSemaphoreGive(s_udp_mutex);

    if (ret != 1) {
      ESP_LOGE(TAG, "Invalid destination IP address: %s", ip_str);
      return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "UDP destination updated to %s:%u", ip_str, port);
    return ESP_OK;
  }

  return ESP_ERR_TIMEOUT;
}

esp_err_t bsp_send_udp(const void *data, size_t len) {
  if (data == NULL || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!s_udp_mutex) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(s_udp_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to acquire UDP mutex (timeout)");
    return ESP_ERR_TIMEOUT;
  }

  if (s_udp_sock < 0) {
    s_udp_sock = create_and_bind_udp_socket();
    if (s_udp_sock < 0) {
      xSemaphoreGive(s_udp_mutex);
      return ESP_FAIL;
    }
  }

  // Select ONE target destination to prevent duplicate packets on the host
  struct sockaddr_in *target_addr;
  if (s_has_active_client) {
    // Direct unicast to client that contacted us
    target_addr = &s_active_client_addr;
  } else if (s_connected_stations > 0) {
    // Direct unicast to connected Windows PC (192.168.4.2:3333)
    target_addr = &s_sta_unicast_addr;
  } else {
    // Subnet broadcast (192.168.4.255:3333)
    target_addr = &s_subnet_bcast_addr;
  }

  ssize_t sent = sendto(s_udp_sock, data, len, 0,
                        (struct sockaddr *)target_addr, sizeof(*target_addr));
  if (sent < 0) {
    ESP_LOGW(TAG,
             "sendto target failed: errno %d (%s), trying subnet broadcast...",
             errno, strerror(errno));
    sent = sendto(s_udp_sock, data, len, 0,
                  (struct sockaddr *)&s_subnet_bcast_addr,
                  sizeof(s_subnet_bcast_addr));
  }

  if (sent >= 0) {
    char target_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &target_addr->sin_addr, target_ip, sizeof(target_ip));
    ESP_LOGD(TAG, "UDP TX: %zu bytes -> %s:%d", len, target_ip,
             ntohs(target_addr->sin_port));
  } else {
    ESP_LOGE(TAG, "UDP sendto failed completely: errno %d (%s)", errno,
             strerror(errno));
    close(s_udp_sock);
    s_udp_sock = -1;
    xSemaphoreGive(s_udp_mutex);
    return ESP_FAIL;
  }

  // If Tera Term (TCP) is connected, send to it as well (non-blocking)
  if (s_tcp_client_sock >= 0) {
    ssize_t tcp_sent = send(s_tcp_client_sock, data, len, MSG_DONTWAIT);
    if (tcp_sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      ESP_LOGW(TAG, "TCP send failed: errno %d", errno);
      close(s_tcp_client_sock);
      s_tcp_client_sock = -1;
    }
  }

  xSemaphoreGive(s_udp_mutex);
  return ESP_OK;
}

esp_err_t bsp_send_udp_str(const char *str) {
  if (str == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  return bsp_send_udp((const void *)str, strlen(str));
}

esp_err_t bsp_init_softap(void) {
  if (s_udp_mutex == NULL) {
    s_udp_mutex = xSemaphoreCreateMutex();
    if (s_udp_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create UDP mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  // Default target address (192.168.4.2:3333)
  memset(&s_dest_addr, 0, sizeof(s_dest_addr));
  s_dest_addr.sin_family = AF_INET;
  s_dest_addr.sin_port = htons(CONFIG_UDP_DEST_PORT);
  inet_pton(AF_INET, CONFIG_UDP_DEST_IP, &s_dest_addr.sin_addr.s_addr);

  // Pre-configure subnet broadcast address (192.168.4.255:3333)
  memset(&s_subnet_bcast_addr, 0, sizeof(s_subnet_bcast_addr));
  s_subnet_bcast_addr.sin_family = AF_INET;
  s_subnet_bcast_addr.sin_port = htons(CONFIG_UDP_DEST_PORT);
  inet_pton(AF_INET, "192.168.4.255", &s_subnet_bcast_addr.sin_addr.s_addr);

  // Pre-configure station default unicast address (192.168.4.2:3333)
  memset(&s_sta_unicast_addr, 0, sizeof(s_sta_unicast_addr));
  s_sta_unicast_addr.sin_family = AF_INET;
  s_sta_unicast_addr.sin_port = htons(CONFIG_UDP_DEST_PORT);
  inet_pton(AF_INET, "192.168.4.2", &s_sta_unicast_addr.sin_addr.s_addr);

  esp_err_t ret = esp_netif_init();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s",
             esp_err_to_name(ret));
    return ret;
  }

  s_ap_netif = esp_netif_create_default_wifi_ap();
  if (s_ap_netif == NULL) {
    ESP_LOGW(TAG, "esp_netif_create_default_wifi_ap returned NULL (may already "
                  "be created)");
  } else {
    // Set SoftAP as default netif
    esp_netif_set_default_netif(s_ap_netif);
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ret = esp_wifi_init(&cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &wifi_event_handler, NULL, NULL);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_event_handler_instance_register failed: %s",
             esp_err_to_name(ret));
    return ret;
  }

  wifi_config_t wifi_config = {
      .ap =
          {
              .ssid = CONFIG_ESP_WIFI_SSID,
              .ssid_len = strlen(CONFIG_ESP_WIFI_SSID),
              .channel =
                  (CONFIG_ESP_WIFI_CHANNEL > 14) ? 0 : CONFIG_ESP_WIFI_CHANNEL,
              .password = CONFIG_ESP_WIFI_PASSWORD,
              .max_connection = CONFIG_ESP_MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
              .authmode = WIFI_AUTH_WPA3_PSK,
              .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else
              .authmode = WIFI_AUTH_WPA2_PSK,
#endif
              .pmf_cfg =
                  {
                      .required = true,
                  },
#ifdef CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT
              .bss_max_idle_cfg =
                  {
                      .period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD,
                      .protected_keep_alive = 1,
                  },
#endif
              .gtk_rekey_interval = GTK_REKEY_INTERVAL,
          },
  };

  if (strlen(CONFIG_ESP_WIFI_PASSWORD) == 0) {
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ret = esp_wifi_set_mode(WIFI_MODE_AP);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = esp_wifi_start();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
    return ret;
  }

#if CONFIG_SOC_WIFI_SUPPORT_5G
#if CONFIG_ESP_WIFI_BAND_5G
  ESP_LOGI(TAG, "Configuring Wi-Fi band to 5 GHz Only...");
  ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY));
  ESP_ERROR_CHECK(
      esp_wifi_set_channel(CONFIG_ESP_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
  ESP_LOGI(TAG, "Wi-Fi Band set to 5 GHz Only (Channel %d)",
           CONFIG_ESP_WIFI_CHANNEL);
#elif CONFIG_ESP_WIFI_BAND_AUTO
  ESP_LOGI(TAG, "Configuring Wi-Fi band to Dual-Band Auto (2.4G + 5G)...");
  ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
  ESP_ERROR_CHECK(
      esp_wifi_set_channel(CONFIG_ESP_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
#else
  ESP_LOGI(TAG, "Configuring Wi-Fi band to 2.4 GHz Only (Channel %d)...",
           CONFIG_ESP_WIFI_CHANNEL);
  ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY));
  ESP_ERROR_CHECK(
      esp_wifi_set_channel(CONFIG_ESP_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
#endif
#endif

  // Bind and initialize UDP socket
  s_udp_sock = create_and_bind_udp_socket();

  // Start background UDP receive task
  xTaskCreate(bsp_udp_rx_task, "bsp_udp_rx", 4096, NULL, 5,
              &s_udp_rx_task_handle);

  // Start background TCP server task for Tera Term
  xTaskCreate(bsp_tcp_server_task, "bsp_tcp_srv", 4096, NULL, 5,
              &s_tcp_server_task_handle);

  ESP_LOGI(TAG,
           "SoftAP initialized successfully. SSID: %s, Channel: %d, Port: %d "
           "(UDP & TCP)",
           CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_CHANNEL, CONFIG_UDP_DEST_PORT);

  return ESP_OK;
}
