#pragma once

/* 屏幕镜像服务：把 320x240 RGB565 帧缓冲经 Wi-Fi TCP 推给局域网内的 Mac/PC 查看器。
 * 端口 3232，协议见 mirror.c 顶部注释。未连接时不产生额外流量。 */
void mirror_start(void);

/* 主循环在一次 render() 之后调用：标记"有新画面"。
 * 只置标志，不阻塞；真正的编码与发送在独立任务里做。 */
void mirror_notify(void);
