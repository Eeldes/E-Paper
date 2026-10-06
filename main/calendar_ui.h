#pragma once

#include <time.h>

#include "esp_err.h"
#include "weather.h"

/* Render and push one frame: the month calendar on the left, today's lunar
 * date and the current weather on the right.  `weather` may be NULL or
 * invalid, in which case the weather section shows a placeholder. */
esp_err_t calendar_ui_render(const struct tm *local_time, int battery_percent,
                             const weather_info_t *weather);
