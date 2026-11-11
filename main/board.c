#include "board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_common.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "BOARD";

static esp_lcd_panel_handle_t s_panel;
static uint16_t s_exio_out = 0xFFFF;   /* XL9555 输出寄存器缓存，上电默认全 1 */
static SemaphoreHandle_t s_lcd_done_sem;

/* 一次整帧传输完成的回调：用来让 ui_flush 等到 DMA 真正结束，避免下一帧改写还在发的缓冲区 */
static bool IRAM_ATTR lcd_color_done_cb(esp_lcd_panel_io_handle_t io,
                                        esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp_task_woken = pdFALSE;
    if (s_lcd_done_sem != NULL) {
        xSemaphoreGiveFromISR(s_lcd_done_sem, &hp_task_woken);
    }
    return hp_task_woken == pdTRUE;
}

bool board_lcd_wait_idle(uint32_t timeout_ms)
{
    if (s_lcd_done_sem == NULL) {
        return true;
    }
    return xSemaphoreTake(s_lcd_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* ---------------- XL9555 (I2C IO 扩展，新版 i2c_master 驱动) ---------------- */



static esp_err_t exio_write(uint8_t reg, const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EXIO_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t exio_read(uint8_t reg, uint8_t *data, size_t len)
{
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EXIO_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EXIO_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t exio_pin_write(uint16_t pin, bool level)
{
    if (level) {
        s_exio_out |= pin;
    } else {
        s_exio_out &= (uint16_t)~pin;
    }
    uint8_t data[2] = { (uint8_t)(s_exio_out & 0xFF), (uint8_t)(s_exio_out >> 8) };
    return exio_write(EXIO_REG_OUTPUT_P0, data, 2);
}

static esp_err_t exio_ioconfig(uint16_t config_value)
{
    uint8_t data[2] = { (uint8_t)(config_value & 0xFF), (uint8_t)(config_value >> 8) };
    return exio_write(EXIO_REG_CONFIG_P0, data, 2);
}

static esp_err_t iic_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_IIC_SDA,
        .scl_io_num = PIN_IIC_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t ret = i2c_param_config(I2C_NUM_0, &conf);
    if (ret != ESP_OK) {
        return ret;
    }
    return i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
}

static esp_err_t xl9555_init(void)
{
    /* 上电先读一次，清中断标志 */
    uint8_t r_data[2] = { 0 };
    exio_read(EXIO_REG_INPUT_P0, r_data, 2);

    /* P00/P01/P14~P17 为输入(按键/中断)，其余为输出 */
    esp_err_t ret = exio_ioconfig(0xF003);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "xl9555 config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* 输出全高：背光关、蜂鸣器关、屏复位释放 */
    s_exio_out = 0xFFFF;
    uint8_t data[2] = { 0xFF, 0xFF };
    ret = exio_write(EXIO_REG_OUTPUT_P0, data, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "xl9555 output init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "XL9555 ready @0x20, keys=0x%04X", board_keys_raw());
    return ESP_OK;
}

/* ---------------- LCD (SPI2 + ST7789V2 320x240) ---------------- */

static esp_err_t lcd_hw_reset(void)
{
    esp_err_t ret = exio_pin_write(EXIO_SLCD_RST_IO, false);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    ret = exio_pin_write(EXIO_SLCD_RST_IO, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ret;
}

static esp_err_t lcd_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    s_lcd_done_sem = xSemaphoreCreateBinary();
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = lcd_color_done_cb,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "panel io init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,          /* 复位由 XL9555 P12 控制 */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        /* 帧缓冲是原生 uint16（低字节在前），必须让 ST7789 也按小端收数据；
           默认的 BIG 会把每个像素的高/低字节对调 -> 照片/UI 全部偏色 */
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "st7789 panel create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = lcd_hw_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lcd hw reset failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));      /* 240x320 -> 320x240 横屏 */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_LOGI(TAG, "ST7789V2 %dx%d ready", LCD_W, LCD_H);
    return ESP_OK;
}

/* ---------------- LED / 蜂鸣器 ---------------- */

static esp_err_t led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }
    gpio_set_level(PIN_LED, 0);

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ret = ledc_timer_config(&timer);
    if (ret != ESP_OK) {
        return ret;
    }
    ledc_channel_config_t ch = {
        .gpio_num = PIN_LED,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&ch);
}

/* ---------------- 对外接口 ---------------- */

esp_err_t board_init(void)
{
    esp_err_t ret = iic_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = led_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = lcd_init();
    if (ret != ESP_OK) {
        return ret;
    }
    return ESP_OK;
}

esp_lcd_panel_handle_t board_panel(void)
{
    return s_panel;
}

esp_err_t board_backlight(bool on)
{
    return exio_pin_write(EXIO_SLCD_PWR_IO, on);
}

esp_err_t board_led(bool on)
{
    return board_led_brightness(on ? 100 : 0);
}

esp_err_t board_led_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint8_t)((255 * percent) / 100)) == ESP_OK
               ? ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0)
               : ESP_FAIL;
}

esp_err_t board_exio_write(uint16_t pin, bool level)
{
    return exio_pin_write(pin, level);
}

esp_err_t board_beep(bool on)
{
    return exio_pin_write(EXIO_BEEP_IO, !on);   /* 低电平响 */
}

uint16_t board_keys_raw(void)
{
    uint8_t data[2] = { 0xFF, 0xFF };
    if (exio_read(EXIO_REG_INPUT_P0, data, 2) != ESP_OK) {
        return 0xFFFF;
    }
    return ((uint16_t)data[1] << 8) | data[0];
}

void board_dump_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "chip: ESP32-S3 rev v%d.%d, %d core(s)", info.revision / 100, info.revision % 100, info.cores);
    ESP_LOGI(TAG, "mac: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "flash: %u MB", (unsigned)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "heap: internal %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "heap: psram %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
