# ESP32-S3 GDEY042Z98 墨水屏日历

基于 ESP-IDF 的 4.2 英寸三色电子纸日历，使用 ESP32-S3 驱动 Good Display GDEY042Z98（SSD1683，400 × 300）。设备连接 Wi-Fi 并通过 SNTP 获取北京时间，在墨水屏上显示当月日期；电子纸刷新后断电仍可保留画面。

功能变更记录见 [CHANGELOG.md](CHANGELOG.md)。

## 功能

- 显示当前年份、星期标题和当月日期，日期按标准星期对齐。
- 今天以红色底、白色字突出显示；白底，支持黑、红、白三色。
- 屏幕左上角显示 IP，右上角显示电池图标和估算电量。
- 接入 Wi-Fi `eiiman`，通过 `ntp.aliyun.com` 校时，时区为 UTC+8。屏幕不显示时分。
- 提供只读 HTTP 状态页面，可在同一局域网访问。
- GPIO7 读取电池分压；使用 ADC 校准（芯片支持时）。

屏幕布局中不显示英文月份名。日期区域通常为五行；遇到按星期对齐必须使用六行的月份时，会缩小行高以完整显示日期。

## 硬件接线

| 墨水屏信号 | ESP32-S3 GPIO |
| --- | ---: |
| MOSI / SDA | 42 |
| SCLK / SCL | 41 |
| CS | 40 |
| D/C | 39 |
| RESET | 38 |
| BUSY | 47 |
| 电池 ADC | 7（ADC1_CH6） |

MISO 未使用，屏幕需配置为 4 线 SPI（BS1 拉低）。ESP32-S3 N16R8 等带 Octal PSRAM 的模组不要使用 GPIO33–37。本工程的 BUSY 默认 GPIO 为 47，可在 `idf.py menuconfig` 的 `GDEY042Z98 e-paper configuration` 中修改。

## 电池电量

工程按已确认的 **1:2 分压**配置 `BATTERY_DIVIDER_RATIO = 2.0`，即用 ADC 测得的分压电压乘以 2 估算电池电压。当前百分比映射假设电池为单节锂电池，3.3 V 对应 0%，4.2 V 对应 100%；这只是基于电压的粗略估算，并非库仑计。若电池类型或分压电路改变，请调整 `main/spi_master_example_main.c` 中的电压阈值和倍率。务必确保 GPIO7 电压不超过芯片允许范围。

## Wi-Fi、时间和后台

SSID 在 `main/spi_master_example_main.c` 中设置。设备取得 IP 后会启动 NTP 校时，并在串口日志中打印 IP 地址。

设备在 TCP 端口 80 提供 HTTP 页面：

- `http://<ESP32-IP>/`：状态页面
- `http://<ESP32-IP>/status`：JSON 状态（Wi-Fi、IP、时间、电量）

该后台没有身份验证，仅应在可信任的局域网中使用；不要在路由器上做公网端口转发。

## 编译和烧录

使用 ESP-IDF 5.2 PowerShell 环境，在工程目录执行：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

首次配置目标芯片时执行 `set-target`；将 `COMx` 替换成开发板串口。构建产物为 `build/E-Paper.bin`。三色全刷约需 20 秒；首次成功校时、日期变化以及每小时会刷新日历。

