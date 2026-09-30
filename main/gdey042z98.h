#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gdey042z98_init(void);
/* Frame planes are packed MSB-first, 1 bit per pixel, 400x300 (15000 bytes
 * each). In black_plane, 1 means white and 0 means black. In red_plane, 1
 * selects red and 0 means non-red. Valid combinations are (red, BW):
 * (0,1)=white, (0,0)=black, (1,0)=red. */
esp_err_t gdey042z98_display(const uint8_t *black_plane, const uint8_t *red_plane);
esp_err_t gdey042z98_show_test_pattern(void);
esp_err_t gdey042z98_show_name(void);

#ifdef __cplusplus
}
#endif
