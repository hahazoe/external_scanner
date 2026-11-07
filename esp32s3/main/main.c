/*
 * ESP32-S3 侧固件：USB UAC1 声卡（蓝牙耳机桥 USB 端）
 *
 * 数据通路（阶段2 起）：
 *   下行：PC → USB 扬声器 → I2S 链路A → ESP32 蓝牙板 → AirPods
 *   上行：AirPods 麦克风 → ESP32 → I2S 链路B → 混单声道 → USB 麦克风 → PC
 * 阶段1 的 USB 环回模式可在 menuconfig 中不切回，已被 I2S 链路模式取代。
 *
 * 音频格式固定 44.1kHz/16bit：扬声器 2ch，麦克风 1ch，
 * 板间两条 I2S 链路均为立体声（上行由对端复制成双声道）。
 * 选 44.1kHz 的原因：A2DP SBC 强制支持 44.1kHz，AirPods 协商必中，全链路免重采样。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "audio_link.h"
#include "web_server.h"

static const char *TAG = "uac_bridge";

//--------------------------------------------------------------------+
// 音频状态
//--------------------------------------------------------------------+
// 描述符只声明 44.1kHz 单一采样率（与 A2DP SBC 协商结果一致）
static uint32_t current_sample_rate = 44100;

// 音频控制当前状态：master + 左/右声道
static uint8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];    // +1 for master channel 0
static int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];  // 单位 1/256 dB

// 流状态（mic 打开 = PC 进入通话 → 经 UART 通知蓝牙板切 HFP）
static volatile bool g_spk_streaming = false;
static volatile bool g_mic_streaming = false;

// UAC 音量范围（与 audio10_get_req_entity 的 MIN/MAX 一致），用于映射到 AVRCP 0..127
#define VOL_MIN_1_256_DB   (-96 * 256)
#define VOL_MAX_1_256_DB   (0)

// 把 UAC 音量(1/256 dB)映射为 0..127 并经 UART 同步给蓝牙板（AVRCP 绝对音量 / HFP VGS）
static void volume_sync_to_bt(int16_t vol_1_256_db)
{
    int32_t v = vol_1_256_db;
    if (v < VOL_MIN_1_256_DB) v = VOL_MIN_1_256_DB;
    if (v > VOL_MAX_1_256_DB) v = VOL_MAX_1_256_DB;
    uint8_t vol127 = (uint8_t)((v - VOL_MIN_1_256_DB) * 127 / (VOL_MAX_1_256_DB - VOL_MIN_1_256_DB));
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "VOL:%u", vol127);
    ctrl_link_send(cmd);
}

// USB 音频缓冲区
static uint8_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ];
static uint8_t mic_buf[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ];

// 链路B 每次读取粒度：约 2ms 立体声 (44.1k/16bit/2ch ≈ 176B/ms)
#define LINK_B_READ_CHUNK  (177 * 2)

//--------------------------------------------------------------------+
// TinyUSB 在 FreeRTOS 下需要应用提供时间函数（osal_none 之外未实现）
//--------------------------------------------------------------------+
uint32_t tusb_time_millis_api(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void tusb_time_delay_ms_api(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+
void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "USB mounted (configured by host)");
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "USB unmounted");
    g_spk_streaming = false;
    if (g_mic_streaming) {
        g_mic_streaming = false;
        ctrl_link_send("MIC_CLOSE");
    }
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    ESP_LOGI(TAG, "USB suspended");
}

void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "USB resumed");
}

//--------------------------------------------------------------------+
// UAC1 控制请求处理
//--------------------------------------------------------------------+
static bool audio10_set_req_ep(tusb_control_request_t const *p_request, uint8_t *pBuff)
{
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

    switch (ctrlSel) {
    case AUDIO10_EP_CTRL_SAMPLING_FREQ:
        if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
            TU_VERIFY(p_request->wLength == 3);
            current_sample_rate = tu_unaligned_read32(pBuff) & 0x00FFFFFF;
            ESP_LOGI(TAG, "EP set sampling freq: %lu", (unsigned long)current_sample_rate);
            return true;
        }
        break;
    default:
        ESP_LOGW(TAG, "EP set req unknown ctrlSel 0x%x", ctrlSel);
        return false;
    }
    return false;
}

static bool audio10_get_req_ep(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

    switch (ctrlSel) {
    case AUDIO10_EP_CTRL_SAMPLING_FREQ:
        if (p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
            uint8_t freq[3];
            freq[0] = (uint8_t)(current_sample_rate & 0xFF);
            freq[1] = (uint8_t)((current_sample_rate >> 8) & 0xFF);
            freq[2] = (uint8_t)((current_sample_rate >> 16) & 0xFF);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
        }
        break;
    default:
        ESP_LOGW(TAG, "EP get req unknown ctrlSel 0x%x", ctrlSel);
        return false;
    }
    return false;
}

static bool audio10_set_req_entity(tusb_control_request_t const *p_request, uint8_t *pBuff)
{
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

    // 仅扬声器 Feature Unit
    if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
        switch (ctrlSel) {
        case AUDIO10_FU_CTRL_MUTE:
            switch (p_request->bRequest) {
            case AUDIO10_CS_REQ_SET_CUR:
                TU_VERIFY(p_request->wLength == 1);
                mute[channelNum] = pBuff[0];
                ESP_LOGI(TAG, "Set Mute: %d of channel: %u", mute[channelNum], channelNum);
                if (channelNum == 0 && mute[0]) {
                    volume_sync_to_bt(VOL_MIN_1_256_DB);   // 静音 → 对端音量 0
                }
                return true;
            default:
                return false;
            }

        case AUDIO10_FU_CTRL_VOLUME:
            switch (p_request->bRequest) {
            case AUDIO10_CS_REQ_SET_CUR:
                TU_VERIFY(p_request->wLength == 2);
                volume[channelNum] = (int16_t)tu_unaligned_read16(pBuff);
                ESP_LOGI(TAG, "Set Volume: %d dB of channel: %u", volume[channelNum] / 256, channelNum);
                volume_sync_to_bt(volume[channelNum]);
                return true;
            default:
                return false;
            }

        default:
            return false;
        }
    }
    return false;
}

static bool audio10_get_req_entity(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

    if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
        switch (ctrlSel) {
        case AUDIO10_FU_CTRL_MUTE:
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute[channelNum], 1);

        case AUDIO10_FU_CTRL_VOLUME:
            switch (p_request->bRequest) {
            case AUDIO10_CS_REQ_GET_CUR:
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &volume[channelNum], sizeof(volume[channelNum]));
            case AUDIO10_CS_REQ_GET_MIN: {
                int16_t min = -96 * 256;  // -96 dB
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &min, sizeof(min));
            }
            case AUDIO10_CS_REQ_GET_MAX: {
                int16_t max = 0;          // 0 dB
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &max, sizeof(max));
            }
            case AUDIO10_CS_REQ_GET_RES: {
                int16_t res = 1 * 256;    // 1 dB 步进
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &res, sizeof(res));
            }
            default:
                return false;
            }
            break;

        default:
            return false;
        }
    }
    return false;
}

//--------------------------------------------------------------------+
// TinyUSB audio 回调入口
//--------------------------------------------------------------------+
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff)
{
    (void)rhport;
    if (tud_audio_version() == 1) {
        return audio10_set_req_ep(p_request, pBuff);
    }
    return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    if (tud_audio_version() == 1) {
        return audio10_get_req_ep(rhport, p_request);
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    if (tud_audio_version() == 1) {
        return audio10_get_req_entity(rhport, p_request);
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    (void)rhport;
    if (tud_audio_version() == 1) {
        return audio10_set_req_entity(p_request, buf);
    }
    return false;
}

// 主机设置 AS 接口备用设置（开/关流）
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);

    if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
        g_spk_streaming = (alt != 0);
        ESP_LOGI(TAG, "Speaker streaming %s", g_spk_streaming ? "START" : "STOP");
    } else if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
        g_mic_streaming = (alt != 0);
        // 通话模式触发：PC 打开/关闭麦克风 → 通知蓝牙板切 HFP / 回 A2DP
        ctrl_link_send(g_mic_streaming ? "MIC_OPEN" : "MIC_CLOSE");
        ESP_LOGI(TAG, "Mic streaming %s (call-mode %s)", g_mic_streaming ? "START" : "STOP",
                 g_mic_streaming ? "ENTER" : "EXIT");
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);

    if (itf == ITF_NUM_AUDIO_STREAMING_SPK && alt == 0) {
        g_spk_streaming = false;
    } else if (itf == ITF_NUM_AUDIO_STREAMING_MIC && alt == 0) {
        if (g_mic_streaming) {
            g_mic_streaming = false;
            ctrl_link_send("MIC_CLOSE");
            ESP_LOGI(TAG, "Mic streaming STOP (ep close, call-mode EXIT)");
        }
    }
    return true;
}

//--------------------------------------------------------------------+
// 音频数据通路（1ms 周期）
//--------------------------------------------------------------------+
// 下行：USB 扬声器 PCM → I2S 链路A → ESP32 蓝牙板
// 上行：I2S 链路B（立体声）混成单声道 → USB 麦克风
static void audio_task(void *arg)
{
    ESP_LOGI(TAG, "audio task started (I2S link mode)");
    static uint8_t linkb_buf[LINK_B_READ_CHUNK];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1));

        // 下行
        int spk_data_size = tud_audio_read(spk_buf, sizeof(spk_buf));
        if (spk_data_size > 0) {
            audio_link_write_spk(spk_buf, (size_t)spk_data_size);
        }

        // 上行：链路B 立体声 → 单声道麦克风
        size_t got = audio_link_read_mic(linkb_buf, sizeof(linkb_buf), pdMS_TO_TICKS(2));
        if (got > 0) {
            int16_t *src = (int16_t *)linkb_buf;
            int16_t *limit = (int16_t *)linkb_buf + got / 2;
            int16_t *dst = (int16_t *)mic_buf;
            while (src < limit) {
                int32_t left = *src++;
                int32_t right = *src++;
                *dst++ = (int16_t)((left >> 1) + (right >> 1));
            }
            tud_audio_write(mic_buf, (uint16_t)(got / 2));
        }
    }
}

//--------------------------------------------------------------------+
// TinyUSB 设备栈任务
//--------------------------------------------------------------------+
static void usb_device_task(void *arg)
{
    // 在配置好的 roothub 端口上初始化设备协议栈
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
    ESP_LOGI(TAG, "TinyUSB device stack inited (UAC1 headset, 44.1kHz/16bit)");

    while (1) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// 阶段2 自检：周期 PING（阶段4 起由通话模式命令取代）
static void ctrl_heartbeat_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ctrl_link_send("PING");
    }
}

//--------------------------------------------------------------------+
// app_main
//--------------------------------------------------------------------+
void app_main(void)
{
    // 配置 USB PHY（内部 FS PHY，设备模式）——原生 tinyusb 路线需自行完成此步
    usb_phy_handle_t phy_hdl = NULL;
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &phy_hdl));

    // I2S 链路A/B + UART 控制通道（对端为 ESP32 蓝牙板）
    ESP_ERROR_CHECK(audio_link_init());

    xTaskCreatePinnedToCore(usb_device_task, "tud", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(ctrl_heartbeat_task, "ctrl_hb", 2048, NULL, 2, NULL, 0);

    // 启动独立 Wi-Fi 热点与 Web 状态页面，不依赖外部路由器
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "USB audio bridge init done, plug native USB port to PC");
}
