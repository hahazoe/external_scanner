#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "web_server.h"

static const char *TAG = "web_server";

#define WEB_AP_SSID      "ESP32S3-Web"
#define WEB_AP_PASSWORD  "esp32s3web"
#define WEB_AP_CHANNEL   6
#define WEB_AP_MAX_CONN  4

static httpd_handle_t s_server;
static volatile uint8_t s_station_count;

static const char s_index_html[] =
    "<!doctype html><html lang=\"zh-CN\"><head>"
    "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ESP32-S3 Web Server</title>"
    "<style>"
    "*{box-sizing:border-box}body{margin:0;background:#0f172a;color:#e2e8f0;"
    "font-family:system-ui,-apple-system,sans-serif}.wrap{max-width:720px;margin:auto;padding:32px 18px}"
    "h1{font-size:26px;margin:0 0 8px}.sub{color:#94a3b8;margin-bottom:24px}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px}"
    ".card{background:#1e293b;border:1px solid #334155;border-radius:14px;padding:18px}"
    ".label{color:#94a3b8;font-size:13px}.value{font-size:25px;font-weight:650;margin-top:7px}"
    ".ok{color:#4ade80}.foot{color:#64748b;font-size:12px;margin-top:24px}"
    "</style></head><body><main class=\"wrap\">"
    "<h1>ESP32-S3 Web Server</h1>"
    "<div class=\"sub\">USB 音频桥与 Wi-Fi Web 服务正在运行</div>"
    "<section class=\"grid\">"
    "<div class=\"card\"><div class=\"label\">设备状态</div><div class=\"value ok\">在线</div></div>"
    "<div class=\"card\"><div class=\"label\">运行时间</div><div class=\"value\" id=\"up\">--</div></div>"
    "<div class=\"card\"><div class=\"label\">可用内存</div><div class=\"value\" id=\"heap\">--</div></div>"
    "<div class=\"card\"><div class=\"label\">Wi-Fi 客户端</div><div class=\"value\" id=\"sta\">--</div></div>"
    "</section><div class=\"foot\">热点 ESP32S3-Web · 192.168.4.1</div>"
    "</main><script>"
    "async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});"
    "const s=await r.json();document.getElementById('up').textContent=s.uptime_s+' 秒';"
    "document.getElementById('heap').textContent=Math.round(s.free_heap/1024)+' KB';"
    "document.getElementById('sta').textContent=s.stations;}catch(e){}}"
    "refresh();setInterval(refresh,2000);"
    "</script></body></html>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char json[160];
    int len = snprintf(json, sizeof(json),
                       "{\"uptime_s\":%lld,\"free_heap\":%u,\"stations\":%u}",
                       esp_timer_get_time() / 1000000,
                       (unsigned)esp_get_free_heap_size(),
                       (unsigned)s_station_count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, len);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        s_station_count++;
        ESP_LOGI(TAG, "client connected, stations=%u", (unsigned)s_station_count);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_station_count > 0) {
            s_station_count--;
        }
        ESP_LOGI(TAG, "client disconnected, stations=%u", (unsigned)s_station_count);
    }
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &status_uri));
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_cfg), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
        TAG, "Wi-Fi event registration failed");

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WEB_AP_SSID,
            .ssid_len = sizeof(WEB_AP_SSID) - 1,
            .channel = WEB_AP_CHANNEL,
            .password = WEB_AP_PASSWORD,
            .max_connection = WEB_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start AP failed");
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "start HTTP server failed");

    ESP_LOGI(TAG, "AP ready: SSID=%s password=%s", WEB_AP_SSID, WEB_AP_PASSWORD);
    ESP_LOGI(TAG, "Web UI: http://192.168.4.1/");
    return ESP_OK;
}
