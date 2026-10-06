#pragma once

#include <stdbool.h>

/* Parsing of the Open-Meteo JSON body, split out of weather.c so it can be
 * exercised on a host with tools/weather_parse_test.c.  No ESP-IDF types. */

typedef struct {
    int temperature_c;
    int weather_code;
    int wind_kmh;
    bool has_wind;
} weather_parse_result_t;

/* Parse a response body.  Returns false when the body does not carry a
 * temperature and a weather code. */
bool weather_extract(const char *json, weather_parse_result_t *out);
