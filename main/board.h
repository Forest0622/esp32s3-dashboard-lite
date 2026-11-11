#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* ---- 正点原子 ATK-DNESP32S3 V1.3 板级引脚（官方 BSP 实测值） ---- */
#define PIN_LED        1      /* 板载 LED，高电平点亮 */
#define PIN_LCD_MOSI   11     /* SPI2 MOSI */
#define PIN_LCD_SCLK   12     /* SPI2 SCLK */
#define PIN_LCD_MISO   13     /* 屏不接 MISO，仅占用总线 */
#define PIN_LCD_DC     40     /* LCD WR / DC */
#define PIN_LCD_CS     21     /* LCD CS */

#define PIN_IIC_SDA    41     /* IIC0 SDA */
#define PIN_IIC_SCL    42     /* IIC0 SCL */

/* XL9555 扩展 IO（7 位地址 0x20） */
#define EXIO_ADDR              0x20
#define EXIO_REG_INPUT_P0      0x00
#define EXIO_REG_OUTPUT_P0     0x02
#define EXIO_REG_CONFIG_P0     0x06

#define EXIO_BEEP_IO           0x0008  /* P03 蜂鸣器，低电平响 */
#define EXIO_SLCD_RST_IO       0x0400  /* P12 屏复位 */
#define EXIO_SLCD_PWR_IO       0x0800  /* P13 屏背光，高电平点亮 */
#define EXIO_KEY3_IO           0x1000  /* P14 */
#define EXIO_KEY2_IO           0x2000  /* P15 */
#define EXIO_KEY1_IO           0x4000  /* P16 */
#define EXIO_KEY0_IO           0x8000  /* P17 */
#define EXIO_CAM_PWDN_IO       0x0010  /* P04 摄像头 PWDN，低电平上电 */
#define EXIO_CAM_RST_IO        0x0020  /* P05 摄像头 RESET，低电平复位 */

#define LCD_W                  320
#define LCD_H                  240

/* 板级初始化：IIC -> XL9555 -> SPI2 -> ST7789 -> LED/背光 */
esp_err_t board_init(void);

/* 已初始化好的 LCD 句柄 */
esp_lcd_panel_handle_t board_panel(void);

/* 写 XL9555 单个扩展 IO（摄像头 PWDN/RESET 用） */
esp_err_t board_exio_write(uint16_t pin, bool level);

/* 等上一帧 SPI/DMA 传输真正结束（超时返回 false），避免改写还在发送的帧缓冲 */
bool board_lcd_wait_idle(uint32_t timeout_ms);

/* 背光开关 */
esp_err_t board_backlight(bool on);

/* LED：on/off 与呼吸灯占空比(0~100) */
esp_err_t board_led(bool on);
esp_err_t board_led_brightness(uint8_t percent);

/* 蜂鸣器 */
esp_err_t board_beep(bool on);

/* 读取 XL9555 输入寄存器（16 位，按键为低电平有效） */
uint16_t board_keys_raw(void);

/* 打印板级状态，供串口确认 */
void board_dump_info(void);
