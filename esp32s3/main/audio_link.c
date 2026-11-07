/*
 * 板间链路实现（ESP32-S3 侧）
 *
 * 接线（与 esp32_bt 工程引脚一一对应）：
 *   链路A（S3→ESP32）：BCK=GPIO12 WS=GPIO13 DOUT=GPIO14
 *   链路B（ESP32→S3）：BCK=GPIO9  WS=GPIO10 DIN=GPIO11
 *   UART：S3 TX=GPIO17 → ESP32 RX=GPIO23；S3 RX=GPIO18 ← ESP32 TX=GPIO19；共地
 */
#include "audio_link.h"
#include <string.h>
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "audio_link";

//--------------------------------------------------------------------+
// 引脚与参数
//--------------------------------------------------------------------+
// 链路A：S3 主机 TX → ESP32（下行）
#define LINK_A_I2S_PORT   I2S_NUM_0
#define LINK_A_BCK        GPIO_NUM_12
#define LINK_A_WS         GPIO_NUM_13
#define LINK_A_DOUT       GPIO_NUM_14

// 链路B：ESP32 主机 → S3 从机 RX（上行）
#define LINK_B_I2S_PORT   I2S_NUM_1
#define LINK_B_BCK        GPIO_NUM_9
#define LINK_B_WS         GPIO_NUM_10
#define LINK_B_DIN        GPIO_NUM_11

// UART 控制通道
#define CTRL_UART_PORT    UART_NUM_1
#define CTRL_UART_TX      GPIO_NUM_17
#define CTRL_UART_RX      GPIO_NUM_18
#define CTRL_UART_BAUD    115200

// 44.1kHz/16bit/立体声：平均每毫秒 176.4 字节（仅用于缓冲粒度估算）
#define LINK_BYTES_PER_MS   177

// 链路B 上行流缓冲容量（约 40ms）
#define LINK_B_STREAM_BUF_SZ  (LINK_BYTES_PER_MS * 40)

static i2s_chan_handle_t s_tx_a = NULL;   // 链路A TX（主机）
static i2s_chan_handle_t s_rx_b = NULL;   // 链路B RX（从机）
static StreamBufferHandle_t s_linkb_stream = NULL;
static volatile uint32_t s_tx_a_drop_bytes = 0;

//--------------------------------------------------------------------+
// I2S 初始化
//--------------------------------------------------------------------+
static esp_err_t link_a_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(LINK_A_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;   // 每描述符 5ms
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_a, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = LINK_A_BCK,
            .ws = LINK_A_WS,
            .dout = LINK_A_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false, .bclk_inv = false, .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_a, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_a));
    ESP_LOGI(TAG, "link A (I2S%d master TX) ready: BCK=%d WS=%d DOUT=%d",
             LINK_A_I2S_PORT, LINK_A_BCK, LINK_A_WS, LINK_A_DOUT);
    return ESP_OK;
}

static esp_err_t link_b_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(LINK_B_I2S_PORT, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_b));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),   // 从机时钟由对端提供，此值仅作参考
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = LINK_B_BCK,
            .ws = LINK_B_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = LINK_B_DIN,
            .invert_flags = {
                .mclk_inv = false, .bclk_inv = false, .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_b, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_b));
    ESP_LOGI(TAG, "link B (I2S%d slave RX) ready: BCK=%d WS=%d DIN=%d",
             LINK_B_I2S_PORT, LINK_B_BCK, LINK_B_WS, LINK_B_DIN);
    return ESP_OK;
}

// 链路B 接收任务：I2S DMA → 流缓冲（供音频任务按 1ms 消费）
static void link_b_rx_task(void *arg)
{
    uint8_t buf[LINK_BYTES_PER_MS * 4];
    while (1) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_b, buf, sizeof(buf), &bytes_read, portMAX_DELAY);
        if (err == ESP_OK && bytes_read > 0) {
            xStreamBufferSend(s_linkb_stream, buf, bytes_read, 0);
        }
    }
}

//--------------------------------------------------------------------+
// UART 控制通道
//--------------------------------------------------------------------+
static void ctrl_link_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = CTRL_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CTRL_UART_PORT, 1024, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CTRL_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CTRL_UART_PORT, CTRL_UART_TX, CTRL_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "ctrl UART ready: TX=%d RX=%d @%d", CTRL_UART_TX, CTRL_UART_RX, CTRL_UART_BAUD);
}

// 控制通道接收任务：按行打印对端日志/应答（阶段4 起解析状态上报）
static void ctrl_rx_task(void *arg)
{
    uint8_t byte;
    char line[96];
    size_t pos = 0;
    while (1) {
        int n = uart_read_bytes(CTRL_UART_PORT, &byte, 1, portMAX_DELAY);
        if (n != 1) continue;
        if (byte == '\n' || pos >= sizeof(line) - 1) {
            line[pos] = '\0';
            if (pos > 0) {
                ESP_LOGI(TAG, "ctrl <- %s", line);
            }
            pos = 0;
        } else if (byte != '\r') {
            line[pos++] = (char)byte;
        }
    }
}

void ctrl_link_send(const char *cmd)
{
    uart_write_bytes(CTRL_UART_PORT, cmd, strlen(cmd));
    uart_write_bytes(CTRL_UART_PORT, "\n", 1);
}

//--------------------------------------------------------------------+
// 对外接口
//--------------------------------------------------------------------+
esp_err_t audio_link_init(void)
{
    s_linkb_stream = xStreamBufferCreate(LINK_B_STREAM_BUF_SZ, 1);
    if (s_linkb_stream == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(link_a_init());
    ESP_ERROR_CHECK(link_b_init());
    ctrl_link_init();

    xTaskCreatePinnedToCore(link_b_rx_task, "linkb_rx", 2048, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(ctrl_rx_task, "ctrl_rx", 2048, NULL, 3, NULL, 0);
    return ESP_OK;
}

size_t audio_link_write_spk(const void *buf, size_t len)
{
    size_t written = 0;
    // 不阻塞：DMA 来不及消费就丢弃（USB 时钟与 I2S 时钟存在微小漂移）
    esp_err_t err = i2s_channel_write(s_tx_a, buf, len, &written, 0);
    if (err != ESP_OK || written < len) {
        s_tx_a_drop_bytes += (len - written);
    }
    return written;
}

size_t audio_link_read_mic(void *buf, size_t len, TickType_t timeout)
{
    return xStreamBufferReceive(s_linkb_stream, buf, len, timeout);
}
