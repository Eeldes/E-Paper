#include "calendar_ui.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gdey042z98.h"

#define WIDTH 400
#define HEIGHT 300
#define BYTES_PER_ROW (WIDTH / 8)
#define FRAME_BYTES (BYTES_PER_ROW * HEIGHT)
#define CELL_W 54
#define GRID_X 11
#define GRID_Y 58
#define CALENDAR_BOTTOM 286
#define FONT_SCALE_DENOMINATOR 4

typedef enum { INK_WHITE, INK_BLACK, INK_RED } ink_t;

static uint8_t s_black[FRAME_BYTES];
static uint8_t s_red[FRAME_BYTES];

/* 5x7 column font, indexed by digits followed by uppercase A-Z. */
static const uint8_t s_font[36][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};

static const uint8_t *glyph_for(char c)
{
    if (c >= '0' && c <= '9') return s_font[c - '0'];
    if (c >= 'A' && c <= 'Z') return s_font[10 + c - 'A'];
    static const uint8_t colon[5] = {0x00,0x36,0x36,0x00,0x00};
    static const uint8_t percent[5] = {0x63,0x13,0x08,0x64,0x63};
    static const uint8_t period[5] = {0x00,0x00,0x01,0x00,0x00};
    static const uint8_t hyphen[5] = {0x08,0x08,0x08,0x08,0x08};
    if (c == ':') return colon;
    if (c == '%') return percent;
    if (c == '.') return period;
    if (c == '-') return hyphen;
    return NULL;
}

static void pixel(int x, int y, ink_t ink)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    const int panel_y = HEIGHT - 1 - y;
    const size_t index = (size_t)panel_y * BYTES_PER_ROW + (size_t)(x / 8);
    const uint8_t mask = (uint8_t)(0x80U >> (x & 7));
    if (ink == INK_WHITE) {
        s_black[index] |= mask;
        s_red[index] &= (uint8_t)~mask;
    } else if (ink == INK_RED) {
        s_black[index] &= (uint8_t)~mask;
        s_red[index] |= mask;
    } else {
        s_black[index] &= (uint8_t)~mask;
        s_red[index] &= (uint8_t)~mask;
    }
}

static void hline(int x, int y, int length, ink_t ink)
{
    for (int i = 0; i < length; ++i) pixel(x + i, y, ink);
}

static void vline(int x, int y, int length, ink_t ink)
{
    for (int i = 0; i < length; ++i) pixel(x, y + i, ink);
}

static void fill_rect(int x, int y, int width, int height, ink_t ink)
{
    for (int py = 0; py < height; ++py) hline(x, y + py, width, ink);
}

static int text_width(const char *text, int scale)
{
    const int length = (int)strlen(text);
    if (length == 0) return 0;
    const int glyph_width = (5 * scale + FONT_SCALE_DENOMINATOR - 1) / FONT_SCALE_DENOMINATOR;
    const int advance = (6 * scale + FONT_SCALE_DENOMINATOR / 2) / FONT_SCALE_DENOMINATOR;
    return (length - 1) * advance + glyph_width;
}

static void two_digits(char out[3], int value)
{
    out[0] = (char)('0' + (value / 10) % 10);
    out[1] = (char)('0' + value % 10);
    out[2] = '\0';
}

static void draw_text(const char *text, int x, int y, int scale, ink_t ink)
{
    const int advance = (6 * scale + FONT_SCALE_DENOMINATOR / 2) / FONT_SCALE_DENOMINATOR;
    for (; *text; ++text, x += advance) {
        const uint8_t *glyph = glyph_for(*text);
        if (!glyph) continue;
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if ((glyph[col] >> row) & 1U) {
                    const int x0 = x + col * scale / FONT_SCALE_DENOMINATOR;
                    const int x1 = x + (col + 1) * scale / FONT_SCALE_DENOMINATOR;
                    const int y0 = y + row * scale / FONT_SCALE_DENOMINATOR;
                    const int y1 = y + (row + 1) * scale / FONT_SCALE_DENOMINATOR;
                    for (int py = y0; py < y1; ++py)
                        for (int px = x0; px < x1; ++px)
                            pixel(px, py, ink);
                }
            }
        }
    }
}

static void draw_battery(int percent)
{
    const bool unknown = percent < 0;
    if (unknown) percent = 0;
    if (percent > 100) percent = 100;
    const bool low = !unknown && percent <= 15;
    const ink_t ink = low ? INK_RED : INK_BLACK;
    const int x = 354, y = 13, w = 30, h = 20;
    hline(x, y, w, ink); hline(x, y + h, w, ink);
    vline(x, y, h + 1, ink); vline(x + w, y, h + 1, ink);
    vline(x + w + 2, y + 6, 8, ink); vline(x + w + 3, y + 6, 8, ink);
    const int fill = (w - 6) * percent / 100;
    for (int py = y + 3; py < y + h - 2; ++py) hline(x + 3, py, fill, ink);
    char label[5];
    if (unknown) strlcpy(label, "--%", sizeof(label));
    else snprintf(label, sizeof(label), "%d%%", percent);
    draw_text(label, 304, 16, 8, low ? INK_RED : INK_BLACK);
}

esp_err_t calendar_ui_render(const struct tm *local_time, int battery_percent,
                             const char *ip_address, bool wifi_connected)
{
    if (local_time == NULL) return ESP_ERR_INVALID_ARG;
    memset(s_black, 0xFF, sizeof(s_black));
    memset(s_red, 0x00, sizeof(s_red));

    static const char *const weekdays[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (local_time->tm_mon < 0 || local_time->tm_mon > 11) return ESP_ERR_INVALID_ARG;

    const int year_number = local_time->tm_year + 1900;
    if (year_number < 0 || year_number > 9999) return ESP_ERR_INVALID_ARG;
    char year[5];
    year[0] = (char)('0' + (year_number / 1000) % 10);
    year[1] = (char)('0' + (year_number / 100) % 10);
    year[2] = (char)('0' + (year_number / 10) % 10);
    year[3] = (char)('0' + year_number % 10);
    year[4] = '\0';
    char ip_label[24];
    snprintf(ip_label, sizeof(ip_label), "IP %s", ip_address ? ip_address : "0.0.0.0");
    draw_text(ip_label, 13, 8, 6, wifi_connected ? INK_BLACK : INK_RED);
    const int year_width = text_width(year, 5);
    draw_text(year, (WIDTH - year_width) / 2, 8, 8, INK_RED);
    draw_battery(battery_percent);
    for (int col = 0; col < 7; ++col) {
        const int width = text_width(weekdays[col], 4);
        draw_text(weekdays[col], GRID_X + col * CELL_W + (CELL_W - width) / 2, 39, 4, INK_RED);
    }
    hline(11, 53, WIDTH - 22, INK_BLACK);

    int days = month_days[local_time->tm_mon];
    if (local_time->tm_mon == 1 && ((year_number % 4 == 0 && year_number % 100 != 0) || year_number % 400 == 0)) days = 29;

    struct tm first = *local_time;
    first.tm_mday = 1;
    first.tm_hour = 12;
    mktime(&first);
    const int offset = (first.tm_wday + 6) % 7;
    const int required_rows = (offset + days + 6) / 7;
    const int calendar_rows = required_rows > 5 ? 6 : 5;
    const int cell_height = (CALENDAR_BOTTOM - GRID_Y) / calendar_rows;

    for (int col = 0; col <= 7; ++col) vline(GRID_X + col * CELL_W, GRID_Y, cell_height * calendar_rows, INK_BLACK);
    for (int row = 0; row <= calendar_rows; ++row) hline(GRID_X, GRID_Y + row * cell_height, CELL_W * 7, INK_BLACK);

    for (int day = 1; day <= days; ++day) {
        const int slot = offset + day - 1;
        const int col = slot % 7;
        const int row = slot / 7;
        char number[3];
        if (day < 10) {
            number[0] = (char)('0' + day);
            number[1] = '\0';
        } else {
            two_digits(number, day);
        }
        const int width = text_width(number, 2);
        const int cx = GRID_X + col * CELL_W + CELL_W / 2;
        if (row >= calendar_rows) continue;
        const int cy = GRID_Y + row * cell_height + cell_height / 2;
        if (day == local_time->tm_mday) {
            const int highlight_height = cell_height - 10;
            fill_rect(cx - 16, cy - highlight_height / 2, 32, highlight_height, INK_RED);
            draw_text(number, cx - width / 2, cy - 5, 8, INK_WHITE);
        } else {
            draw_text(number, cx - width / 2, cy - 5, 8, INK_BLACK);
        }
    }

    return gdey042z98_display(s_black, s_red);
}
