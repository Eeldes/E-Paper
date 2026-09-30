#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "calendar_ui.h"
#include "gdey042z98.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define WIFI_SSID "eiiman"
#define WIFI_PASSWORD "12345678"
#define HTTP_PORT 80
#define BATTERY_GPIO 7
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_6 /* ESP32-S3 GPIO7 = ADC1_CH6 */

/* The battery input is assumed to be a 1-cell Li-ion voltage behind a 1:2
 * divider. Change this to the actual board divider ratio for meaningful %. */
#define BATTERY_DIVIDER_RATIO 2.0f
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200

static const char *TAG = "calendar_app";
static const EventBits_t WIFI_HAS_IP = BIT0;
static EventGroupHandle_t s_wifi_events;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_calibrated;
static bool s_sntp_started;
static int s_battery_percent = -1;
static char s_ip_address[16] = "0.0.0.0";
static portMUX_TYPE s_network_info_lock = portMUX_INITIALIZER_UNLOCKED;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_HAS_IP);
        portENTER_CRITICAL(&s_network_info_lock);
        strlcpy(s_ip_address, "0.0.0.0", sizeof(s_ip_address));
        portEXIT_CRITICAL(&s_network_info_lock);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected to %s, IP=" IPSTR, WIFI_SSID, IP2STR(&event->ip_info.ip));
        char ip_text[16];
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&event->ip_info.ip));
        portENTER_CRITICAL(&s_network_info_lock);
        strlcpy(s_ip_address, ip_text, sizeof(s_ip_address));
        portEXIT_CRITICAL(&s_network_info_lock);
        xEventGroupSetBits(s_wifi_events, WIFI_HAS_IP);
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *station_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(station_netif ? ESP_OK : ESP_ERR_NO_MEM);

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void sntp_start(void)
{
    if (s_sntp_started) return;
    const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com)");
    } else {
        ESP_LOGE(TAG, "SNTP initialization failed: %s", esp_err_to_name(err));
    }
}

static void battery_adc_init(void)
{
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &s_adc));
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHANNEL, &channel_config));

    adc_cali_scheme_ver_t scheme_mask = 0;
    if (adc_cali_check_scheme(&scheme_mask) == ESP_OK) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (scheme_mask & ADC_CALI_SCHEME_VER_CURVE_FITTING) {
            const adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .chan = BATTERY_ADC_CHANNEL,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            s_adc_calibrated = adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali) == ESP_OK;
        }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        if (!s_adc_calibrated && (scheme_mask & ADC_CALI_SCHEME_VER_LINE_FITTING)) {
            const adc_cali_line_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            s_adc_calibrated = adc_cali_create_scheme_line_fitting(&cali_config, &s_adc_cali) == ESP_OK;
        }
#endif
    }
    ESP_LOGI(TAG, "Battery ADC initialized on GPIO%d (calibrated=%s)", BATTERY_GPIO,
             s_adc_calibrated ? "yes" : "no");
}

static int battery_read_percent(void)
{
    int raw_total = 0;
    for (int i = 0; i < 16; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) return -1;
        raw_total += raw;
    }
    const int raw_average = raw_total / 16;
    int pin_mv = 0;
    if (!s_adc_calibrated || adc_cali_raw_to_voltage(s_adc_cali, raw_average, &pin_mv) != ESP_OK) {
        pin_mv = raw_average * 3100 / 4095; /* Approximate ESP32-S3 12 dB full scale. */
    }
    const int cell_mv = (int)(pin_mv * BATTERY_DIVIDER_RATIO + 0.5f);
    if (cell_mv <= BATTERY_EMPTY_MV) return 0;
    if (cell_mv >= BATTERY_FULL_MV) return 100;
    return (cell_mv - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
}

static bool get_local_time(struct tm *local_time)
{
    const time_t now = time(NULL);
    if (now < 1704067200) return false; /* Ignore unset/epoch time before NTP succeeds. */
    return localtime_r(&now, local_time) != NULL;
}

static bool send_all(int socket_fd, const char *data, size_t length)
{
    while (length > 0) {
        const int sent = send(socket_fd, data, length, 0);
        if (sent <= 0) return false;
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

static void http_reply(int socket_fd, int status, const char *content_type, const char *body)
{
    char header[192];
    const char *reason = status == 200 ? "OK" : (status == 404 ? "Not Found" : "Service Unavailable");
    const int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
        status, reason, content_type, (unsigned)strlen(body));
    if (header_len > 0 && (size_t)header_len < sizeof(header)) {
        send_all(socket_fd, header, (size_t)header_len);
        send_all(socket_fd, body, strlen(body));
    }
}

static void handle_http_client(int socket_fd)
{
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char request[768] = {0};
    size_t used = 0;
    while (used < sizeof(request) - 1) {
        const int received = recv(socket_fd, request + used, sizeof(request) - 1 - used, 0);
        if (received <= 0) break;
        used += (size_t)received;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL) break;
    }

    if (strncmp(request, "GET /status ", 12) == 0) {
        struct tm now_tm;
        char time_text[32] = "unsynchronized";
        char ip_text[16];
        portENTER_CRITICAL(&s_network_info_lock);
        strlcpy(ip_text, s_ip_address, sizeof(ip_text));
        portEXIT_CRITICAL(&s_network_info_lock);
        if (get_local_time(&now_tm)) strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S", &now_tm);
        char body[320];
        snprintf(body, sizeof(body),
                 "{\"ssid\":\"%s\",\"wifi\":%s,\"ip\":\"%s\",\"time\":\"%s\",\"battery_percent\":%d,\"adc_gpio\":%d}\n",
                 WIFI_SSID, (xEventGroupGetBits(s_wifi_events) & WIFI_HAS_IP) ? "true" : "false",
                 ip_text, time_text, s_battery_percent, BATTERY_GPIO);
        http_reply(socket_fd, 200, "application/json; charset=utf-8", body);
    } else if (strncmp(request, "GET / ", 6) == 0 || strncmp(request, "GET /index.html ", 16) == 0) {
        static const char page[] =
            "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width'>"
            "<title>ESP32 Calendar</title><style>body{font:18px system-ui;max-width:620px;margin:3em auto;padding:0 1em;color:#222}"
            "h1{color:#b22}pre{background:#f3f3f3;padding:1em;border-radius:8px}</style></head>"
            "<body><h1>E-Paper Calendar</h1><p>ESP32-S3 display backend</p><pre id=s>Loading...</pre>"
            "<script>const s=document.getElementById('s');async function u(){try{let r=await fetch('/status'),j=await r.json();"
            "s.textContent=JSON.stringify(j,null,2)}catch(e){s.textContent=e}}u();setInterval(u,10000)</script></body></html>";
        http_reply(socket_fd, 200, "text/html; charset=utf-8", page);
    } else {
        http_reply(socket_fd, 404, "text/plain; charset=utf-8", "Try / or /status\n");
    }
}

static void tcp_http_server_task(void *arg)
{
    (void)arg;
    const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "TCP socket create failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    const int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(HTTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_fd, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(listen_fd, 4) != 0) {
        ESP_LOGE(TAG, "TCP server bind/listen failed: errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "HTTP-over-TCP backend listening on port %d (/ and /status)", HTTP_PORT);
    while (true) {
        const int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) continue;
        handle_http_client(client_fd);
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Wi-Fi calendar and GDEY042Z98 display");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(gdey042z98_init());
    battery_adc_init();
    s_battery_percent = battery_read_percent();

    setenv("TZ", "CST-8", 1);
    tzset();
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_events ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_start();
    xTaskCreate(tcp_http_server_task, "calendar_http", 6144, NULL, 4, NULL);

    if (xEventGroupWaitBits(s_wifi_events, WIFI_HAS_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000)) & WIFI_HAS_IP) {
        sntp_start();
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) == ESP_OK) {
            ESP_LOGI(TAG, "Network time synchronized");
        } else {
            ESP_LOGW(TAG, "Time sync is pending; will keep waiting while Wi-Fi retries");
        }
    } else {
        ESP_LOGW(TAG, "No Wi-Fi IP yet; server is ready and station will keep reconnecting");
    }

    bool calendar_drawn = false;
    int drawn_day = -1;
    int drawn_month = -1;
    time_t last_draw = 0;
    while (true) {
        if ((xEventGroupGetBits(s_wifi_events) & WIFI_HAS_IP) && !s_sntp_started) {
            sntp_start();
        }
        struct tm now_tm;
        if (get_local_time(&now_tm)) {
            const time_t now = time(NULL);
            const bool date_changed = now_tm.tm_mday != drawn_day || now_tm.tm_mon != drawn_month;
            const bool hourly_refresh = calendar_drawn && now - last_draw >= 3600;
            if (!calendar_drawn || date_changed || hourly_refresh) {
                char ip_text[16];
                portENTER_CRITICAL(&s_network_info_lock);
                strlcpy(ip_text, s_ip_address, sizeof(ip_text));
                portEXIT_CRITICAL(&s_network_info_lock);
                const esp_err_t err = calendar_ui_render(&now_tm, s_battery_percent, ip_text,
                    (xEventGroupGetBits(s_wifi_events) & WIFI_HAS_IP) != 0);
                if (err == ESP_OK) {
                    calendar_drawn = true;
                    drawn_day = now_tm.tm_mday;
                    drawn_month = now_tm.tm_mon;
                    last_draw = now;
                } else {
                    ESP_LOGE(TAG, "Calendar refresh failed: %s", esp_err_to_name(err));
                }
            }
        }
        s_battery_percent = battery_read_percent();
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
