# ESP32-S3 蓝牙音频桥（S3 端：USB 声卡）

把开发板变成一块 **USB 声卡（UAC1 headset，44.1kHz/16bit，立体声扬声器 + 单声道麦克风）**，PC 的音频经 I2S 转发给 ESP32 经典蓝牙板（见 `../esp32_bt`），最终由 AirPods 播放；通话时 AirPods 的麦克风原路回传给 PC。

```
PC --Type-C(UAC1)--> ESP32-S3 --I2S双链路+UART--> ESP32 --A2DP/HFP--> AirPods 3
```

## Web 状态页面

固件同时启动独立 Wi-Fi 热点，不需要路由器：

- 热点名称：`ESP32S3-Web`
- 密码：`esp32s3web`
- 页面地址：`http://192.168.4.1/`
- 状态接口：`http://192.168.4.1/api/status`

页面每 2 秒显示运行时间、可用内存和已连接 Wi-Fi 客户端数量。Wi-Fi 启动时有较大的瞬时电流；如果串口出现 `Brownout detector was triggered`，请用可靠的 USB 线连接开发板 native USB/OTG 口供电，或从稳定的 5V 电源给开发板供电。

## 接线（重要）

板子通常有两个 USB 口：

| 口 | 用途 |
|---|---|
| **USB-UART**（现有 COM4） | 烧录 / 串口日志 |
| **USB-OTG / native USB** | 枚举为 USB 声卡，**也是整机供电来源** |

PC 要用声卡，必须插 **USB-OTG**（板丝印 `USB`/`OTG` 口）那根线；COM4 只用来看日志。

## 与 ESP32 蓝牙板的连线

| 信号 | S3 引脚 | ESP32 引脚 | 说明 |
|---|---|---|---|
| 链路A BCK | GPIO12 | GPIO26 | S3 主机 → ESP32 从机（下行：PC 扬声器 PCM） |
| 链路A WS | GPIO13 | GPIO25 | 同上 |
| 链路A DOUT | GPIO14 | GPIO22 | 同上 |
| 链路B BCK | GPIO9 | GPIO27 | ESP32 主机 → S3 从机（上行：AirPods 麦克风） |
| 链路B WS | GPIO10 | GPIO15 | 同上 |
| 链路B DIN | GPIO11 | GPIO21 | 同上 |
| UART TX | GPIO17 | GPIO23 | 控制通道 115200 8N1 |
| UART RX | GPIO18 | GPIO19 | 同上 |
| 5V / GND | 5V / GND | 5V / GND | 共地；ESP32 可由 S3 的 5V 供电（BT 峰值约 350mA） |

## UART 控制协议（S3 → ESP32）

| 命令 | 触发时机 |
|---|---|
| `MIC_OPEN` | PC 打开 USB 麦克风流（会议软件开始采集）→ ESP32 暂停 A2DP、建立 HFP eSCO 通话 |
| `MIC_CLOSE` | PC 关闭麦克风流 / USB 拔出 → 恢复 A2DP 音乐 |
| `VOL:0..127` | PC 改系统音量（UAC SET_CUR 回调）→ ESP32 同步 AVRCP 绝对音量 / HFP VGS |
| `PING` | 每 5 秒心跳（ESP32 回 `PONG`），用于链路自检 |

## 环境

```powershell
$env:IDF_PATH = "C:\Espressif\v5.5.2\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python_env\idf5.5_py3.11_env"
. C:\Espressif\v5.5.2\esp-idf\export.ps1
```

## 编译烧录

```powershell
idf.py -C esp32s3 build
idf.py -C esp32s3 -p COM4 flash
python -m esp_idf_monitor -p COM4 esp32s3\build\esp32s3_bt_audio_bridge.elf
```

退出监视器：`Ctrl+]`。

## 使用

1. 两块板按上表接好线，先给 ESP32 上电（或由 S3 的 5V 供电）。
2. Type-C 插 S3 的 **OTG 口** 连 PC，系统声音输出/输入设备选择 **ESP32-S3 BT Audio Bridge**。
3. AirPods 配对由 ESP32 侧完成（首次自动搜索名为 "AirPods" 的设备，见 `../esp32_bt/README.md`）。
4. 平时播放走 A2DP（约 150~200ms 延时）；会议软件打开麦克风自动切 HFP 双向通话，关闭麦克风自动回 A2DP。

## 代码结构

- `main/main.c`：UAC1 声卡（TinyUSB 原生协议栈）、流状态/音量回调 → UART 命令
- `main/usb_descriptors.[ch]`：UAC1 headset 描述符（单采样率 44.1kHz）
- `main/audio_link.[ch]`：I2S 链路A（主 TX）/ 链路B（从 RX）+ UART 控制通道
- `components/tinyusb/`：vendor 的原生 TinyUSB 组件（裁剪版）
