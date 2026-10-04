#pragma once

#include <time.h>

#include "esp_err.h"

esp_err_t calendar_ui_render(const struct tm *local_time, int battery_percent);
