#include "weather.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "weather_parse.h"

static const char *TAG = "weather";

const char *weather_code_text(int code)
{
    /* WMO weather interpretation codes used by Open-Meteo. */
    switch (code) {
    case 0: return "晴";
    case 1: return "晴间多云";
    case 2: return "多云";
    case 3: return "阴";
    case 45:
    case 48: return "雾";
    case 51:
    case 53:
    case 55: return "毛毛雨";
    case 56:
    case 57: return "冻雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66:
    case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "阵雪";
    case 80:
    case 81: return "阵雨";
    case 82: return "暴雨";
    case 85:
    case 86: return "阵雪";
    case 95: return "雷阵雨";
    case 96:
    case 99: return "雷暴";
    default: return "未知";
    }
}

static esp_err_t weather_http_get(char *response, size_t response_size)
{
    const esp_http_client_config_t config = {
        .host = WEATHER_API_HOST,
        .path = WEATHER_API_PATH,
        .method = HTTP_METHOD_GET,
        .timeout_ms = WEATHER_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open to %s failed: %s", WEATHER_API_HOST, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) content_length = 0;
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "Weather service returned HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t total = 0;
    while (total + 1 < response_size) {
        const int wanted = content_length > 0
                               ? content_length - (int)total
                               : (int)(response_size - 1 - total);
        if (wanted <= 0) break;
        const int read = esp_http_client_read(client, response + total, wanted);
        if (read < 0) {
            ESP_LOGW(TAG, "HTTP read failed");
            err = ESP_FAIL;
            break;
        }
        if (read == 0) break;
        total += (size_t)read;
    }
    response[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (total == 0) {
        ESP_LOGW(TAG, "Weather service returned an empty body");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t weather_fetch(weather_info_t *info)
{
    if (info == NULL) return ESP_ERR_INVALID_ARG;
    *info = (weather_info_t){0};

    static char response[WEATHER_RESPONSE_MAX];
    const esp_err_t err = weather_http_get(response, sizeof(response));
    if (err != ESP_OK) return err;

    weather_parse_result_t parsed;
    if (!weather_extract(response, &parsed)) {
        ESP_LOGW(TAG, "Weather response did not contain a temperature and weather code");
        return ESP_ERR_NOT_FOUND;
    }

    info->temperature_c = parsed.temperature_c;
    info->weather_code = parsed.weather_code;
    info->wind_kmh = parsed.wind_kmh;
    info->has_wind = parsed.has_wind;
    info->valid = true;

    ESP_LOGI(TAG, "Weather for %s: %d C, code %d, wind %d km/h",
             WEATHER_PLACE_NAME, info->temperature_c, info->weather_code,
             info->has_wind ? info->wind_kmh : -1);
    return ESP_OK;
}
