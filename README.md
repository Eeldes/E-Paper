# ESP32-S3 GDEY042Z98 墨水屏日历

基于 ESP-IDF 的 4.2 英寸三色电子纸日历，使用 ESP32-S3 驱动 Good Display GDEY042Z98（SSD1683，400 × 300）。设备连接 Wi-Fi 并通过 SNTP 获取北京时间，在墨水屏上显示当月日期；电子纸刷新后断电仍可保留画面。

功能变更记录见 [CHANGELOG.md](CHANGELOG.md)。
第三方字形和农历数据来源见 [main/THIRD_PARTY_NOTICES.md](main/THIRD_PARTY_NOTICES.md)。

## 功能

- 左侧显示标准星期对齐的当月月历，星期标题为放大的英文缩写（周六、周日为红色）；右侧显示当天农历日期、年/日干支和生肖。
- 年份位于左上方；农历换算在设备上离线完成，支持 1900–2100 年。
- 今天以红色底、白色字突出显示；白底，支持黑、红、白三色。
- 年份显示在屏幕左上方原 IP 区域；右上角显示电池图标和估算电量。
- 接入 Wi-Fi `eiiman`，通过 `ntp.aliyun.com` 校时，时区为 UTC+8。屏幕不显示时分。
- Wi-Fi 仅用于获取网络时间；不启动 TCP/HTTP 后台服务，屏幕不显示 IP 地址。
- GPIO7 读取电池分压；使用 ADC 校准（芯片支持时）。
- 每次启动/唤醒后先成功完成 SNTP 校时，再刷新一次日历并自动进入深度休眠；默认每 12 小时定时唤醒，也可由外部按钮唤醒。

屏幕布局中不显示英文月份名。左侧日期区域通常为五行；遇到按星期对齐必须使用六行的月份时，会缩小行高以完整显示日期。右侧万年历信息使用内置的 20×20 中文字形子集。

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
| 深度休眠唤醒按钮 | 4（按钮另一端接 GND，低电平唤醒） |

MISO 未使用，屏幕需配置为 4 线 SPI（BS1 拉低）。ESP32-S3 N16R8 等带 Octal PSRAM 的模组不要使用 GPIO33–37。本工程的 BUSY 默认 GPIO 为 47，可在 `idf.py menuconfig` 的 `GDEY042Z98 e-paper configuration` 中修改。

## 电池电量

工程按已确认的 **1:2 分压**配置 `BATTERY_DIVIDER_RATIO = 2.0`，即用 ADC 测得的分压电压乘以 2 估算电池电压。当前百分比映射假设电池为单节锂电池，3.3 V 对应 0%，4.2 V 对应 100%；这只是基于电压的粗略估算，并非库仑计。若电池类型或分压电路改变，请调整 `main/spi_master_example_main.c` 中的电压阈值和倍率。务必确保 GPIO7 电压不超过芯片允许范围。

## Wi-Fi 和时间

SSID 和密码在 `main/spi_master_example_main.c` 中设置。设备连上 Wi-Fi 后启动 SNTP 校时；若暂时无法连接，会继续尝试重连，并且在成功校时前不会刷新日历或进入休眠。项目不提供 TCP/HTTP 远程访问接口。

深度休眠默认间隔为 12 小时，按钮 GPIO 默认为 GPIO4；可在 `main/spi_master_example_main.c` 修改 `DEEP_SLEEP_INTERVAL_HOURS` 和 `WAKE_BUTTON_GPIO`。按钮输入使用 RTC 内部上拉，按下时将 GPIO4 拉到 GND。按钮唤醒后设备会重新连接 Wi-Fi、获取最新网络时间、刷新屏幕，再次休眠。若按键在进入休眠时仍被按住，固件会等到松开后再睡眠。

## 编译和烧录

使用 ESP-IDF 5.2 PowerShell 环境，在工程目录执行：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

首次配置目标芯片时执行 `set-target`；将 `COMx` 替换成开发板串口。构建产物为 `build/E-Paper.bin`。三色全刷约需 20 秒；每次上电或唤醒后成功校时并刷新一次日历，然后自动进入深度休眠。
