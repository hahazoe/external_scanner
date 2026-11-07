/*
 * USB 描述符：UAC1 耳机设备（立体声扬声器 + 单声道麦克风，48kHz/16bit）
 */
#include "tusb.h"
#include "usb_descriptors.h"
#include "esp_mac.h"

/* 厂商自定义 VID/PID（Cafe 为 TinyUSB 示例惯例测试 VID）
 * PID 位图: [MSB] AUDIO | MIDI | HID | MSC | CDC [LSB] */
#define PID_MAP(itf, n)  ((CFG_TUD_##itf) ? (1 << (n)) : 0)
#define USB_PID           (0x4000 | PID_MAP(CDC, 0) | PID_MAP(MSC, 1) | PID_MAP(HID, 2) | \
    PID_MAP(MIDI, 3) | PID_MAP(AUDIO, 4) | PID_MAP(VENDOR, 5) )

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    // 使用 IAD 组合音频接口
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0xCafe,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
#define EPNUM_AUDIO_OUT   0x01
#define EPNUM_AUDIO_IN    0x01

#define CONFIG_UAC1_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_AUDIO10_HEADSET_STEREO_DESC_LEN(1))

static uint8_t const desc_uac1_configuration[] =
{
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_UAC1_TOTAL_LEN, 0x00, 100),

    // 仅声明 44100Hz 单一采样率：与 A2DP SBC 协商采样率一致，避免重采样
    TUD_AUDIO10_HEADSET_STEREO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 4,
      CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
      CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX,
      EPNUM_AUDIO_OUT, CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_OUT,
      EPNUM_AUDIO_IN | 0x80, CFG_TUD_AUDIO10_FUNC_1_FORMAT_1_EP_SZ_IN,
      44100)
};

TU_VERIFY_STATIC(sizeof(desc_uac1_configuration) == CONFIG_UAC1_TOTAL_LEN, "Incorrect size");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_uac1_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
};

static char const *string_desc_arr[] =
{
  (const char[]) { 0x09, 0x04 },  // 0: English (0x0409)
  "Espressif",                    // 1: Manufacturer
  "ESP32-S3 BT Audio Bridge",     // 2: Product
  NULL,                           // 3: Serial = eFuse MAC
  "UAC1 Headset",                 // 4: Function
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    switch (index) {
    case STRID_LANGID:
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
        break;

    case STRID_SERIAL: {
        // 用 eFuse 默认 MAC 作为序列号（12 位十六进制）
        uint8_t mac[6] = {0};
        char mac_str[13] = {0};
        if (esp_efuse_mac_get_default(mac) == ESP_OK) {
            for (int i = 0; i < 6; i++) {
                static const char hex[] = "0123456789ABCDEF";
                mac_str[i * 2]     = hex[(mac[i] >> 4) & 0xF];
                mac_str[i * 2 + 1] = hex[mac[i] & 0xF];
            }
        }
        chr_count = strlen(mac_str);
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = mac_str[i];
        }
        break;
    }

    default: {
        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) return NULL;

        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
        if (chr_count > max_count) chr_count = max_count;

        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
        break;
    }
    }

    // 首字节为长度（含头部），次字节为描述符类型
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
