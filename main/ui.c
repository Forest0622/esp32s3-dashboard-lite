#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "font8x16.h"

static const char *TAG = "UI";
static uint16_t *s_fb;

esp_err_t ui_init(void)
{
    /* 帧缓冲必须放在内部 RAM 且 DMA 可见：
       SPI 的 DMA 直接从这块内存取数，放 PSRAM 时 CPU 缓存写回与 DMA 读取不同步，
       整屏大图（如第 4 页照片）会出现漏色、竖条纹。内部 RAM 没有这个问题。 */
    s_fb = (uint16_t *)heap_caps_malloc((size_t)LCD_W * LCD_H * sizeof(uint16_t),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_fb == NULL) {
        ESP_LOGW(TAG, "internal dma ram alloc failed, fallback to psram (photo may tear)");
        s_fb = (uint16_t *)heap_caps_malloc((size_t)LCD_W * LCD_H * sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "framebuffer %u bytes @ %p (%s)", (unsigned)(LCD_W * LCD_H * 2), s_fb,
             esp_ptr_internal(s_fb) ? "internal" : "psram");
    ui_clear(COL_BG);
    return ESP_OK;
}

void ui_clear(uint16_t color)
{
    if (s_fb == NULL) {
        return;
    }
    uint32_t n = (uint32_t)LCD_W * LCD_H;
    for (uint32_t i = 0; i < n; i++) {
        s_fb[i] = color;
    }
}

void ui_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (s_fb == NULL) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > LCD_W) {
        x1 = LCD_W;
    }
    if (y1 > LCD_H) {
        y1 = LCD_H;
    }
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = s_fb + (uint32_t)yy * LCD_W;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

void ui_border(int x, int y, int w, int h, uint16_t color, int thickness)
{
    ui_fill_rect(x, y, w, thickness, color);
    ui_fill_rect(x, y + h - thickness, w, thickness, color);
    ui_fill_rect(x, y, thickness, h, color);
    ui_fill_rect(x + w - thickness, y, thickness, h, color);
}

static void draw_glyph(int x, int y, uint8_t scale, uint16_t color, unsigned char ch)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) {
        ch = '?';
    }
    const uint8_t *glyph = s_font8x16[ch - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        if (bits == 0) {
            continue;
        }
        for (int col = 0; col < FONT_W; col++) {
            if ((bits & (0x80 >> col)) == 0) {
                continue;
            }
            int px = x + col * scale;
            int py = y + row * scale;
            if (px < 0 || py < 0 || px >= LCD_W || py >= LCD_H) {
                continue;
            }
            if (scale == 1) {
                s_fb[(uint32_t)py * LCD_W + px] = color;
            } else {
                if (px + scale > LCD_W || py + scale > LCD_H) {
                    continue;
                }
                for (int dy = 0; dy < scale; dy++) {
                    uint16_t *row_p = s_fb + (uint32_t)(py + dy) * LCD_W + px;
                    for (int dx = 0; dx < scale; dx++) {
                        row_p[dx] = color;
                    }
                }
            }
        }
    }
}

void ui_text(int x, int y, uint8_t scale, uint16_t color, const char *s)
{
    if (s_fb == NULL || s == NULL || scale == 0) {
        return;
    }
    int cx = x;
    for (const char *p = s; *p != '\0'; p++) {
        draw_glyph(cx, y, scale, color, (unsigned char)*p);
        cx += FONT_W * scale;
        if (cx > LCD_W) {
            break;
        }
    }
}

void ui_textf(int x, int y, uint8_t scale, uint16_t color, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_text(x, y, scale, color, buf);
}

void ui_progress(int x, int y, int w, int h, int percent, uint16_t fg, uint16_t bg)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    ui_fill_rect(x, y, w, h, bg);
    int filled = (w * percent) / 100;
    if (filled > 0) {
        ui_fill_rect(x, y, filled, h, fg);
    }
}

void ui_blit(int x, int y, int w, int h, const uint16_t *src)
{
    if (s_fb == NULL || src == NULL) {
        return;
    }
    int px0 = x < 0 ? 0 : x;
    int px1 = x + w;
    if (px1 > LCD_W) {
        px1 = LCD_W;
    }
    if (px1 <= px0) {
        return;
    }
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= LCD_H) {
            continue;
        }
        uint16_t *dst = s_fb + (uint32_t)py * LCD_W + px0;
        const uint16_t *src_row = src + (uint32_t)row * w + (px0 - x);
        memcpy(dst, src_row, (size_t)(px1 - px0) * sizeof(uint16_t));
    }
}

uint16_t *ui_framebuffer(void)
{
    return s_fb;
}

void ui_flush(void)
{
    if (s_fb == NULL) {
        return;
    }
    esp_lcd_panel_handle_t panel = board_panel();
    if (panel == NULL) {
        return;
    }
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, s_fb);
    /* 等这一帧真正传完再返回，下一帧改写同一个缓冲区才不会和 DMA 打架 */
    board_lcd_wait_idle(200);
}
