#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

/* 卡挂在 SPI2 上（与 LCD 共用 MOSI=11 / SCLK=12 / MISO=13），只有 CS 是独占的 */
#define SD_MOUNT_POINT  "/sdcard"
#define SD_CS_PIN       2       /* V1.3 实测 TF 卡 CS = GPIO2 */

/* 尝试挂载 SD 卡：成功返回 ESP_OK，并把卡的句柄/容量/文件系统存起来 */
esp_err_t sd_card_mount(void);

bool sd_card_ready(void);

/* 认到卡时使用的 CS 引脚，未认到返回 -1 */
int sd_card_cs(void);

/* 容量与占用（KB），未挂载返回 0 */
uint32_t sd_card_capacity_kb(void);
uint32_t sd_card_free_kb(void);

/* 文件系统类型名（"FAT32" / "FAT16" / "?"），未挂载返回 "-" */
const char *sd_card_fs_name(void);

/* 根目录文件数（只统计普通文件），未挂载返回 -1 */
int sd_card_file_count(void);
