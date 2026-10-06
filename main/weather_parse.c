#include "weather_parse.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Find `"key":` in the JSON body and return the value text after it.  A `"`
 * directly after the key marks a sibling such as "temperature_2m_units", which
 * is skipped so the first data field wins. */
static const char *json_find(const char *json, const char *key)
{
    if (json == NULL || key == NULL) return NULL;
    char pattern[40];
    const int written = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (written <= 0 || written >= (int)sizeof(pattern)) return NULL;

    const size_t length = (size_t)written;
    for (const char *found = strstr(json, pattern); found != NULL; found = strstr(found + 1, pattern)) {
        if (found[length] != '"') return found + length;
    }
    return NULL;
}

static bool json_read_number(const char *json, const char *key, double *out)
{
    const char *value = json_find(json, key);
    if (value == NULL) return false;

    while (*value == ' ') ++value;
    if (*value == '\0' || *value == 'n' /* null */) return false;

    errno = 0;
    char *end = NULL;
    const double parsed = strtod(value, &end);
    if (end == value || errno == ERANGE || !isfinite(parsed)) return false;

    *out = parsed;
    return true;
}

/* Read the first key that is present.  The current Open-Meteo "current" block
 * uses snake_case names while the older "current_weather" block used the short
 * ones, so both spellings are accepted. */
static bool json_read_alias(const char *json, double *out, const char *primary, const char *alias)
{
    return json_read_number(json, primary, out) || json_read_number(json, alias, out);
}

bool weather_extract(const char *json, weather_parse_result_t *out)
{
    if (json == NULL || out == NULL) return false;
    *out = (weather_parse_result_t){0};

    double temperature = 0;
    double code = 0;
    if (!json_read_alias(json, &temperature, "temperature_2m", "temperature")) return false;
    if (!json_read_alias(json, &code, "weather_code", "weathercode")) return false;

    out->temperature_c = (int)lroundf((float)temperature);
    out->weather_code = (int)lroundf((float)code);

    double wind = 0;
    if (json_read_alias(json, &wind, "wind_speed_10m", "windspeed")) {
        out->wind_kmh = (int)lroundf((float)wind);
        out->has_wind = true;
    }
    return true;
}
