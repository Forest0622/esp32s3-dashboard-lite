#include "sd_card.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "SDCARD";

static sdmmc_card_t *s_card;
static bool s_ready;
static int s_cs_pin = -1;
static const char *s_fs_name = "-";

/* 候选 CS 引脚：先试最常见的 GPIO10，再遍历其余空闲脚。
   故意避开已被占用的脚：1(LED) 11/12/13(SPI2 数据) 21/40(LCD) 41/42(I2C)
   43/44(UART0) 19/20(USB) 26~32(PSRAM) 33~37(flash/PSRAM) 0/45/46(strapping) */
static const int k_cs_candidates[] = {
    SD_CS_PIN, 10, 4, 5, 3, 6, 7, 8, 9, 14, 15, 16, 17, 18, 38, 39, 47, 48,
};

bool sd_card_ready(void)
{
    return s_ready;
}

int sd_card_cs(void)
{
    return s_cs_pin;
}

uint32_t sd_card_capacity_kb(void)
{
    if (!s_ready || s_card == NULL) {
        return 0;
    }
    return (uint32_t)(((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / 1024);
}

uint32_t sd_card_free_kb(void)
{
    if (!s_ready) {
        return 0;
    }
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
        return 0;
    }
    return (uint32_t)(free_bytes / 1024);
}

const char *sd_card_fs_name(void)
{
    return s_fs_name;
}

int sd_card_file_count(void)
{
    if (!s_ready) {
        return -1;
    }
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (dir == NULL) {
        return -1;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG || ent->d_type == DT_UNKNOWN) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static esp_err_t mount_one(int cs)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;          /* 复用 LCD 已经初始化好的 SPI2 总线 */
    host.max_freq_khz = 10000;      /* 10MHz，杜邦线/长走线更稳 */

    sdspi_device_config_t slot = {
        .host_id = SPI2_HOST,
        .gpio_cs = cs,
        .gpio_cd = SDSPI_SLOT_NO_CD,
        .gpio_wp = SDSPI_SLOT_NO_WP,
        .gpio_int = SDSPI_SLOT_NO_INT,
    };

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 4 * 1024,
    };

    return esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
}

static bool write_read_test(void)
{
    const char path[] = SD_MOUNT_POINT "/sddemo.txt";
    const char *payload = "hello from esp32s3";

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "write test: fopen failed");
        return false;
    }
    fputs(payload, f);
    fclose(f);

    char back[32] = {0};
    f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "write test: reopen failed");
        return false;
    }
    fread(back, 1, sizeof(back) - 1, f);
    fclose(f);
    remove(path);

    bool ok = strcmp(back, payload) == 0;
    ESP_LOGI(TAG, "write+read test %s (\"%s\")", ok ? "PASS" : "FAIL", back);
    return ok;
}

static void list_root(void)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir failed");
        return;
    }
    struct dirent *ent;
    int shown = 0;
    while ((ent = readdir(dir)) != NULL && shown < 6) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        ESP_LOGI(TAG, "  /sdcard/%s", ent->d_name);
        shown++;
    }
    closedir(dir);
}

static void fill_fs_name(void)
{
    /* FAT 卷的具体类型（FAT16/FAT32/ExFAT）驱动没直接暴露，UI 上统一显示 FAT 即可 */
    s_fs_name = "FAT";
}

esp_err_t sd_card_mount(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    for (size_t i = 0; i < sizeof(k_cs_candidates) / sizeof(k_cs_candidates[0]); i++) {
        int cs = k_cs_candidates[i];
        esp_err_t ret = mount_one(cs);
        if (ret == ESP_OK) {
            s_ready = true;
            s_cs_pin = cs;
            fill_fs_name();
            ESP_LOGI(TAG, "mounted on CS=GPIO%d, %s %u MB, %u KB free, %d file(s)",
                     cs, s_card->is_mmc ? "MMC" : "SD",
                     (unsigned)(sd_card_capacity_kb() / 1024), (unsigned)sd_card_free_kb(),
                     sd_card_file_count());
            ESP_LOGI(TAG, "name=%s speed=%d kHz sectors=%u", s_card->cid.name,
                     s_card->real_freq_khz, (unsigned)s_card->csd.sector_size);
            list_root();
            write_read_test();
            return ESP_OK;
        }
        ESP_LOGD(TAG, "CS=GPIO%d -> %s", cs, esp_err_to_name(ret));
    }

    ESP_LOGW(TAG, "no SD card found on any candidate CS pin");
    return ESP_ERR_NOT_FOUND;
}
