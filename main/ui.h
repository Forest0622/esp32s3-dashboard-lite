#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
/* RGB565 颜色 */
#define UI_RGB(r, g, b) ((uint16_t)((((r) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((b) & 0x1F)))

#define COL_BG        UI_RGB(4, 8, 16)
#define COL_CARD      UI_RGB(10, 18, 32)
#define COL_BAR       UI_RGB(6, 16, 34)
#define COL_CYAN      UI_RGB(12, 60, 28)
#define COL_WHITE     UI_RGB(31, 63, 31)
#define COL_GREY      UI_RGB(18, 34, 18)
#define COL_GREEN     UI_RGB(6, 60, 14)
#define COL_AMBER     UI_RGB(31, 52, 4)
#define COL_RED       UI_RGB(31, 6, 6)
#define COL_BLUE      UI_RGB(4, 26, 31)

esp_err_t ui_init(void);

void ui_clear(uint16_t color);
void ui_fill_rect(int x, int y, int w, int h, uint16_t color);
void ui_border(int x, int y, int w, int h, uint16_t color, int thickness);
void ui_text(int x, int y, uint8_t scale, uint16_t color, const char *s);
void ui_textf(int x, int y, uint8_t scale, uint16_t color, const char *fmt, ...);
void ui_progress(int x, int y, int w, int h, int percent, uint16_t fg, uint16_t bg);

/* 贴一张 RGB565 图片（源数据按行紧密排列），用于第 4 页照片 */
void ui_blit(int x, int y, int w, int h, const uint16_t *src);

/* 取帧缓冲指针（给 img 驱动做缩放绘制用，外部不要长期持有） */
uint16_t *ui_framebuffer(void);

/* 把整帧推送到屏幕 */
void ui_flush(void);
