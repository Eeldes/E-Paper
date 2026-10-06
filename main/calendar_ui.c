#include "calendar_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "chinese_font.h"
#include "gdey042z98.h"
#include "weather.h"

#define WIDTH 400
#define HEIGHT 300
#define BYTES_PER_ROW (WIDTH / 8)
#define FRAME_BYTES (BYTES_PER_ROW * HEIGHT)
#define GRID_X 7
#define GRID_W 259
#define GRID_Y 58
#define CALENDAR_BOTTOM 286

/* ------------------------------------------------------------------------- *
 * Text metrics
 *
 * The ASCII font is stored as 5x7 cells.  Every ASCII metric is expressed in
 * quarter-pixels and multiplied by the caller's scale, so a glyph's size and
 * its advance grow together; rounding happens once, when a pixel is set.
 * Chinese glyphs come from a single 20x20 face and are always drawn at
 * CHINESE_SIZE so every Chinese string on the panel has the same weight and
 * line height.
 * ------------------------------------------------------------------------- */
#define SCALE_UNIT 4
#define CHINESE_SIZE 20

/* 5x7 cell metrics, in pixels at scale 4 (so every value scales as
 * `value * scale / SCALE_UNIT`).  A cell advances 6 units while its glyph body
 * is 5 units wide, leaving a small gap between neighbouring cells so stacked
 * digits such as the year stay readable. */
#define ASCII_ADVANCE_UNITS 6
#define ASCII_GLYPH_UNITS 5
#define ASCII_BODY_HEIGHT_UNITS 7

/* ASCII scale (in quarter-pixels) for numbers and units drawn inside Chinese
 * lines, and the smallest scale a line may shrink to in order to fit. */
#define ASCII_VALUE_SCALE 8
#define ASCII_SCALE_MIN 6

typedef enum { INK_WHITE, INK_BLACK, INK_RED } ink_t;

static uint8_t s_black[FRAME_BYTES];
static uint8_t s_red[FRAME_BYTES];

typedef struct {
    int year;
    int month;
    int day;
    bool leap_month;
} lunar_date_t;

/* Packed lunar-year/month lengths for 1900-2100. Calendar data follows
 * the corrected 1900-2100 table published with jjonline/calendar.js. */
static const uint32_t s_lunar_info[] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6,
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
    0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0,
    0x0a2e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
    0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
    0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160,
    0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252,
    0x0d520
};

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
    static const uint8_t slash[5] = {0x20,0x10,0x08,0x04,0x02};
    if (c == ':') return colon;
    if (c == '%') return percent;
    if (c == '.') return period;
    if (c == '/') return slash;
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

/* --------------------------------------------------------------------------
 * ASCII helpers
 * ------------------------------------------------------------------------ */

/* Advance between two 5x7 cells, for a cell of `scale` quarter-pixels. */
static int ascii_advance(int scale)
{
    return ASCII_ADVANCE_UNITS * scale / SCALE_UNIT;
}

/* Drawn width of a 5x7 cell body at `scale` quarter-pixels; the remaining
 * advance is the gap to the next cell. */
static int ascii_glyph_width(int scale)
{
    return ASCII_GLYPH_UNITS * scale / SCALE_UNIT;
}

/* Width of `text` in pixels for a 5x7 cell of `scale` quarter-pixels. */
static int ascii_text_width(const char *text, int scale)
{
    const size_t length = strlen(text);
    if (length == 0) return 0;
    return (int)(length - 1) * ascii_advance(scale) + ascii_glyph_width(scale);
}

/* Draw one 5x7 cell of `scale` quarter-pixels with the cell top-left at
 * (x, y).  The glyph body is painted with its baseline on the cell floor, so
 * the ink of a digit or capital is 7 * scale / 4 pixels tall and every mixed
 * line keeps one baseline. */
static void draw_ascii_glyph(const uint8_t *glyph, int x, int y, int scale, ink_t ink)
{
    if (glyph == NULL) return;
    for (int col = 0; col < 5; ++col) {
        const int x0 = x + col * scale / SCALE_UNIT;
        const int x1 = x + (col + 1) * scale / SCALE_UNIT;
        for (int row = 0; row < 7; ++row) {
            if (!((glyph[col] >> row) & 1U)) continue;
            const int y0 = y + row * scale / SCALE_UNIT;
            const int y1 = y + (row + 1) * scale / SCALE_UNIT;
            for (int py = y0; py < y1; ++py)
                for (int px = x0; px < x1; ++px)
                    pixel(px, py, ink);
        }
    }
}

static void draw_text(const char *text, int x, int y, int scale, ink_t ink)
{
    const int advance = ascii_advance(scale);
    for (; *text; ++text, x += advance) {
        draw_ascii_glyph(glyph_for(*text), x, y, scale, ink);
    }
}

/* Vertical centre of the ink produced by draw_text() at the same arguments. */
static int ascii_center_y(int y, int scale)
{
    return y + ASCII_BODY_HEIGHT_UNITS * scale / (2 * SCALE_UNIT);
}

/* --------------------------------------------------------------------------
 * Chinese helpers
 *
 * Chinese glyphs are 20x20 cells drawn at CHINESE_SIZE; ASCII taken from the
 * 5x7 font is drawn at a caller-chosen scale.  Because the two advances differ,
 * a mixed string such as "风6km/h" must be measured per character instead of
 * one cell per character.
 * ------------------------------------------------------------------------ */

static const chinese_glyph_t *chinese_glyph(uint16_t codepoint)
{
    for (size_t i = 0; i < sizeof(s_chinese_font) / sizeof(s_chinese_font[0]); ++i) {
        if (s_chinese_font[i].codepoint == codepoint) return &s_chinese_font[i];
    }
    return NULL;
}

/* Convert the next UTF-8 codepoint and advance the cursor past it. */
static uint16_t utf8_next_codepoint(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    if (p[0] < 0x80) {
        *text += 1;
        return p[0];
    }
    if ((p[0] & 0xF0) == 0xE0 && p[1] != 0 && p[2] != 0) {
        const uint16_t codepoint = (uint16_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
        *text += 3;
        return codepoint;
    }
    *text += 1;
    return 0;
}

/* Horizontal offset of a glyph's ink inside its own cell. */
static void chinese_glyph_ink_x(const chinese_glyph_t *glyph, int size, int *start, int *end)
{
    int first = size;
    int last = -1;
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 20; ++x) {
            if ((glyph->rows[y] >> (19 - x)) & 1U) {
                if (x < first) first = x;
                if (x > last) last = x;
            }
        }
    }
    if (last < 0) {
        *start = 0;
        *end = size;
        return;
    }
    *start = first * size / 20;
    *end = (last + 1) * size / 20;
}

/* Advance of one character at the current Chinese size and ASCII scale. */
static int char_advance(uint16_t codepoint, int ascii_scale)
{
    return chinese_glyph(codepoint) != NULL ? CHINESE_SIZE : ascii_advance(ascii_scale);
}

/* Pen advance of a mixed string, in pixels. */
static int mixed_text_width(const char *text, int ascii_scale)
{
    int width = 0;
    const char *cursor = text;
    while (cursor && *cursor) {
        width += char_advance(utf8_next_codepoint(&cursor), ascii_scale);
    }
    return width;
}

/* Ink extent of a mixed string relative to the pen origin: [start, end).  The
 * blank side bearings of the 20x20 cells are excluded so that a string is
 * centred by what is actually visible, not by its advance width. */
static void mixed_text_ink(const char *text, int ascii_scale, int *start, int *end)
{
    const char *cursor = text;
    int pen = 0;
    int first = -1;
    int last = -1;
    while (cursor && *cursor) {
        const uint16_t codepoint = utf8_next_codepoint(&cursor);
        const chinese_glyph_t *glyph = chinese_glyph(codepoint);
        if (glyph != NULL) {
            int ink_start = 0;
            int ink_end = 0;
            chinese_glyph_ink_x(glyph, CHINESE_SIZE, &ink_start, &ink_end);
            if (ink_end > ink_start) {
                if (first < 0) first = pen + ink_start;
                last = pen + ink_end;
            }
            pen += CHINESE_SIZE;
        } else {
            const int advance = ascii_advance(ascii_scale);
            const int body = ascii_glyph_width(ascii_scale);
            if (glyph_for((char)codepoint) != NULL) {
                if (first < 0) first = pen;
                last = pen + body;
            }
            pen += advance;
        }
    }
    if (last < 0) {
        *start = 0;
        *end = 0;
        return;
    }
    *start = first;
    *end = last;
}

static bool chinese_text_supported(const char *text)
{
    const char *cursor = text;
    if (cursor == NULL || *cursor == '\0') return false;
    while (*cursor) {
        const uint16_t codepoint = utf8_next_codepoint(&cursor);
        if (chinese_glyph(codepoint) == NULL) return false;
    }
    return true;
}

/* Draw every glyph that exists in the subset; unknown codepoints advance the
 * pen without drawing, so a stray character cannot shift the rest of a line. */
static void draw_mixed_text(const char *text, int x, int y, int ascii_scale, ink_t ink)
{
    while (text && *text) {
        const uint16_t codepoint = utf8_next_codepoint(&text);
        const chinese_glyph_t *glyph = chinese_glyph(codepoint);
        if (glyph != NULL) {
            for (int py = 0; py < CHINESE_SIZE; ++py) {
                for (int px = 0; px < CHINESE_SIZE; ++px) {
                    if ((glyph->rows[py] >> (19 - px)) & 1U) {
                        pixel(x + px, y + py, ink);
                    }
                }
            }
        } else {
            draw_ascii_glyph(glyph_for((char)codepoint), x, y, ascii_scale, ink);
        }
        x += char_advance(codepoint, ascii_scale);
    }
}

/* Draw a mixed string centred in [left, left + width) by its visible ink. */
static void draw_mixed_centered(const char *text, int left, int width, int y, int ascii_scale, ink_t ink)
{
    int ink_start = 0;
    int ink_end = 0;
    mixed_text_ink(text, ascii_scale, &ink_start, &ink_end);
    const int visible = ink_end - ink_start;
    draw_mixed_text(text, left + (width - visible) / 2 - ink_start, y, ascii_scale, ink);
}

/* Largest ASCII scale that keeps a mixed string inside `width`, never above
 * `max_scale`.  Keeps numbers and units from being clipped by the panel. */
static int mixed_fit_scale(const char *text, int width, int max_scale, int min_scale)
{
    for (int scale = max_scale; scale > min_scale; --scale) {
        int ink_start = 0;
        int ink_end = 0;
        mixed_text_ink(text, scale, &ink_start, &ink_end);
        if (ink_end - ink_start <= width) return scale;
    }
    return min_scale;
}

/* --------------------------------------------------------------------------
 * Lunar calendar
 * ------------------------------------------------------------------------ */

static int64_t civil_day_number(int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned adjusted_month = (unsigned)(month + (month > 2 ? -3 : 9));
    const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + (unsigned)day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static int lunar_leap_month(int year)
{
    return (int)(s_lunar_info[year - 1900] & 0x0FU);
}

static int lunar_leap_days(int year)
{
    return lunar_leap_month(year) ? ((s_lunar_info[year - 1900] & 0x10000U) ? 30 : 29) : 0;
}

static int lunar_month_days(int year, int month)
{
    return (s_lunar_info[year - 1900] & (0x10000U >> month)) ? 30 : 29;
}

static int lunar_year_days(int year)
{
    int days = 348;
    for (uint32_t bit = 0x8000U; bit > 0x8U; bit >>= 1) {
        if (s_lunar_info[year - 1900] & bit) ++days;
    }
    return days + lunar_leap_days(year);
}

static bool solar_to_lunar(const struct tm *date, lunar_date_t *lunar)
{
    const int year = date->tm_year + 1900;
    if (year < 1900 || year > 2100) return false;
    int64_t offset = civil_day_number(year, date->tm_mon + 1, date->tm_mday) - civil_day_number(1900, 1, 31);
    if (offset < 0) return false;

    int lunar_year = 1900;
    while (lunar_year < 2100 && offset >= lunar_year_days(lunar_year)) {
        offset -= lunar_year_days(lunar_year++);
    }

    const int leap_month = lunar_leap_month(lunar_year);
    for (int month = 1; month <= 12; ++month) {
        const int days = lunar_month_days(lunar_year, month);
        if (offset < days) {
            *lunar = (lunar_date_t){.year = lunar_year, .month = month, .day = (int)offset + 1, .leap_month = false};
            return true;
        }
        offset -= days;
        if (month == leap_month) {
            const int leap_days = lunar_leap_days(lunar_year);
            if (offset < leap_days) {
                *lunar = (lunar_date_t){.year = lunar_year, .month = month, .day = (int)offset + 1, .leap_month = true};
                return true;
            }
            offset -= leap_days;
        }
    }
    return false;
}

static void lunar_month_text(char *out, size_t out_size, const lunar_date_t *lunar)
{
    static const char *const month_names[] = {"正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"};
    snprintf(out, out_size, "%s%s月", lunar->leap_month ? "闰" : "", month_names[lunar->month - 1]);
}

static void lunar_day_text(char *out, size_t out_size, int day)
{
    static const char *const digits[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};
    if (day == 10) strlcpy(out, "初十", out_size);
    else if (day == 20) strlcpy(out, "二十", out_size);
    else if (day == 30) strlcpy(out, "三十", out_size);
    else if (day < 10) snprintf(out, out_size, "初%s", digits[day - 1]);
    else if (day < 20) snprintf(out, out_size, "十%s", digits[day - 11]);
    else snprintf(out, out_size, "廿%s", digits[day - 21]);
}

/* --------------------------------------------------------------------------
 * Panel sections
 * ------------------------------------------------------------------------ */

/* Right-hand column: today's lunar date, the local weather for
 * WEATHER_PLACE_NAME, and the temperature/wind read from the service.
 *
 * The panel is split into groups (the lunar date and the weather report) so the
 * two topics read as separate blocks: rows within a group are spaced a line
 * apart, and ROW_GROUP_GAP separates the groups.  The whole block is centred
 * vertically and horizontally inside the panel margins. */
#define PANEL_LEFT 280
#define PANEL_WIDTH 108
#define PANEL_MARGIN_TOP 64
#define PANEL_MARGIN_BOTTOM 276
#define PANEL_ROW_STEP 26
#define PANEL_GROUP_GAP 22

static void draw_today_panel(const struct tm *local_time, const weather_info_t *weather)
{
    vline(274, GRID_Y - 4, CALENDAR_BOTTOM - GRID_Y + 4, INK_BLACK);

    lunar_date_t lunar;
    if (!solar_to_lunar(local_time, &lunar)) return;

    char month_text[16];
    lunar_month_text(month_text, sizeof(month_text), &lunar);

    char day_text[16];
    lunar_day_text(day_text, sizeof(day_text), lunar.day);

    char condition[24] = {0};
    char temperature[24] = {0};
    char wind[24] = {0};
    const char *name = NULL;
    if (weather != NULL && weather->valid) {
        snprintf(condition, sizeof(condition), "%s", weather_code_text(weather->weather_code));
        snprintf(temperature, sizeof(temperature), "%d度", weather->temperature_c);
        /* Chinese unit: the panel's 5x7 ASCII font has no lowercase letters. */
        if (weather->has_wind) snprintf(wind, sizeof(wind), "风%d公里", weather->wind_kmh);
        name = WEATHER_PLACE_NAME;
    } else {
        /* Compact fallback: the weather block would otherwise be one bare "--". */
        strlcpy(condition, "无法获取", sizeof(condition));
    }

    /* Only show the place name when the built-in font can draw every glyph
     * in it; otherwise keep the lines that are guaranteed to render. */
    if (name != NULL && !chinese_text_supported(name)) name = NULL;
    if (name != NULL && mixed_text_width(name, ASCII_VALUE_SCALE) > PANEL_WIDTH) name = NULL;

    /* Rows are (label, value, starts_new_group); a NULL label means the value
     * carries the line on its own. */
    enum { MAX_ROWS = 8 };
    const char *labels[MAX_ROWS];
    const char *values[MAX_ROWS];
    bool group_start[MAX_ROWS];
    int row_count = 0;

    labels[row_count] = "农历";
    values[row_count] = month_text;
    group_start[row_count++] = true;
    labels[row_count] = NULL;
    values[row_count] = day_text;
    group_start[row_count++] = false;

    labels[row_count] = NULL;
    values[row_count] = condition;
    group_start[row_count++] = true;
    if (name != NULL) {
        labels[row_count] = NULL;
        values[row_count] = name;
        group_start[row_count++] = false;
    }
    if (temperature[0] != '\0') {
        labels[row_count] = NULL;
        values[row_count] = temperature;
        group_start[row_count++] = false;
    }
    if (wind[0] != '\0') {
        labels[row_count] = NULL;
        values[row_count] = wind;
        group_start[row_count++] = false;
    }
    if (row_count > MAX_ROWS) return;

    /* Centre the block, then place each row, adding the group gap where a new
     * group begins. */
    const int block_height = (row_count - 1) * PANEL_ROW_STEP + CHINESE_SIZE + PANEL_GROUP_GAP;
    int y = PANEL_MARGIN_TOP + ((PANEL_MARGIN_BOTTOM - PANEL_MARGIN_TOP + 1) - block_height) / 2;
    for (int i = 0; i < row_count; ++i) {
        if (i > 0 && group_start[i]) {
            /* A hairline between the groups keeps the two topics apart without
             * the empty middle looking accidental, the way the almanac panel
             * used to be divided. */
            hline(PANEL_LEFT + 8, y - 2, PANEL_WIDTH - 16, INK_BLACK);
            y += PANEL_GROUP_GAP;
        }
        const char *label = labels[i];
        const char *value = values[i];
        /* The ASCII part (digits, units, signs) is sized per line so the text
         * always fits the panel instead of being clipped. */
        const int value_scale = mixed_fit_scale(value, PANEL_WIDTH, ASCII_VALUE_SCALE, ASCII_SCALE_MIN);
        if (label != NULL) {
            int label_start = 0;
            int label_end = 0;
            int value_start = 0;
            int value_end = 0;
            mixed_text_ink(label, ASCII_VALUE_SCALE, &label_start, &label_end);
            mixed_text_ink(value, value_scale, &value_start, &value_end);
            const int pair_width = (label_end - label_start) + (value_end - value_start);
            /* A label is only shown when the pair fits; the data matters more. */
            if (pair_width <= PANEL_WIDTH) {
                const int x = PANEL_LEFT + (PANEL_WIDTH - pair_width) / 2;
                /* Labels are red and sit one pixel high so the pair does not
                 * look bottom-heavy. */
                draw_mixed_text(label, x - label_start, y + 1, ASCII_VALUE_SCALE, INK_RED);
                draw_mixed_text(value, x + (label_end - label_start) - value_start, y, value_scale, INK_BLACK);
                y += PANEL_ROW_STEP;
                continue;
            }
        }
        draw_mixed_centered(value, PANEL_LEFT, PANEL_WIDTH, y, value_scale, INK_BLACK);
        y += PANEL_ROW_STEP;
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
                             const weather_info_t *weather)
{
    if (local_time == NULL) return ESP_ERR_INVALID_ARG;
    memset(s_black, 0xFF, sizeof(s_black));
    memset(s_red, 0x00, sizeof(s_red));

    static const char *const weekdays[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (local_time->tm_mon < 0 || local_time->tm_mon > 11) return ESP_ERR_INVALID_ARG;

    const int year_number = local_time->tm_year + 1900;
    if (year_number < 0 || year_number > 9999) return ESP_ERR_INVALID_ARG;

    /* Header: the Arabic year/month digits and the Chinese 年/月 units share a
     * vertical centre so nothing sits higher than its neighbour. */
    char year[5];
    year[0] = (char)('0' + (year_number / 1000) % 10);
    year[1] = (char)('0' + (year_number / 100) % 10);
    year[2] = (char)('0' + (year_number / 10) % 10);
    year[3] = (char)('0' + year_number % 10);
    year[4] = '\0';
    const int header_digit_scale = 12;
    const int header_y = 8;
    const int header_center = ascii_center_y(header_y, header_digit_scale);
    const int header_unit_y = header_center - CHINESE_SIZE / 2 + 1;
    draw_text(year, 13, header_y, header_digit_scale, INK_RED);
    const int year_unit_x = 13 + ascii_text_width(year, header_digit_scale) + 2;
    draw_mixed_text("年", year_unit_x, header_unit_y, header_digit_scale, INK_RED);

    char month_number[3];
    const int month_value = local_time->tm_mon + 1;
    if (month_value >= 10) {
        month_number[0] = '1';
        month_number[1] = (char)('0' + month_value - 10);
        month_number[2] = '\0';
    } else {
        month_number[0] = (char)('0' + month_value);
        month_number[1] = '\0';
    }
    const int month_x = year_unit_x + CHINESE_SIZE + 2;
    draw_text(month_number, month_x, header_y, header_digit_scale, INK_RED);
    draw_mixed_text("月", month_x + ascii_text_width(month_number, header_digit_scale) + 2,
                    header_unit_y, header_digit_scale, INK_RED);
    draw_battery(battery_percent);

    for (int col = 0; col < 7; ++col) {
        const int cell_left = GRID_X + col * GRID_W / 7;
        const int cell_width = GRID_X + (col + 1) * GRID_W / 7 - cell_left;
        const int width = ascii_text_width(weekdays[col], 6);
        const ink_t ink = col >= 5 ? INK_RED : INK_BLACK;
        draw_text(weekdays[col], cell_left + (cell_width - width) / 2, 38, 6, ink);
    }
    hline(GRID_X, 53, GRID_W, INK_BLACK);

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

    for (int col = 0; col <= 7; ++col) vline(GRID_X + col * GRID_W / 7, GRID_Y, cell_height * calendar_rows, INK_BLACK);
    for (int row = 0; row <= calendar_rows; ++row) hline(GRID_X, GRID_Y + row * cell_height, GRID_W, INK_BLACK);

    for (int day = 1; day <= days; ++day) {
        const int slot = offset + day - 1;
        const int col = slot % 7;
        const int row = slot / 7;
        char number[3];
        if (day < 10) {
            number[0] = (char)('0' + day);
            number[1] = '\0';
        } else {
            number[0] = (char)('0' + day / 10);
            number[1] = (char)('0' + day % 10);
            number[2] = '\0';
        }
        const int scale = 8;
        const int width = ascii_text_width(number, scale);
        const int cell_left = GRID_X + col * GRID_W / 7;
        const int cell_width = GRID_X + (col + 1) * GRID_W / 7 - cell_left;
        const int cx = cell_left + cell_width / 2;
        if (row >= calendar_rows) continue;
        const int cy = GRID_Y + row * cell_height + cell_height / 2;
        const int text_y = cy - ASCII_BODY_HEIGHT_UNITS * scale / (2 * SCALE_UNIT);
        if (day == local_time->tm_mday) {
            const int highlight_height = cell_height - 10;
            fill_rect(cx - 16, cy - highlight_height / 2, 32, highlight_height, INK_RED);
            draw_text(number, cx - width / 2, text_y, scale, INK_WHITE);
        } else {
            draw_text(number, cx - width / 2, text_y, scale, INK_BLACK);
        }
    }

    draw_today_panel(local_time, weather);
    return gdey042z98_display(s_black, s_red);
}
