#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"

#define APP_BUTTON          (GPIO_NUM_0) /* BOOT on most DevKits */
#define DISTANCE_MAX        125
#define DELTA_SCALAR        5
#define MOVE_INTERVAL_MS    20
#define SQUARE_PAUSE_MS     800

static const char *TAG = "hid_mouse";

/************* TinyUSB descriptors ****************/

#define TUSB_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

/* Mouse-only HID report descriptor */
const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_ITF_PROTOCOL_MOUSE))
};

const char *hid_string_descriptor[5] = {
    (char[]){0x09, 0x04},       /* 0: English (0x0409) */
    "Espressif",                /* 1: Manufacturer */
    "ESP32-S3 Ergo Mouse",      /* 2: Product */
    "0001",                     /* 3: Serial */
    "HID Mouse",                /* 4: Interface */
};

static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

/********* TinyUSB HID callbacks ***************/

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

/********* Mouse movement ***************/

typedef enum {
    MOUSE_DIR_RIGHT = 0,
    MOUSE_DIR_DOWN,
    MOUSE_DIR_LEFT,
    MOUSE_DIR_UP,
    MOUSE_DIR_MAX,
} mouse_dir_t;

/* Relative move helper (host cursor follows HID deltas). */
static bool mouse_move(int8_t dx, int8_t dy)
{
    if (!tud_hid_ready()) {
        return false;
    }
    return tud_hid_mouse_report(HID_ITF_PROTOCOL_MOUSE, 0x00, dx, dy, 0, 0);
}

static void mouse_draw_square_once(void)
{
    static mouse_dir_t cur_dir = MOUSE_DIR_RIGHT;
    static uint32_t distance = 0;

    ESP_LOGI(TAG, "Move cursor in a square");
    for (int i = 0; i < (DISTANCE_MAX / DELTA_SCALAR) * 4; i++) {
        int8_t dx = 0;
        int8_t dy = 0;

        switch (cur_dir) {
        case MOUSE_DIR_RIGHT: dx = DELTA_SCALAR; break;
        case MOUSE_DIR_DOWN:  dy = DELTA_SCALAR; break;
        case MOUSE_DIR_LEFT:  dx = (int8_t)(-DELTA_SCALAR); break;
        case MOUSE_DIR_UP:    dy = (int8_t)(-DELTA_SCALAR); break;
        default: break;
        }

        while (!mouse_move(dx, dy)) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        distance += DELTA_SCALAR;
        if (distance >= DISTANCE_MAX) {
            distance = 0;
            cur_dir = (mouse_dir_t)((cur_dir + 1) % MOUSE_DIR_MAX);
        }
        vTaskDelay(pdMS_TO_TICKS(MOVE_INTERVAL_MS));
    }
}

static bool boot_pressed(void)
{
    /* BOOT is active-low with pull-up */
    return gpio_get_level(APP_BUTTON) == 0;
}

void app_main(void)
{
    const gpio_config_t boot_button_config = {
        .pin_bit_mask = BIT64(APP_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = true,
        .pull_down_en = false,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_button_config));

    ESP_LOGI(TAG, "USB HID mouse init (use native USB-OTG, not UART COM port)");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
    tusb_cfg.descriptor.string = hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB ready — wait for host mount, then move cursor");

    bool paused = false;
    bool prev_boot = false;

    while (1) {
        bool boot = boot_pressed();
        if (boot && !prev_boot) {
            paused = !paused;
            ESP_LOGI(TAG, "movement %s (BOOT)", paused ? "paused" : "running");
        }
        prev_boot = boot;

        if (tud_mounted() && !paused) {
            mouse_draw_square_once();
            vTaskDelay(pdMS_TO_TICKS(SQUARE_PAUSE_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
