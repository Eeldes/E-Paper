#include "gdey042z98.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_EPD_BUSY_GPIO
#define CONFIG_EPD_BUSY_GPIO 1
#endif

/* Keep the project's confirmed ESP32-S3 wiring. The panel is operated in
 * 4-wire SPI mode (BS1 must be tied low on the panel side).
 * SDI is the panel's data-in line and is driven by the ESP32-S3 MOSI output. */
#define EPD_HOST       SPI2_HOST
#define EPD_PIN_SDI    38
#define EPD_PIN_SCLK   39
#define EPD_PIN_CS     40
#define EPD_PIN_DC     41
#define EPD_PIN_RESET  42
#define EPD_PIN_BUSY   CONFIG_EPD_BUSY_GPIO

#define EPD_WIDTH             400
#define EPD_HEIGHT            300
#define EPD_BYTES_PER_LINE    (EPD_WIDTH / 8)
#define EPD_FRAME_BYTES       (EPD_BYTES_PER_LINE * EPD_HEIGHT)
#define EPD_SPI_CLOCK_HZ      (10 * 1000 * 1000)

/* At 25 C the panel's specified full refresh is about 15-16 seconds. This
 * conservative delay is used only when BUSY polling is explicitly disabled. */
#define EPD_FULL_REFRESH_WAIT_MS 25000

static const char *TAG = "gdey042z98";
static spi_device_handle_t s_spi;
static bool s_sleeping;

/* Two 1-bit planes: 0x24 black/white and 0x26 red. These static buffers live
 * in internal RAM and are DMA-capable on ESP32-S3. */



static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t wait_ready(const char *operation, uint32_t timeout_ms, bool require_busy_assertion)
{
#if CONFIG_EPD_BUSY_GPIO >= 0
    const TickType_t started = xTaskGetTickCount();
    bool busy_seen = gpio_get_level(EPD_PIN_BUSY) != 0;
    while (true) {
        const int busy_level = gpio_get_level(EPD_PIN_BUSY);
        if (busy_level != 0) {
            busy_seen = true;
        } else if (!require_busy_assertion || busy_seen) {
            ESP_LOGI(TAG, "%s: BUSY released (asserted=%s)", operation, busy_seen ? "yes" : "no");
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - started) >= pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGE(TAG, "%s timed out on BUSY GPIO %d (level=%d, asserted=%s)",
                     operation, EPD_PIN_BUSY, busy_level, busy_seen ? "yes" : "no");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#else
    (void)require_busy_assertion;
    ESP_LOGW(TAG, "%s: BUSY is not connected; waiting %u ms", operation, (unsigned)timeout_ms);
    delay_ms(timeout_ms);
    return ESP_OK;
#endif
}

static esp_err_t spi_write(const uint8_t *data, size_t length, bool is_data)
{
    gpio_set_level(EPD_PIN_DC, is_data ? 1 : 0);

    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &transaction);
}

static esp_err_t epd_command(uint8_t command)
{
    return spi_write(&command, 1, false);
}

static esp_err_t epd_data(const uint8_t *data, size_t length)
{
    if (length == 0) {
        return ESP_OK;
    }
    return spi_write(data, length, true);
}

static esp_err_t epd_command_data(uint8_t command, const uint8_t *data, size_t length)
{
    ESP_RETURN_ON_ERROR(epd_command(command), TAG, "SPI command 0x%02X failed", command);
    return epd_data(data, length);
}

static esp_err_t set_full_ram_window(void)
{
    /* Follow the panel's reference sequence: X increases while Y scans down. */
    const uint8_t entry_mode = 0x01;
    const uint8_t x_range[] = {0x00, 0x31}; // 50 bytes: X = 0..399
    const uint8_t y_range[] = {0x2B, 0x01, 0x00, 0x00}; // Y = 299..0
    const uint8_t x_start = 0x00;
    const uint8_t y_start[] = {0x2B, 0x01}; // 0x012B = 299; start at window end for decrement scan

    ESP_RETURN_ON_ERROR(epd_command_data(0x11, &entry_mode, sizeof(entry_mode)), TAG, "RAM entry mode failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x44, x_range, sizeof(x_range)), TAG, "RAM X range failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x45, y_range, sizeof(y_range)), TAG, "RAM Y range failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x4E, &x_start, sizeof(x_start)), TAG, "RAM X address failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x4F, y_start, sizeof(y_start)), TAG, "RAM Y address failed");
    return ESP_OK;
}

static esp_err_t write_plane(uint8_t ram_command, const uint8_t *plane)
{
    ESP_RETURN_ON_ERROR(set_full_ram_window(), TAG, "Setting RAM window failed");
    ESP_RETURN_ON_ERROR(epd_command(ram_command), TAG, "Selecting RAM 0x%02X failed", ram_command);
    return epd_data(plane, EPD_FRAME_BYTES);
}

static esp_err_t panel_reset_and_configure(void)
{
    /* The reference sequence allows VCI to settle before the hardware reset. */
    delay_ms(10);
    gpio_set_level(EPD_PIN_RESET, 0);
    esp_rom_delay_us(200);
    gpio_set_level(EPD_PIN_RESET, 1);
    esp_rom_delay_us(200);
    ESP_RETURN_ON_ERROR(wait_ready("Hardware reset", 1000, false), TAG, "Controller remained busy after hardware reset");

    /* Software reset followed by the SSD1683 configuration listed for this
     * exact panel in the Good Display datasheet, section 14.2. */
    ESP_RETURN_ON_ERROR(epd_command(0x12), TAG, "Software reset failed");
    delay_ms(10);
    ESP_RETURN_ON_ERROR(wait_ready("Software reset", 1000, false), TAG, "Controller did not become ready after reset");

    const uint8_t driver_output[] = {0x2B, 0x01, 0x00}; // 300 gate lines
    const uint8_t entry_mode = 0x01; // X increment, Y decrement (panel reference mode)
    const uint8_t x_range[] = {0x00, 0x31};
    const uint8_t y_range[] = {0x2B, 0x01, 0x00, 0x00};
    const uint8_t border = 0x01;
    const uint8_t temperature_sensor = 0x80;
    const uint8_t booster_soft_start[] = {0x8B, 0x9C, 0x96, 0x0F};

    ESP_RETURN_ON_ERROR(epd_command_data(0x01, driver_output, sizeof(driver_output)), TAG, "Driver output setup failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x11, &entry_mode, sizeof(entry_mode)), TAG, "RAM entry mode setup failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x44, x_range, sizeof(x_range)), TAG, "RAM X range setup failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x45, y_range, sizeof(y_range)), TAG, "RAM Y range setup failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x3C, &border, sizeof(border)), TAG, "Border waveform setup failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x18, &temperature_sensor, sizeof(temperature_sensor)), TAG, "Temperature sensor setup failed");
    ESP_RETURN_ON_ERROR(epd_command_data(0x0C, booster_soft_start, sizeof(booster_soft_start)), TAG, "Booster soft-start setup failed");

    s_sleeping = false;
    return ESP_OK;
}

esp_err_t gdey042z98_init(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_config), TAG, "Configuring control GPIOs failed");
#if CONFIG_EPD_BUSY_GPIO >= 0
    gpio_config_t busy_config = {
        .pin_bit_mask = 1ULL << EPD_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_config), TAG, "Configuring BUSY GPIO failed");
#endif

    spi_bus_config_t bus_config = {
        .mosi_io_num = EPD_PIN_SDI,
        .miso_io_num = -1,
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EPD_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = EPD_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = EPD_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(EPD_HOST, &device_config, &s_spi), TAG, "Adding e-paper SPI device failed");

    ESP_RETURN_ON_ERROR(panel_reset_and_configure(), TAG, "Panel initialization failed");

    ESP_LOGI(TAG, "Initialized SSD1683 for %dx%d tri-color panel", EPD_WIDTH, EPD_HEIGHT);
    return ESP_OK;
}

esp_err_t gdey042z98_display(const uint8_t *black_plane, const uint8_t *red_plane)
{
    ESP_RETURN_ON_FALSE(s_spi != NULL, ESP_ERR_INVALID_STATE, TAG, "Initialize the panel before drawing");
    ESP_RETURN_ON_FALSE(black_plane != NULL && red_plane != NULL, ESP_ERR_INVALID_ARG, TAG, "Both frame planes are required");
    if (s_sleeping) {
        ESP_RETURN_ON_ERROR(panel_reset_and_configure(), TAG, "Waking the panel failed");
    }

    ESP_RETURN_ON_ERROR(write_plane(0x24, black_plane), TAG, "Writing black/white plane failed");
    ESP_RETURN_ON_ERROR(write_plane(0x26, red_plane), TAG, "Writing red plane failed");

    /* Load the OTP waveform for a full color update and start the panel scan. */
    const uint8_t full_update = 0xF7;
    ESP_RETURN_ON_ERROR(epd_command_data(0x22, &full_update, sizeof(full_update)), TAG, "Selecting full refresh failed");
    ESP_RETURN_ON_ERROR(epd_command(0x20), TAG, "Starting display refresh failed");
    ESP_LOGI(TAG, "Full refresh started");
    ESP_RETURN_ON_ERROR(wait_ready("Full refresh", EPD_FULL_REFRESH_WAIT_MS, true), TAG, "Full refresh did not complete");

    /* The panel is bistable; put its controller into deep sleep after refresh. */
    const uint8_t deep_sleep = 0x01;
    ESP_RETURN_ON_ERROR(epd_command_data(0x10, &deep_sleep, sizeof(deep_sleep)), TAG, "Entering deep sleep failed");
    s_sleeping = true;
    ESP_LOGI(TAG, "Refresh complete; panel retains the image without power");
    return ESP_OK;
}




