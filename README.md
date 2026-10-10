# ESP32-S3 GDEY042Z98 墨水屏日历

基于 ESP-IDF 的 4.2 英寸三色电子纸日历，使用 ESP32-S3 驱动 Good Display GDEY042Z98（SSD1683，400 × 300）。设备连接 Wi-Fi 并通过 SNTP 获取北京时间，在墨水屏上显示当月日期；电子纸刷新后断电仍可保留画面。

功能变更记录见 [CHANGELOG.md](CHANGELOG.md)。
第三方字形和农历数据来源见 [main/THIRD_PARTY_NOTICES.md](main/THIRD_PARTY_NOTICES.md)。

## 功能

- 左侧显示标准星期对齐的当月月历，星期标题为放大的英文缩写（周六、周日为红色）；右侧显示当天农历日期和本地实时天气。
- 左上方显示年份和当前公历月份（如“2026年10月”）；农历换算在设备上离线完成，支持 1900–2100 年。
- 今天以红色底、白色字突出显示；白底，支持黑、红、白三色。
- 右侧天气区显示当天农历、天气现象、地点名、气温和风速，数据来自 Open-Meteo（无需 API Key）。
- 年份显示在屏幕左上方原 IP 区域；右上角显示电池图标和估算电量。
- 通过 `ntp.aliyun.com` 获取 UTC+8 网络时间。首次仍会尝试原 Wi-Fi `eiiman`；成功联网校时后将凭据保存到 NVS，屏幕不显示时分。
- Wi-Fi 日常仅用于获取网络时间和天气；不启动常驻 TCP/HTTP 后台服务，屏幕不显示 IP 地址。只有临时配网模式会开启 HTTP 配置页。
- GPIO7 读取电池分压；使用 ADC 校准（芯片支持时）。
- 每次启动/唤醒后先成功完成 SNTP 校时，再获取天气并刷新一次日历，然后自动进入深度休眠；默认每 12 小时定时唤醒，也可由外部按钮唤醒。
- GPIO4 短按触发立即刷新；长按约 3 秒开启临时 Wi-Fi 配网热点，平时不运行后台服务。

屏幕布局中不显示英文月份名。左侧日期区域通常为五行；遇到按星期对齐必须使用六行的月份时，会缩小行高以完整显示日期。右侧面板分「农历」和「天气」两组，组间有细分隔线；组内行距与组间留白由 `main/calendar_ui.c` 里的 `PANEL_ROW_STEP` 和 `PANEL_GROUP_GAP` 控制，整块内容按面板上下边距垂直居中。所有中文一律以同一字号绘制，因此字重和行距保持一致。

## 天气地点

面板只显示一个固定地点的天气，地点在 `main/weather.h` 顶部用宏定义，修改后重新编译即可（浙江温岭大溪为默认值）：

```c
#define WEATHER_PLACE_NAME "温岭大溪"
#define WEATHER_LATITUDE   "28.4833"
#define WEATHER_LONGITUDE  "121.3500"
#define WEATHER_TIMEZONE   "Asia/Shanghai"
```

- `WEATHER_LATITUDE` / `WEATHER_LONGITUDE` 是十进制度，南纬和西经为负值，也是天气服务实际需要的地点参数。
- `WEATHER_PLACE_NAME` 用内置中文点阵显示，字符必须已包含在 `main/chinese_font.h` 中，且宽度不超过右侧面板（约 5 个汉字）。字符缺失或文字过宽时面板会跳过地点名而保留其他内容；新增汉字的方法见 `tools/glyphgen.py`（`python tools/glyphgen.py header 20 --weight=600` 可重新生成字库）。
- 天气接口地址、超时和响应缓冲也都在 `main/weather.h` 中定义；`weather.c` 里的 WMO 天气代码中文映射表在更换天气服务时需要同步修改。
- 取不到天气时（无网络、接口失败）天气行显示「无法获取」，不影响日历刷新。

## 硬件接线

| 墨水屏信号 | ESP32-S3 GPIO |
| --- | ---: |
| SDI / MOSI / SDA | 38 |
| SCLK / SCL | 39 |
| CS | 40 |
| D/C | 41 |
| RESET | 42 |
| BUSY | 1 |
| 电池 ADC | 7（ADC1_CH6） |
| 深度休眠唤醒按钮 | 4（按钮另一端接 GND，低电平唤醒） |

MISO 未使用，屏幕需配置为 4 线 SPI（BS1 拉低）。本工程面向 **ESP32-S3 N16R8** 模组（16 MB Flash + 8 MB PSRAM）；该模组的 Octal PSRAM 占用 GPIO33–37，即使当前未启用 PSRAM 也不要拿这几个脚接外设。BUSY 默认 GPIO 为 1，可在 `idf.py menuconfig` 的 `GDEY042Z98 e-paper configuration` 中修改。

## Flash 分区

`partitions.csv` 按 16 MB Flash 划分，应用分区从 1 MB 扩大到 6 MB；`sdkconfig.defaults`（已纳入版本管理）保证新克隆的工程沿用 `esp32s3` + 16 MB + 该分区表，`sdkconfig` 本身是生成文件、不提交。

| 分区 | 类型 | 偏移 | 大小 | 说明 |
| --- | --- | ---: | ---: | --- |
| nvs | data/nvs | 0x9000 | 24 KB | Wi-Fi 凭据等 |
| otadata | data/ota | 0xf000 | 8 KB | OTA 状态 |
| phy_init | data/phy | 0x11000 | 4 KB | 射频校准 |
| ota_0 | app | 0x20000 | 6 MB | 当前运行的应用 |
| ota_1 | app | 0x620000 | 6 MB | 预留的第二个应用槽 |
| storage | data/spiffs | 0xc20000 | 3904 KB | 预留数据区 |

当前固件约 0.96 MB，应用分区剩余约 84%。第二个 6 MB 槽位预留给后续 OTA 升级，现在不启用 OTA，两个槽位都可由 `idf.py flash` 直接写入。

**分区表变更后需要整片擦除再烧录**（NVS 偏移从 0x9000 起算没变，但仍建议擦净，避免残留数据）：

```powershell
idf.py -p COMx erase-flash
idf.py -p COMx flash monitor
```

`idf.py flash` 会自动按 `flash_args` 写入 bootloader、分区表、`ota_data_initial.bin` 和应用镜像，无需手动指定地址。8 MB PSRAM 目前未启用（`CONFIG_SPIRAM` 关闭）：固件两块 400×300 单色帧缓冲合计仅 30 KB，DRAM 还剩约 26%。

## 电池电量

工程按已确认的 **1:2 分压**配置 `BATTERY_DIVIDER_RATIO = 2.0`，即用 ADC 测得的分压电压乘以 2 估算电池电压。当前百分比映射假设电池为单节锂电池，3.3 V 对应 0%，4.2 V 对应 100%；这只是基于电压的粗略估算，并非库仑计。若电池类型或分压电路改变，请调整 `main/spi_master_example_main.c` 中的电压阈值和倍率。务必确保 GPIO7 电压不超过芯片允许范围。

## Wi-Fi 和时间

设备连接 Wi-Fi 后启动 SNTP 校时；**校时失败不会立即休眠，而是保持唤醒继续重试**：默认最多尝试 5 次、每次等待 20 秒、两次之间间隔 3 秒（总计约 2 分钟），任一次成功就继续取天气并刷新。全部失败才进入原有的 30 分钟休眠重试，避免一直耗电。重试次数与间隔由 `main/spi_master_example_main.c` 的 `SNTP_SYNC_ATTEMPTS`、`SNTP_SYNC_TIMEOUT_SECONDS` 和 `SNTP_SYNC_RETRY_DELAY_SECONDS` 控制。项目不提供常驻 TCP/HTTP 远程访问接口。

校时成功后，设备会在仍然联网时向 Open-Meteo 请求一次当前位置的实时天气（`current=temperature_2m,weather_code,wind_speed_10m`），随后才刷新屏幕。天气请求失败只影响右侧天气区，不影响日历；单次请求超时由 `WEATHER_HTTP_TIMEOUT_MS` 控制。

深度休眠默认间隔为 12 小时，按钮 GPIO 默认为 GPIO4；可在 `main/spi_master_example_main.c` 修改 `DEEP_SLEEP_INTERVAL_HOURS`、`WIFI_RETRY_SLEEP_MINUTES` 和 `WAKE_BUTTON_GPIO`。按钮输入使用 RTC 内部上拉，按下时将 GPIO4 拉到 GND。按钮唤醒后设备会重新连接 Wi-Fi、获取最新网络时间、刷新屏幕，再次休眠。若按键在进入休眠时仍被按住，固件会等到松开后再睡眠。

更换 Wi-Fi 时，从休眠状态**长按 GPIO4 约 3 秒**，手机连接热点 `E-Paper-Setup`（密码 `epaper123`），浏览器打开 `http://192.168.4.1/`，填入新 Wi-Fi 名称和密码。设备重启后会先验证联网和 SNTP 校时，成功后才保存为正式配置、刷新日历并休眠。配网热点最多运行 5 分钟；首次默认网络不可用时也会自动开启配网热点。可通过 `SETUP_AP_SSID`、`SETUP_AP_PASSWORD` 和 `SETUP_PORTAL_TIMEOUT_MINUTES` 修改热点名称、密码和配网时长。

## 编译和烧录

使用 ESP-IDF 5.2 PowerShell 环境，在工程目录执行：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

首次配置目标芯片时执行 `set-target`；`sdkconfig.defaults` 会在生成 `sdkconfig` 时自动套用 16 MB Flash 和本工程的分区表。将 `COMx` 替换成开发板串口；改了 `partitions.csv` 之后请先 `idf.py -p COMx erase-flash` 再烧录。构建产物为 `build/E-Paper.bin`。三色全刷约需 20 秒；每次上电或唤醒后成功校时并刷新一次日历，然后自动进入深度休眠。

## 开发辅助脚本

- `tools/glyphgen.py` 从系统里的 Noto Sans SC 生成 `main/chinese_font.h` 的 20×20 字形（需要 `freetype-py`）。面板是 1bit，直接取 Regular 字重会偏细，所以用 `--weight=600` 取半粗实例、并留出格内留白：

  ```powershell
  python tools/glyphgen.py header 20 --weight=600
  ```

- `tools/preview.py` 按 `calendar_ui.c` 的坐标和真实字库把整屏渲染成 PNG，改排版前后都应该看一眼：

  ```powershell
  python tools/preview.py build/panel.png --date 2026-02-14 --scale 3
  ```

- `tools/check_layout.py` 检查排版不变量（ASCII 字模必须随字号缩放、混合中英文按字符度量、右侧面板每行按可见墨迹居中且不越界）。
- `tools/panel_layout.py` 校验右侧面板各行的行宽与上下边界。
- `tools/weather_parse_test.c` 是天气响应解析的宿主机测试；`tools/weather_parse_sim.py` 用同样的算法校验这些测试用例。

> 注意：面板内置的 5×7 ASCII 字模只有数字和**大写**字母，没有小写字母。中文行里的单位要用汉字（如 `度`、`公里`），写 `km/h` 会渲染成空白。
