#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* ------------------------------------------------------------------------- *
 * Weather location
 *
 * The panel shows the current weather for one fixed place.  Change the macros
 * below to move the calendar to another town; everything else stays the same.
 * Latitude/longitude are decimal degrees (south and west are negative) and are
 * the only values the weather service needs.
 *
 * WEATHER_PLACE_NAME is drawn on the panel with the built-in 20x20 Chinese
 * font subset, so every character in it must exist in main/chinese_font.h.
 * The right-hand panel fits at most five characters, and the firmware skips
 * the name (instead of printing blanks) when a glyph is missing or the text is
 * too wide.  See tools/glyphgen.py for adding characters.
 * ------------------------------------------------------------------------- */
#define WEATHER_PLACE_NAME "温岭大溪"
#define WEATHER_LATITUDE   "28.4833" /* 28°29'N - Daxi Town, Wenling, Zhejiang */
#define WEATHER_LONGITUDE  "121.3500" /* 121°21'E */
#define WEATHER_TIMEZONE   "Asia/Shanghai"

/* Open-Meteo needs no API key.  WMO weather codes are mapped to Chinese text
 * in weather.c, so replacing the endpoint means adjusting that table too. */
#define WEATHER_API_HOST "api.open-meteo.com"
#define WEATHER_API_PATH "/v1/forecast?latitude=" WEATHER_LATITUDE \
                         "&longitude=" WEATHER_LONGITUDE \
                         "&current=temperature_2m,weather_code,wind_speed_10m" \
                         "&forecast_days=1&timezone=" WEATHER_TIMEZONE
#define WEATHER_HTTP_TIMEOUT_MS 8000
#define WEATHER_RESPONSE_MAX 1536

/* Filled in by weather_fetch(). */
typedef struct {
    bool valid;          /* true once temperature_c and code are meaningful */
    int temperature_c;   /* rounded current temperature */
    int weather_code;    /* WMO weather code from the service */
    int wind_kmh;        /* rounded wind speed */
    bool has_wind;
} weather_info_t;

/* Fetch the current weather.  Returns ESP_OK only when a temperature and a
 * weather code were parsed; on failure `info` is left invalid and the caller
 * should draw the calendar without weather.  Requires an active network. */
esp_err_t weather_fetch(weather_info_t *info);

/* Human readable, panel-safe text for a WMO weather code (never NULL). */
const char *weather_code_text(int code);
