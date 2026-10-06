# Third-party notices

## Noto Sans CJK SC glyph subset

`chinese_font.h` contains a small 20x20 raster subset generated from Noto Sans SC (via `tools/glyphgen.py`) for the Chinese lunar, weather and location labels. Noto Sans CJK is copyright 2014-2021 Adobe and is licensed under the SIL Open Font License, Version 1.1. The license text and upstream files are available from the [Noto CJK project](https://github.com/notofonts/noto-cjk).

## Weather data

Current conditions come from the [Open-Meteo](https://open-meteo.com/) forecast API, which is free for non-commercial use and needs no API key. Weather codes returned by the service are WMO interpretation codes; `weather_code_text()` in `weather.c` maps them to Chinese labels.

## Lunar calendar data

The packed 1900-2100 lunar year table in `calendar_ui.c` follows the corrected table published with [jjonline/calendar.js](https://gist.github.com/pingdongyi/bf75e285bff445e4fb2820729ba913c4). The conversion code in this project is an independent C implementation.
