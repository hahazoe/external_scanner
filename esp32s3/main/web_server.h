#ifndef WEB_SERVER_H_
#define WEB_SERVER_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动 Wi-Fi SoftAP 与 HTTP 服务。
// 默认热点：ESP32S3-Web，密码：esp32s3web，访问地址：http://192.168.4.1/
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SERVER_H_ */
