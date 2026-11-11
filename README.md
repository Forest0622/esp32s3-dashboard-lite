# ATK-DNESP32S3 开发板状态仪表盘（Lite 版 · 无照片页）

正点原子 **ATK-DNESP32S3 V1.3** 板载 2.4" 屏上的「开发板状态仪表盘」固件，ESP-IDF **v5.5.6**。
本包是 **Lite 版**：去掉照片展示页，共 **5 页**，固件更小（894 KB），可直接分享与编译。

> 带照片的自用版是另一个目录 `ATK-DNESP32S3-Dashboard-Photo`（6 页，含整屏照片页）。

## 硬件参数

| 项目 | 参数 |
| --- | --- |
| SoC | ESP32-S3 rev v0.2，双核 240 MHz，512 KB SRAM |
| Flash | 16 MB，BOYA，QIO / 80 MHz |
| PSRAM | 8 MB（Octal） |
| 屏幕 | ST7789V2 320×240，SPI2_HOST，40 MHz，RGB565 小端 |
| IO 扩展 | XL9555 @ I2C0 0x20，400 kHz |
| 外部存储 | TF 卡座，SPI 模式，CS = **GPIO2**，10 MHz，FAT32 |

## 屏幕页面（5 页）

| 页面 | 内容 |
| --- | --- |
| 1 SYSTEM STATUS | 芯片型号/主频/核数、Wi-Fi MAC、IDF 版本、运行时长、内部 RAM 与 PSRAM 余量、LED 模式、Wi-Fi 状态（ONLINE + IP）、扫到的 AP 数量 |
| 2 WIFI SCAN | 已连接 AP 的 SSID / IP / 实时 RSSI；下方 AP 列表（序号 + SSID + dBm + 信号条，>-55 绿 / >-75 黄 / 其它红），60 秒自动重扫 |
| 3 LED / BOARD IO | LED 状态块、模式（ON / OFF / BREATH）、GPIO1 占空比进度条、XL9555 按键提示 |
| 4 SD CARD | 挂载结果、`FAT32 29.1GB (29818MB)`、剩余容量、CS 引脚、根目录文件名、写读自检 |
| 5 MEM / STORAGE | RAM / PSRAM 剩余与总量、Flash 容量与总线模式、app 分区地址与大小、SD 卡容量/剩余/文件数/总线 |

底部固定提示：`K0:PAGE  K1:LED  K2:SCAN`

## 按键

| 按键 | XL9555 位 | 功能 |
| --- | --- | --- |
| K0 | 0x8000 (P15) | 翻页（5 页循环） |
| K1 | 0x4000 (P14) | 切换 LED 模式 ON → OFF → BREATH |
| K2 | 0x2000 (P13) | 立即重扫 Wi-Fi |

去抖 60 ms，读回按位取反后做低有效边沿检测（`(~now & last) & 0xF000`）。

## Wi-Fi 凭据怎么填

凭据不在代码里，放在 **`main/wifi_config.h`**（该文件已被 `.gitignore` 忽略，不会进仓库）：

```c
#define WIFI_SSID "你的SSID"
#define WIFI_PASS "你的密码"
```

该文件缺失时用占位符 `YOUR_SSID` / `YOUR_PASSWORD` 编译，只表现为 Wi-Fi 页连接失败，其它页面正常。

## 编译与烧录

```bash
# 1) 激活 ESP-IDF 环境（按自己的安装路径改）
source /Users/xiaoyi/.espressif/tools/activate_idf_v5.5.6.sh

# 2) 编译
cd ATK-DNESP32S3-Dashboard-Lite
idf.py build

# 3) 烧录（串口按实际改，macOS 常见 /dev/cu.usbserial-XXX）
idf.py -p /dev/cu.usbserial-140 -b 921600 flash

# 4) 看日志
idf.py -p /dev/cu.usbserial-140 monitor
```

只烧固件、不动分区表与 bootloader：

```bash
esptool.py -p /dev/cu.usbserial-140 -b 921600 write_flash 0x10000 build/esp32s3_dashboard.bin
```

本包已附带编译好的 `firmware-lite.bin`（0xDF8B0，约 894 KB），可直接烧到 `0x10000`。

## 分区表（`partitions.csv`）

| 名称 | 类型 | 偏移 | 大小 |
| --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24 KB |
| phy_init | data/phy | 0xF000 | 4 KB |
| factory | app | 0x10000 | 1536 KB |

Flash 共 16 MB，剩余约 14.4 MB 未分配，可再划 `storage` 数据分区或 OTA 双槽。

## 关键配置与踩过的坑（改代码前先看）

1. **屏偏色的唯一根因**：`data_endian` 必须是 `LCD_RGB_DATA_ENDIAN_LITTLE`。设成 BIG 时小面积 UI 看似正常、整屏图片呈彩色噪点。
2. **I2C 驱动不能混用**：`board.c` 的 XL9555 用 legacy `driver/i2c.h`；`esp32-camera` 的 SCCB 走新版 `i2c_master`，两者共存会打印 `i2c: CONFLICT! driver_ng is not allowed to be used with this old driver` 并无限重启。本工程已无相机，不受影响。
3. **SD 卡 CS = GPIO2**：与 LCD 共用 SPI2 的 MOSI11 / SCLK12 / MISO13，只独占总线片选；10 MHz 下读写自检通过。
4. **`EMBED_FILES` 的符号名不带目录前缀**：嵌入 `assets/photo320x240.bin` 后符号是 `_binary_photo320x240_bin_start`。
5. **`esp_flash_get_size`（v5.5.6）要两个参数**：`esp_flash_get_size(NULL, &size)`，且 `main` 需要 `PRIV_REQUIRES spi_flash`。
6. **工程路径变过之后必须 `idf.py fullclean`**，否则 CMake 缓存里的旧绝对路径会报错。

## 主要引脚

| 功能 | 引脚 |
| --- | --- |
| LCD MOSI / SCLK / MISO | GPIO11 / GPIO12 / GPIO13 |
| LCD DC / CS | GPIO40 / GPIO21 |
| LCD 复位 / 背光 | XL9555 P10 (0x0400) / P11 (0x0800) |
| I2C SDA / SCL | GPIO41 / GPIO42 |
| LED | GPIO1（LEDC，低电平点亮） |
| TF 卡 CS | GPIO2 |
| 串口 | CH340，115200 |

## 目录结构

```
.
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig                  # 屏 / PSRAM / 分区等关键配置已就绪
├── firmware-lite.bin          # 预编译固件（无照片页）
└── main
    ├── app_main.c             # 5 页仪表盘 + 按键 + 主循环
    ├── board.c / board.h      # XL9555、屏初始化、按键读取
    ├── ui.c / ui.h            # 绘图原语（文字/行/进度条/清屏）
    ├── font8x16.h             # 8×16 ASCII 字库
    ├── sd_card.c / sd_card.h  # SD 挂载、容量、文件列举、写读自检
    ├── wifi_config.h          # 本地 Wi-Fi 凭据（git 忽略，不入库）
    ├── idf_component.yml      # 组件依赖（本版无 esp32-camera）
    └── CMakeLists.txt
```

## 版本记录

- **v1.0-lite**：5 页（状态 / Wi-Fi / LED / SD / MEM），移除照片页与 `assets/`，去掉 `esp32-camera` 依赖，容量显示统一为 `xx.xGB (xxxxxMB)`。
