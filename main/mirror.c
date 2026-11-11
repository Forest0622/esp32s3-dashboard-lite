/* 屏幕镜像：把 ui 的整帧 RGB565 缓冲经 Wi-Fi TCP 推给局域网内的电脑。
 *
 * 协议（little endian）
 *   连接建立后服务器先发 10 字节头：
 *     'M','I','R','1' | u16 width | u16 height | u8 bpp(=2) | u8 fmt(1=RLE)
 *   之后每帧：
 *     u32 payload_len | payload
 *   payload = 若干组 (u16 count, u16 rgb565)，按行优先顺序铺满 width*height 像素，
 *   count 为 1..65535 的连续同色像素个数。
 *
 * 屏幕本身 250ms 刷新一次，所以镜像帧率上限 4fps，流量经过 RLE 压缩后通常 10~40KB/帧。
 * 帧缓冲在内部 DMA RAM（CPU 可读），编码缓冲放 PSRAM，避免挤占内部堆。
 */
#include "mirror.h"

#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "lwip/sockets.h"
#include "ui.h"

static const char *TAG = "MIRROR";

#define MIRROR_PORT       3232
#define MIRROR_MAX_ENC    (192 * 1024)   /* RLE 输出上限，最坏情况 4 字节/像素也不到 300KB */
#define MIRROR_TASK_STACK (4096)

static uint8_t *s_enc;                        /* RLE 编码缓冲（PSRAM） */
static volatile bool s_dirty;                 /* 有新画面待发 */
static volatile bool s_client;                /* 有客户端连着（仅用于日志/统计） */

/* 把一帧 RGB565 做 RLE 编码，返回字节数；超上限返回 0 */
static size_t encode_rle(const uint16_t *fb, uint8_t *out, size_t cap)
{
    size_t n = 0;
    uint32_t total = (uint32_t)LCD_W * LCD_H;
    uint32_t i = 0;

    while (i < total) {
        uint16_t color = fb[i];
        uint32_t run = 1;
        while (i + run < total && fb[i + run] == color && run < 0xFFFF) {
            run++;
        }
        if (n + 4 > cap) {
            return 0;
        }
        out[n++] = (uint8_t)(run & 0xFF);
        out[n++] = (uint8_t)(run >> 8);
        out[n++] = (uint8_t)(color & 0xFF);
        out[n++] = (uint8_t)(color >> 8);
        i += run;
    }
    return n;
}

static bool send_all(int fd, const uint8_t *p, size_t len)
{
    while (len > 0) {
        ssize_t w = send(fd, p, len, 0);
        if (w <= 0) {
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

/* 取每 8 个像素做一次折叠，得到一个廉价的"画面有没有变"指纹 */
static uint32_t frame_fingerprint(const uint16_t *fb)
{
    uint32_t total = (uint32_t)LCD_W * LCD_H;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < total; i += 8) {
        h = (h ^ fb[i]) * 16777619u;
    }
    return h;
}

static void mirror_task(void *arg)
{
    (void)arg;

    int listen_fd = -1;
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(MIRROR_PORT);

    while (1) {
        if (listen_fd < 0) {
            listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (listen_fd < 0) {
                ESP_LOGE(TAG, "socket failed errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            int one = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                ESP_LOGE(TAG, "bind %d failed errno=%d", MIRROR_PORT, errno);
                close(listen_fd);
                listen_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            if (listen(listen_fd, 2) != 0) {
                ESP_LOGE(TAG, "listen failed errno=%d", errno);
                close(listen_fd);
                listen_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            ESP_LOGI(TAG, "screen mirror listening on tcp/%d", MIRROR_PORT);
        }

        struct sockaddr_in client = { 0 };
        socklen_t clen = sizeof(client);
        int fd = accept(listen_fd, (struct sockaddr *)&client, &clen);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        s_client = true;
        ESP_LOGI(TAG, "client %s:%d connected", inet_ntoa(client.sin_addr), ntohs(client.sin_port));

        uint8_t hdr[10] = { 'M', 'I', 'R', '1',
                            (uint8_t)(LCD_W & 0xFF), (uint8_t)((LCD_W >> 8) & 0xFF),
                            (uint8_t)(LCD_H & 0xFF), (uint8_t)((LCD_H >> 8) & 0xFF),
                            2, 1 };
        if (!send_all(fd, hdr, sizeof(hdr))) {
            close(fd);
            s_client = false;
            continue;
        }

        uint32_t last_fp = 0;
        uint32_t frames = 0, bytes = 0;
        int64_t last_send_ms = 0;
        bool first = true;

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(20));

            int64_t now = esp_timer_get_time() / 1000;
            /* 静态画面下每 5s 补一帧：既能让新连上的客户端立刻看到内容，
               也让查看端不必靠超时判断"还活着" */
            bool heartbeat = (now - last_send_ms) >= 5000;
            bool changed = s_dirty;
            if (!changed && !heartbeat) {
                continue;
            }
            s_dirty = false;

            const uint16_t *fb = ui_framebuffer();
            if (fb == NULL || s_enc == NULL) {
                continue;
            }
            uint32_t fp = frame_fingerprint(fb);
            if (!first && !heartbeat && fp == last_fp) {
                continue;                    /* 画面没变，省流量 */
            }
            first = false;
            last_fp = fp;

            size_t len = encode_rle(fb, s_enc, MIRROR_MAX_ENC);
            if (len == 0) {
                ESP_LOGW(TAG, "rle overflow, frame skipped");
                continue;
            }
            uint8_t lenbuf[4] = { (uint8_t)len, (uint8_t)(len >> 8),
                                  (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
            if (!send_all(fd, lenbuf, 4) || !send_all(fd, s_enc, len)) {
                ESP_LOGW(TAG, "send failed, client dropped");
                break;
            }
            frames++;
            bytes += (uint32_t)len + 4;
            last_send_ms = now;
            if (frames % 20 == 0) {
                ESP_LOGI(TAG, "mirror %u frames, %u KB, last %u bytes",
                         (unsigned)frames, (unsigned)(bytes / 1024), (unsigned)len);
            }
        }

        close(fd);
        s_client = false;
        ESP_LOGI(TAG, "client disconnected after %u frames", (unsigned)frames);
    }
}

void mirror_start(void)
{
    s_enc = (uint8_t *)heap_caps_malloc(MIRROR_MAX_ENC, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_enc == NULL) {
        s_enc = (uint8_t *)heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_enc == NULL) {
        ESP_LOGE(TAG, "no psram for mirror buffer, mirror disabled");
        return;
    }
    xTaskCreate(mirror_task, "mirror", MIRROR_TASK_STACK, NULL, 3, NULL);
}

void mirror_notify(void)
{
    if (!s_client) {
        return;                              /* 没人连就别触发编码 */
    }
    s_dirty = true;
}
