/*
 * TinyUSB 配置：ESP32-S3 UAC1 声卡（蓝牙耳机桥 USB 侧）
 * 全速(FS)设备：立体声扬声器(48k/16bit) + 单声道麦克风(48k/16bit)
 */
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

//--------------------------------------------------------------------+
// Common Configuration
//--------------------------------------------------------------------
// CFG_TUSB_MCU 由 espressif/tinyusb 组件通过编译选项注入 (OPT_MCU_ESP32S3)
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_FREERTOS
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

// Enable Device stack
#define CFG_TUD_ENABLED       1

// ESP32-S3 USB-OTG 仅支持全速 (Full-Speed 12Mbps)
#define CFG_TUD_MAX_SPEED     OPT_MODE_FULL_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_AUDIO             1
#define CFG_TUD_VENDOR            0

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------
// 不用 AC 状态中断端点（Windows UAC1 基本不支持，且描述符未包含）
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP    0

// 单一格式 16bit，采样率固定 44.1kHz（SBC 强制采样率，与 AirPods 协商结果一致，避免重采样）
#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS       1
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE 44100
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX   1    // 麦克风（设备→主机）
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX   2    // 扬声器（主机→设备）

#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX   2
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX           16
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX   2
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX           16

// EP IN（麦克风）缓冲：等时端点缓冲与 EP 等大，软件 FIFO 取 8 倍以抗抖动
#define CFG_TUD_AUDIO_ENABLE_EP_IN           1
#define CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_IN   TUD_AUDIO_EP_SIZE(false, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX          CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_IN
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ       (8 * CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX)

// EP OUT（扬声器）
#define CFG_TUD_AUDIO_ENABLE_EP_OUT          1
#define CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_OUT  TUD_AUDIO_EP_SIZE(false, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX         CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_OUT
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ      (8 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
