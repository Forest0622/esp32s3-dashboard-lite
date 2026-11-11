#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sd_card.h"
#include "mirror.h"
#include "ui.h"

static const char *TAG = "DASH";

#define MAX_APS 16
#define PAGE_COUNT 5

enum { PAGE_STATUS = 0, PAGE_WIFI, PAGE_LED, PAGE_SD, PAGE_MEM };


/* Wi-Fi 连接信息（改这里即可换网络） */
/* Wi-Fi 凭据放在 main/wifi_config.h（已被 .gitignore 忽略，不进仓库）。
   该文件不存在时用下面的占位符编译，连不上只是显示 OFFLINE，不影响其他页面。 */
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"
#endif

enum { LK_IDLE = 0, LK_CONNECTING, LK_GOT_IP, LK_FAILED };

static esp_netif_t *s_netif;
static volatile int s_link_state = LK_IDLE;
static char s_ip[20] = "0.0.0.0";
static int s_retry;
static int s_conn_rssi;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_ap_record_t s_aps[MAX_APS];
static uint16_t s_ap_count;
static bool s_scanning;
static volatile bool s_scan_request = true;

static int s_page = PAGE_STATUS;
static int s_led_mode;              /* 0 = ON, 1 = OFF, 2 = BREATH */
static uint8_t s_led_duty;
static uint16_t s_last_keys = 0xFFFF;
static int64_t s_last_key_ms;
static char s_mac[18];

static const char *LED_MODE_NAME[] = { "ON", "OFF", "BREATH" };
#define LED_MODE_COUNT 3

/* ---- 家里的 AP（由用户提供） ---- */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---------------- Wi-Fi ---------------- */

static void wifi_scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    while (1) {
        if (s_scan_request && s_link_state != LK_CONNECTING) {
            s_scan_request = false;
            s_scanning = true;

            esp_err_t ret = esp_wifi_scan_start(&cfg, true);
            if (ret == ESP_OK) {
                uint16_t num = 0;
                esp_wifi_scan_get_ap_num(&num);
                if (num > MAX_APS) {
                    num = MAX_APS;
                }
                wifi_ap_record_t tmp[MAX_APS];
                uint16_t got = num;
                ret = esp_wifi_scan_get_ap_records(&got, tmp);
                if (ret == ESP_OK) {
                    portENTER_CRITICAL(&s_lock);
                    memcpy(s_aps, tmp, sizeof(wifi_ap_record_t) * got);
                    s_ap_count = got;
                    portEXIT_CRITICAL(&s_lock);
                    ESP_LOGI(TAG, "scan done: %u AP(s)", (unsigned)got);
                }
            } else {
                ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(ret));
                if (ret == ESP_ERR_WIFI_STATE) {
                    s_scan_request = true;   /* 正在连接，稍后重试 */
                }
            }
            s_scanning = false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_link_state = LK_CONNECTING;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < 20) {
            s_retry++;
            s_link_state = LK_CONNECTING;
            esp_wifi_connect();
        } else {
            s_link_state = LK_FAILED;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        s_link_state = LK_GOT_IP;
        ESP_LOGI(TAG, "got ip: %s", s_ip);
    }
}

static esp_err_t wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t sta_cfg = { 0 };
    strncpy((char *)sta_cfg.sta.ssid, WIFI_SSID, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, WIFI_PASS, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    sta_cfg.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi station started, target ssid=%s", WIFI_SSID);
    return ESP_OK;
}

static void update_link_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_conn_rssi = ap.rssi;
    }
}

static uint16_t ap_count_snapshot(wifi_ap_record_t *out, uint16_t max)
{
    portENTER_CRITICAL(&s_lock);
    uint16_t n = s_ap_count;
    if (n > max) {
        n = max;
    }
    memcpy(out, s_aps, sizeof(wifi_ap_record_t) * n);
    portEXIT_CRITICAL(&s_lock);
    return n;
}

/* ---------------- UI ---------------- */

static void draw_header(const char *title)
{
    ui_fill_rect(0, 0, LCD_W, 30, COL_BAR);
    ui_fill_rect(0, 29, LCD_W, 1, COL_CYAN);
    ui_text(8, 7, 1, COL_CYAN, title);
    ui_textf(LCD_W - 56, 7, 1, COL_GREY, "%d/%d", s_page + 1, PAGE_COUNT);
}

static void draw_footer(void)
{
    ui_fill_rect(0, LCD_H - 20, LCD_W, 20, COL_BAR);
    ui_fill_rect(0, LCD_H - 20, LCD_W, 1, COL_CYAN);
    ui_text(8, LCD_H - 16, 1, COL_GREY, "K0:PAGE  K1:LED  K2:SCAN");
}

static void draw_row(int y, const char *label, const char *value, uint16_t value_color)
{
    ui_text(10, y, 1, COL_GREY, label);
    ui_text(74, y, 1, value_color, value);
}

static void page_status(void)
{
    draw_header("SYSTEM STATUS");

    int y = 40;
    char buf[48];

    draw_row(y, "CHIP", "ESP32-S3 2C 240MHz", COL_WHITE);
    y += 20;
    draw_row(y, "MAC", s_mac, COL_WHITE);
    y += 20;
    draw_row(y, "IDF", "ESP-IDF v5.5.6", COL_WHITE);
    y += 20;

    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             (int)(now_ms() / 3600000), (int)((now_ms() / 60000) % 60), (int)((now_ms() / 1000) % 60));
    draw_row(y, "UPTIME", buf, COL_WHITE);
    y += 20;

    snprintf(buf, sizeof(buf), "%u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    draw_row(y, "HEAP", buf, COL_GREEN);
    y += 20;

    snprintf(buf, sizeof(buf), "%u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    draw_row(y, "PSRAM", buf, COL_GREEN);
    y += 20;

    snprintf(buf, sizeof(buf), "%s (GPIO1)", LED_MODE_NAME[s_led_mode]);
    draw_row(y, "LED", buf, s_led_mode == 1 ? COL_GREY : COL_AMBER);
    y += 20;

    switch (s_link_state) {
    case LK_GOT_IP:
        snprintf(buf, sizeof(buf), "ONLINE %s", s_ip);
        draw_row(y, "WIFI", buf, COL_GREEN);
        break;
    case LK_CONNECTING:
        snprintf(buf, sizeof(buf), "CONNECTING %d", s_retry + 1);
        draw_row(y, "WIFI", buf, COL_AMBER);
        break;
    case LK_FAILED:
        draw_row(y, "WIFI", "FAILED, press K2", COL_RED);
        break;
    default:
        draw_row(y, "WIFI", "starting...", COL_GREY);
        break;
    }
    y += 20;

    wifi_ap_record_t aps[MAX_APS];
    uint16_t n = ap_count_snapshot(aps, MAX_APS);
    snprintf(buf, sizeof(buf), "%u AP%s %s", (unsigned)n, n == 1 ? "" : "s", s_scanning ? "scanning..." : "listed");
    draw_row(y, "APS", buf, COL_CYAN);

    draw_footer();
}

static void page_wifi(void)
{
    draw_header("WIFI SCAN");

    wifi_ap_record_t aps[MAX_APS];
    uint16_t n = ap_count_snapshot(aps, MAX_APS);

    if (s_link_state == LK_GOT_IP) {
        ui_textf(8, 36, 1, COL_GREEN, "%s  %s  %ddBm", WIFI_SSID, s_ip, s_conn_rssi);
        ui_textf(LCD_W - 88, 36, 1, COL_GREY, "%u AP", (unsigned)n);
    } else {
        ui_textf(8, 36, 1, COL_CYAN, "FOUND %u AP%s%s", (unsigned)n, n == 1 ? "" : "s",
                 s_scanning ? "  SCANNING..." : "");
    }

    int y = 56;
    for (uint16_t i = 0; i < n && i < 6; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        char name[41];
        snprintf(name, sizeof(name), "%02u %s", (unsigned)(i + 1), ssid[0] ? ssid : "<hidden>");

        int rssi = aps[i].rssi;
        int pct = (rssi + 100) * 100 / 70;   /* -100dBm..-30dBm -> 0..100 */
        if (pct < 0) {
            pct = 0;
        }
        if (pct > 100) {
            pct = 100;
        }
        uint16_t col = rssi > -55 ? COL_GREEN : (rssi > -75 ? COL_AMBER : COL_RED);

        ui_text(8, y, 1, COL_WHITE, name);
        ui_textf(LCD_W - 72, y, 1, col, "%ddBm", rssi);
        ui_progress(8, y + 17, LCD_W - 16, 4, pct, col, COL_CARD);
        y += 26;
    }

    if (n == 0) {
        ui_text(8, 80, 1, COL_GREY, s_scanning ? "scanning, please wait ..." : "no AP found, press K2");
    }

    draw_footer();
}

static void page_led(void)
{
    draw_header("LED / BOARD IO");

    int cx = LCD_W / 2;
    uint16_t lamp = COL_GREY;
    if (s_led_mode == 1) {
        lamp = COL_CARD;
    } else if (s_led_mode == 2) {
        lamp = (s_led_duty > 96) ? COL_AMBER : COL_GREEN;
    } else {
        lamp = COL_AMBER;
    }
    ui_border(cx - 44, 46, 88, 58, COL_CYAN, 2);
    ui_fill_rect(cx - 40, 50, 80, 50, lamp);

    ui_textf(cx - 40, 112, 2, COL_WHITE, "%s", LED_MODE_NAME[s_led_mode]);
    ui_textf(cx - 32, 150, 1, COL_GREY, "GPIO %d  DUTY %u%%", PIN_LED, (unsigned)s_led_duty);
    ui_progress(cx - 80, 172, 160, 8, s_led_mode == 1 ? 0 : (s_led_mode == 2 ? s_led_duty : 100), COL_AMBER, COL_CARD);

    ui_text(8, 190, 1, COL_GREY, "XL9555: 0x20  KEYS  P14-P17");
    ui_text(8, 204, 1, COL_GREY, "LED PWM 2kHz 8bit  BUZZ P03");

    draw_footer();
}

/* 内存容量 + 外接存储状态：RAM / PSRAM / FLASH / APP 分区 / SD 卡 */
/* 容量统一格式：29.1GB (29818MB)，按二进制 1024 换算 */
static void fmt_gb_mb(char *out, size_t n, uint32_t kb)
{
    uint32_t mb = kb / 1024;
    uint32_t gb10 = (mb * 10) / 1024;      /* GB 的十倍值，用于保留一位小数 */
    snprintf(out, n, "%u.%uGB (%uMB)", (unsigned)(gb10 / 10), (unsigned)(gb10 % 10), (unsigned)mb);
}

static void page_mem(void)
{
    draw_header("MEM / STORAGE");

    char buf[48];
    int y = 40;

    snprintf(buf, sizeof(buf), "%u / %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024));
    draw_row(y, "RAM", buf, COL_GREEN);
    y += 20;

    snprintf(buf, sizeof(buf), "%u / %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    draw_row(y, "PSRAM", buf, COL_GREEN);
    y += 20;

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    snprintf(buf, sizeof(buf), "%u MB QIO 80MHz", (unsigned)(flash_size / (1024 * 1024)));
    draw_row(y, "FLASH", buf, COL_WHITE);
    y += 20;

    const esp_partition_t *app = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                          ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (app != NULL) {
        snprintf(buf, sizeof(buf), "0x%06X  %u KB", (unsigned)app->address,
                 (unsigned)(app->size / 1024));
        draw_row(y, "APP PART", buf, COL_WHITE);
    }
    y += 20;

    if (sd_card_ready()) {
        char cap[32], fre[32];
        fmt_gb_mb(cap, sizeof(cap), sd_card_capacity_kb());
        fmt_gb_mb(fre, sizeof(fre), sd_card_free_kb());
        snprintf(buf, sizeof(buf), "%s %s", cap, sd_card_fs_name());
        draw_row(y, "SD CARD", buf, COL_GREEN);
        y += 20;
        snprintf(buf, sizeof(buf), "%s free %d f", fre, sd_card_file_count());
        draw_row(y, "SD FREE", buf, COL_GREEN);
        y += 20;
        snprintf(buf, sizeof(buf), "CS GPIO%d 10MHz /sdcard", sd_card_cs());
        draw_row(y, "SD BUS", buf, COL_GREY);
    } else {
        draw_row(y, "SD CARD", "NOT INSERTED", COL_RED);
        y += 20;
        draw_row(y, "SD FREE", "--", COL_GREY);
        y += 20;
        draw_row(y, "SD BUS", "CS GPIO2 10MHz", COL_GREY);
    }

    draw_footer();
}

static void page_sd(void)
{
    draw_header("SD CARD");

    char buf[48];
    int y = 44;

    if (sd_card_ready()) {
        char cap[32];
        fmt_gb_mb(cap, sizeof(cap), sd_card_capacity_kb());
        snprintf(buf, sizeof(buf), "%s  %s", sd_card_fs_name(), cap);
        draw_row(y, "CARD", buf, COL_GREEN);
        y += 20;

        char fre[32];
        fmt_gb_mb(fre, sizeof(fre), sd_card_free_kb());
        snprintf(buf, sizeof(buf), "%s free", fre);
        draw_row(y, "FREE", buf, COL_WHITE);
        y += 20;

        snprintf(buf, sizeof(buf), "CS = GPIO%d", sd_card_cs());
        draw_row(y, "SPI2", buf, COL_CYAN);
        y += 20;

        int files = sd_card_file_count();
        snprintf(buf, sizeof(buf), "%d file(s) on /sdcard", files);
        draw_row(y, "FILES", buf, COL_WHITE);
        y += 20;

        draw_row(y, "MOUNT", "OK", COL_GREEN);
    } else {
        draw_row(y, "CARD", "NOT DETECTED", COL_RED);
        y += 20;
        draw_row(y, "SPI2", "MOSI11 SCLK12 MISO13", COL_GREY);
        y += 20;
        draw_row(y, "CS", "GPIO10 tried + 17 pins", COL_GREY);
        y += 20;
        ui_text(16, y + 8, 1, COL_AMBER, "insert card, then reboot");
        y += 20;
        ui_text(16, y + 8, 1, COL_GREY, "log: SDCARD mount result");
    }

    draw_footer();
}

static void render(void)
{
    ui_clear(COL_BG);
    switch (s_page) {
    case PAGE_WIFI:
        page_wifi();
        break;
    case PAGE_LED:
        page_led();
        break;
    case PAGE_SD:
        page_sd();
        break;
    case PAGE_MEM:
        page_mem();
        break;
    case PAGE_STATUS:
    default:
        page_status();
        break;
    }
    ui_flush();
}

/* ---------------- 按键 / LED ---------------- */

static void handle_keys(void)
{
    uint16_t now_keys = board_keys_raw() & 0xF000;                 /* 只用 P14~P17 四个按键 */
    uint16_t pressed = (uint16_t)(~now_keys) & s_last_keys & 0xF000; /* 上一次高、这次低 = 新按下 */
    s_last_keys = now_keys;

    if (pressed == 0) {
        return;
    }
    if (now_ms() - s_last_key_ms < 60) {   /* 简单去抖 */
        return;
    }
    s_last_key_ms = now_ms();

    if (pressed & EXIO_KEY0_IO) {
        s_page = (s_page + 1) % PAGE_COUNT;
    } else if (pressed & EXIO_KEY3_IO) {
        s_page = (s_page + PAGE_COUNT - 1) % PAGE_COUNT;
    } else if (pressed & EXIO_KEY1_IO) {
        s_led_mode = (s_led_mode + 1) % LED_MODE_COUNT;
        if (s_led_mode == 1) {
            board_led_brightness(0);
            s_led_duty = 0;
        } else if (s_led_mode == 0) {
            board_led_brightness(100);
            s_led_duty = 100;
        }
    } else if (pressed & EXIO_KEY2_IO) {
        s_scan_request = true;
        if (s_link_state == LK_FAILED) {   /* 连接失败时按 K2 重新连 */
            s_retry = 0;
            s_link_state = LK_CONNECTING;
            esp_wifi_connect();
        }
    }
    board_beep(true);
    vTaskDelay(pdMS_TO_TICKS(25));
    board_beep(false);
    ESP_LOGI(TAG, "key 0x%04X -> page %d, led mode %d", pressed, s_page, s_led_mode);
}

static void breath_step(void)
{
    static int64_t last = 0;
    static int dir = 1;
    int64_t t = now_ms();
    if (t - last < 25) {
        return;
    }
    last = t;
    int v = (int)s_led_duty + dir * 3;
    if (v >= 100) {
        v = 100;
        dir = -1;
    } else if (v <= 0) {
        v = 0;
        dir = 1;
    }
    s_led_duty = (uint8_t)v;
    board_led_brightness(s_led_duty);
}

/* ---------------- 入口 ---------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "ATK-DNESP32S3 V1.3 dashboard booting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(board_init());
    board_dump_info();

    /* TF 卡：与 LCD 共用 SPI2，只独占 CS；没插卡不影响其它功能 */
    if (sd_card_mount() != ESP_OK) {
        ESP_LOGW(TAG, "no TF card detected");
    }

    ESP_ERROR_CHECK(ui_init());
    ui_clear(COL_BG);
    ui_text(40, 100, 2, COL_CYAN, "ATK-DNESP32S3");
    ui_text(72, 140, 1, COL_GREY, "display self test ...");
    ui_fill_rect(40, 170, 240, 3, COL_CYAN);
    ui_flush();
    board_backlight(true);
    ESP_LOGI(TAG, "backlight on, splash shown");

    ESP_ERROR_CHECK(wifi_start());

    s_led_mode = 0;      /* 开机常亮，K1 循环 ON -> OFF -> BREATH */
    s_led_duty = 100;
    board_led_brightness(100);

    xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 4, NULL);

    mirror_start();     /* 屏幕镜像：TCP 3232，见 mirror.c */

    int64_t last_tick = now_ms();
    int64_t last_render = 0;
    int last_page = -1;

    while (1) {
        handle_keys();
        if (s_led_mode == 2) {
            breath_step();
        }
        int64_t t = now_ms();

        if (s_page != last_page) {          /* 刚切页，立刻重画 */
            last_page = s_page;
            last_render = 0;
        }

        if (t - last_render > 250 || t - last_tick > 5000) {
            update_link_rssi();
            render();
            mirror_notify();
            last_render = t;
        }
        if (t - last_tick > 60000) {          /* 每 60s 自动重扫一次 */
            last_tick = t;
            s_scan_request = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
