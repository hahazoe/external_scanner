/*
 * 板间链路接口（ESP32-S3 侧）
 * - I2S 链路A：S3 主机 TX → ESP32 从机 RX，下行 PC 扬声器 PCM（48k/16bit/立体声）
 * - I2S 链路B：ESP32 主机 TX → S3 从机 RX，上行 AirPods 麦克风 PCM（48k/16bit/立体声）
 * - UART 控制通道：模式切换 / 音量 / 状态（115200 8N1）
 */
#ifndef AUDIO_LINK_H_
#define AUDIO_LINK_H_

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// 链路B 接收流缓冲至少保留的毫秒数（供调试）
esp_err_t audio_link_init(void);

// 下行：USB 扬声器 PCM 写入链路A（非阻塞，DMA 满则丢弃并计数）
// 返回实际写入字节数
size_t audio_link_write_spk(const void *buf, size_t len);

// 上行：从链路B 流缓冲读取 PCM（立体声 48k/16bit），timeout 内无数据返回 0
// 返回实际读取字节数
size_t audio_link_read_mic(void *buf, size_t len, TickType_t timeout);

// 控制通道（UART）：发送一行文本命令（自动追加 \n）
void ctrl_link_send(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_LINK_H_ */
