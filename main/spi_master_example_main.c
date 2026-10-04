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
#include "esp_http_server.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BATTERY_GPIO 7
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_6 /* ESP32-S3 GPIO7 = ADC1_CH6 */

/* Change these settings to adjust the deep-sleep schedule and wake button. */
#define DEEP_SLEEP_INTERVAL_HOURS 12
#define WAKE_BUTTON_GPIO GPIO_NUM_4 /* Active-low button from GPIO4 to GND; RTC-capable on ESP32-S3. */
#define WAKE_BUTTON_LONG_PRESS_MS 3000
#define WIFI_CONNECT_TIMEOUT_SECONDS 30
#define SNTP_SYNC_TIMEOUT_SECONDS 20
#define WIFI_RETRY_SLEEP_MINUTES 30
#define SETUP_PORTAL_TIMEOUT_MINUTES 5

/* Used only before the first credentials have been saved to NVS. */
#define DEFAULT_WIFI_SSID "eiiman"
#define DEFAULT_WIFI_PASSWORD "12345678"
#define SETUP_AP_SSID "E-Paper-Setup"
#define SETUP_AP_PASSWORD "epaper123"
#define WIFI_NVS_NAMESPACE "wifi_cfg"

/* The battery input is assumed to be a 1-cell Li-ion voltage behind a 1:2
 * divider. Change this to the actual board divider ratio for meaningful %. */
#define BATTERY_DIVIDER_RATIO 2.0f
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200

static const char *TAG = "calendar_app";
static const EventBits_t WIFI_HAS_IP = BIT0;
static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_http_server;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_calibrated;
static bool s_sntp_started;
static bool s_wifi_started;
static bool s_setup_mode;
static char s_current_ssid[33];
static int s_battery_percent = -1;

typedef struct {
    char ssid[33];
    char password[65];
} wifi_credentials_t;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_HAS_IP);
        if (!s_setup_mode) {
            ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected to %s, IP=" IPSTR, s_current_ssid, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_HAS_IP);
    }
}

static void wifi_stack_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK((s_sta_netif && s_ap_netif) ? ESP_OK : ESP_ERR_NO_MEM);

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
}

static bool nvs_read_credentials(const char *ssid_key, const char *password_key,
                                 wifi_credentials_t *credentials)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t ssid_size = sizeof(credentials->ssid);
    size_t password_size = sizeof(credentials->password);
    const esp_err_t ssid_err = nvs_get_str(handle, ssid_key, credentials->ssid, &ssid_size);
    const esp_err_t password_err = nvs_get_str(handle, password_key, credentials->password, &password_size);
    nvs_close(handle);

    return ssid_err == ESP_OK && password_err == ESP_OK && credentials->ssid[0] != '\0';
}

static bool nvs_write_credentials(const char *ssid_key, const char *password_key,
                                  const wifi_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    err = nvs_set_str(handle, ssid_key, credentials->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, password_key, credentials->password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool nvs_save_pending_credentials(const wifi_credentials_t *credentials)
{
    return nvs_write_credentials("pending_ssid", "pending_pass", credentials);
}

static bool nvs_promote_pending_credentials(const wifi_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    err = nvs_set_str(handle, "ssid", credentials->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "pass", credentials->password);
    if (err == ESP_OK) {
        (void)nvs_erase_key(handle, "pending_ssid");
        (void)nvs_erase_key(handle, "pending_pass");
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

static void nvs_clear_pending_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_erase_key(handle, "pending_ssid");
    (void)nvs_erase_key(handle, "pending_pass");
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static bool button_long_pressed(void)
{
    ESP_ERROR_CHECK(rtc_gpio_init(WAKE_BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_set_direction(WAKE_BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(WAKE_BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(WAKE_BUTTON_GPIO));
    if (rtc_gpio_get_level(WAKE_BUTTON_GPIO) != 0) return false;

    const int64_t started_us = esp_timer_get_time();
    const int64_t threshold_us = (int64_t)WAKE_BUTTON_LONG_PRESS_MS * 1000;
    while (rtc_gpio_get_level(WAKE_BUTTON_GPIO) == 0 && esp_timer_get_time() - started_us < threshold_us) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return esp_timer_get_time() - started_us >= threshold_us;
}

static bool url_decode(char *destination, size_t destination_size, const char *source)
{
    size_t output = 0;
    while (*source != '\0') {
        char value = *source++;
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (source[0] == '\0' || source[1] == '\0') return false;
            char hex[3] = {source[0], source[1], '\0'};
            char *end = NULL;
            const long decoded = strtol(hex, &end, 16);
            if (end != hex + 2 || decoded < 0 || decoded > 255) return false;
            value = (char)decoded;
            source += 2;
        }
        if (output + 1 >= destination_size) return false;
        destination[output++] = value;
    }
    destination[output] = '\0';
    return true;
}

static bool form_get_value(char *body, const char *wanted_key, char *destination, size_t destination_size)
{
    char *save_pointer = NULL;
    for (char *field = strtok_r(body, "&", &save_pointer); field != NULL;
         field = strtok_r(NULL, "&", &save_pointer)) {
        char *equals = strchr(field, '=');
        if (equals == NULL) continue;
        *equals = '\0';
        if (strcmp(field, wanted_key) == 0) {
            return url_decode(destination, destination_size, equals + 1);
        }
    }
    return false;
}

static esp_err_t setup_page_handler(httpd_req_t *request)
{
    static const char page[] =
        "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>墨水屏 Wi-Fi 配置</title><body style='font:18px sans-serif;max-width:480px;margin:3em auto;padding:0 1em'>"
        "<h2>配置 Wi-Fi</h2><p>输入路由器 Wi-Fi 名称和密码。连接并校时成功后设备会保存配置、刷新日历并休眠。</p>"
        "<form method='post' action='/save'><label>Wi-Fi 名称<br><input name='ssid' maxlength='32' required style='width:100%;padding:10px'></label><br><br>"
        "<label>Wi-Fi 密码（开放网络可留空）<br><input name='password' type='password' maxlength='64' style='width:100%;padding:10px'></label><br><br>"
        "<button style='padding:12px 24px'>保存并连接</button></form></body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t setup_save_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > 512) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form data");
    }

    char body[513] = {0};
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Could not read form");
        received += (size_t)count;
    }
    body[received] = '\0';

    wifi_credentials_t credentials = {0};
    char ssid_body[513];
    char password_body[513];
    memcpy(ssid_body, body, received + 1);
    memcpy(password_body, body, received + 1);
    const bool has_ssid = form_get_value(ssid_body, "ssid", credentials.ssid, sizeof(credentials.ssid));
    const bool has_password = form_get_value(password_body, "password", credentials.password,
                                             sizeof(credentials.password));
    if (!has_ssid || !has_password || credentials.ssid[0] == '\0') {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "SSID or password is invalid");
    }
    if (!nvs_save_pending_credentials(&credentials)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save credentials");
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_sendstr(request,
        "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
        "<h2>配置已接收</h2><p>设备正在重启并验证网络与时间，请稍候。</p>");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static void setup_portal_start(void)
{
    s_setup_mode = true;
    xEventGroupClearBits(s_wifi_events, WIFI_HAS_IP);
    if (s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_stop());
        s_wifi_started = false;
    }

    wifi_config_t config = {0};
    strlcpy((char *)config.ap.ssid, SETUP_AP_SSID, sizeof(config.ap.ssid));
    strlcpy((char *)config.ap.password, SETUP_AP_PASSWORD, sizeof(config.ap.password));
    config.ap.ssid_len = strlen(SETUP_AP_SSID);
    config.ap.channel = 1;
    config.ap.max_connection = 2;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.max_uri_handlers = 4;
    server_config.stack_size = 6144;
    ESP_ERROR_CHECK(httpd_start(&s_http_server, &server_config));
    const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = setup_page_handler};
    const httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = setup_save_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &save_uri));
    ESP_LOGI(TAG, "Wi-Fi setup portal: SSID=%s password=%s URL=http://192.168.4.1/",
             SETUP_AP_SSID, SETUP_AP_PASSWORD);
}

static void setup_portal_wait_or_sleep(void)
{
    const int64_t timeout_us = (int64_t)SETUP_PORTAL_TIMEOUT_MINUTES * 60 * 1000000;
    const int64_t started_us = esp_timer_get_time();
    while (esp_timer_get_time() - started_us < timeout_us) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "Wi-Fi setup timed out; sleeping to save power");
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
}

static bool wifi_start_station(const wifi_credentials_t *credentials)
{
    s_setup_mode = false;
    strlcpy(s_current_ssid, credentials->ssid, sizeof(s_current_ssid));
    xEventGroupClearBits(s_wifi_events, WIFI_HAS_IP);

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, credentials->ssid, strnlen(credentials->ssid, sizeof(config.sta.ssid)));
    memcpy(config.sta.password, credentials->password,
           strnlen(credentials->password, sizeof(config.sta.password)));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    const esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGE(TAG, "Could not start Wi-Fi station: %s", esp_err_to_name(start_err));
        return false;
    }
    s_wifi_started = true;

    const EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_HAS_IP, pdFALSE, pdTRUE,
                                                  pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_SECONDS * 1000));
    return (bits & WIFI_HAS_IP) != 0;
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

static void enter_deep_sleep(uint32_t sleep_minutes, const char *reason)
{
    const uint64_t sleep_duration_us = (uint64_t)sleep_minutes * 60ULL * 1000000ULL;

    /* EXT0 uses an RTC GPIO and active-low level. Keep the internal pull-up on
     * so the input remains high while the optional button is not pressed. */
    ESP_ERROR_CHECK(rtc_gpio_init(WAKE_BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_set_direction(WAKE_BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(WAKE_BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(WAKE_BUTTON_GPIO));

    /* Avoid immediately waking again if the button is still held as sleep begins. */
    while (rtc_gpio_get_level(WAKE_BUTTON_GPIO) == 0) {
        ESP_LOGI(TAG, "Release wake button on GPIO%d to enter deep sleep", WAKE_BUTTON_GPIO);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(sleep_duration_us));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(WAKE_BUTTON_GPIO, 0));

    ESP_LOGI(TAG, "Entering deep sleep for %u minutes (%s); active-low wake button on GPIO%d",
             (unsigned)sleep_minutes, reason, WAKE_BUTTON_GPIO);
    /* Wi-Fi is not retained in deep sleep; stop it cleanly before sleeping. */
    if (s_wifi_started) ESP_ERROR_CHECK(esp_wifi_stop());
    esp_deep_sleep_start();
}

static bool get_local_time(struct tm *local_time)
{
    const time_t now = time(NULL);
    if (now < 1704067200) return false; /* Ignore unset/epoch time before NTP succeeds. */
    return localtime_r(&now, local_time) != NULL;
}

void app_main(void)
{
    const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Woke from 12-hour timer; refreshing network time");
    } else if (wake_cause == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "Woke from button on GPIO%d; refreshing network time", WAKE_BUTTON_GPIO);
    } else {
        ESP_LOGI(TAG, "Power-on/reset; will sync time, refresh display, then sleep");
    }
    ESP_ERROR_CHECK(nvs_flash_init());
    setenv("TZ", "CST-8", 1);
    tzset();
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_events ? ESP_OK : ESP_ERR_NO_MEM);

    const bool long_press = button_long_pressed();
    if (long_press) {
        ESP_LOGI(TAG, "Long press detected; opening Wi-Fi setup portal");
        nvs_clear_pending_credentials();
        wifi_stack_init();
        setup_portal_start();
        setup_portal_wait_or_sleep();
        enter_deep_sleep(WIFI_RETRY_SLEEP_MINUTES, "setup portal timeout");
    }

    wifi_credentials_t pending_credentials = {0};
    wifi_credentials_t saved_credentials = {0};
    const bool has_pending = nvs_read_credentials("pending_ssid", "pending_pass", &pending_credentials);
    const bool has_saved = nvs_read_credentials("ssid", "pass", &saved_credentials);

    wifi_credentials_t credentials = {0};
    if (has_pending) {
        credentials = pending_credentials;
        ESP_LOGI(TAG, "Testing newly submitted Wi-Fi credentials");
    } else if (has_saved) {
        credentials = saved_credentials;
        ESP_LOGI(TAG, "Using Wi-Fi credentials saved in NVS");
    } else {
        strlcpy(credentials.ssid, DEFAULT_WIFI_SSID, sizeof(credentials.ssid));
        strlcpy(credentials.password, DEFAULT_WIFI_PASSWORD, sizeof(credentials.password));
        ESP_LOGI(TAG, "No saved Wi-Fi credentials; trying the existing default network first");
    }

    wifi_stack_init();
    if (!wifi_start_station(&credentials)) {
        ESP_LOGW(TAG, "Could not connect to Wi-Fi within %d seconds", WIFI_CONNECT_TIMEOUT_SECONDS);
        if (has_pending) {
            nvs_clear_pending_credentials();
            ESP_LOGW(TAG, "New credentials failed; returning to setup portal");
            setup_portal_start();
            setup_portal_wait_or_sleep();
            enter_deep_sleep(WIFI_RETRY_SLEEP_MINUTES, "setup portal timeout");
        }
        if (!has_saved) {
            ESP_LOGW(TAG, "Default network unavailable; opening setup portal");
            setup_portal_start();
            setup_portal_wait_or_sleep();
            enter_deep_sleep(WIFI_RETRY_SLEEP_MINUTES, "setup portal timeout");
        }
        enter_deep_sleep(WIFI_RETRY_SLEEP_MINUTES, "Wi-Fi retry");
    }

    sntp_start();
    const esp_err_t sync_err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_SYNC_TIMEOUT_SECONDS * 1000));
    struct tm now_tm;
    if (sync_err != ESP_OK || !get_local_time(&now_tm)) {
        ESP_LOGW(TAG, "Network time sync failed (%s); will retry after sleep", esp_err_to_name(sync_err));
        if (has_pending) {
            nvs_clear_pending_credentials();
            ESP_LOGW(TAG, "New network did not provide time sync; returning to setup portal");
            setup_portal_start();
            setup_portal_wait_or_sleep();
            enter_deep_sleep(WIFI_RETRY_SLEEP_MINUTES, "setup portal timeout");
        }
        enter_deep_sleep(WIFI_RETRY_SLEEP_MINUTES, "SNTP retry");
    }

    if (has_pending) {
        if (!nvs_promote_pending_credentials(&pending_credentials)) {
            ESP_LOGE(TAG, "Could not save verified Wi-Fi credentials to NVS");
        } else {
            ESP_LOGI(TAG, "Verified Wi-Fi credentials saved");
        }
    } else if (!has_saved && !nvs_write_credentials("ssid", "pass", &credentials)) {
        ESP_LOGW(TAG, "Could not save default Wi-Fi credentials to NVS");
    }

    ESP_ERROR_CHECK(gdey042z98_init());
    battery_adc_init();
    s_battery_percent = battery_read_percent();
    ESP_LOGI(TAG, "Time synchronized; refreshing calendar");
    const esp_err_t render_err = calendar_ui_render(&now_tm, s_battery_percent);
    if (render_err != ESP_OK) {
        ESP_LOGE(TAG, "Calendar refresh failed: %s", esp_err_to_name(render_err));
        enter_deep_sleep(WIFI_RETRY_SLEEP_MINUTES, "display retry");
    }

    enter_deep_sleep(DEEP_SLEEP_INTERVAL_HOURS * 60, "regular refresh");
}
