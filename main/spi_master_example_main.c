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
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define WIFI_SSID "eiiman"
#define WIFI_PASSWORD "12345678"
#define BATTERY_GPIO 7
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_6 /* ESP32-S3 GPIO7 = ADC1_CH6 */

/* Change these settings to adjust the deep-sleep schedule and wake button. */
#define DEEP_SLEEP_INTERVAL_HOURS 12
#define WAKE_BUTTON_GPIO GPIO_NUM_4 /* Active-low button from GPIO4 to GND; RTC-capable on ESP32-S3. */

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

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_HAS_IP);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected to %s, IP=" IPSTR, WIFI_SSID, IP2STR(&event->ip_info.ip));
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

static void enter_deep_sleep(void)
{
    const uint64_t sleep_duration_us = (uint64_t)DEEP_SLEEP_INTERVAL_HOURS * 60ULL * 60ULL * 1000000ULL;

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

    ESP_LOGI(TAG, "Entering deep sleep for %d hours; active-low wake button on GPIO%d",
             DEEP_SLEEP_INTERVAL_HOURS, WAKE_BUTTON_GPIO);
    /* Wi-Fi is not retained in deep sleep; stop it cleanly before sleeping. */
    ESP_ERROR_CHECK(esp_wifi_stop());
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
    ESP_ERROR_CHECK(gdey042z98_init());
    battery_adc_init();
    s_battery_percent = battery_read_percent();

    setenv("TZ", "CST-8", 1);
    tzset();
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_events ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_start();

    ESP_LOGI(TAG, "Waiting for Wi-Fi and a successful SNTP sync before refreshing");
    while (true) {
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_HAS_IP)) {
            xEventGroupWaitBits(s_wifi_events, WIFI_HAS_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }
        if (!s_sntp_started) {
            sntp_start();
        }
        if (!s_sntp_started) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        const esp_err_t sync_err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000));
        if (sync_err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP not synchronized yet (%s); will retry", esp_err_to_name(sync_err));
            continue;
        }

        struct tm now_tm;
        if (!get_local_time(&now_tm)) {
            ESP_LOGW(TAG, "System time is not valid after SNTP; will retry");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Network time synchronized; refreshing calendar");
        s_battery_percent = battery_read_percent();
        const esp_err_t render_err = calendar_ui_render(&now_tm, s_battery_percent);
        if (render_err != ESP_OK) {
            ESP_LOGE(TAG, "Calendar refresh failed: %s; retrying", esp_err_to_name(render_err));
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "Calendar refreshed; entering deep sleep");
        enter_deep_sleep();
    }
}
