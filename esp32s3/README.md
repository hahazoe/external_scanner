# ESP32-S3 USB HID 鼠标（Ergo Mouse）

把开发板模拟成 USB 鼠标设备，并驱动光标按正方形轨迹移动。

## 接线（重要）

板子通常有两个 USB 口：

| 口 | 用途 |
|---|---|
| **USB-UART**（现有 COM4） | 烧录 / 串口日志 |
| **USB-OTG / native USB** | 作为鼠标被电脑识别 |

电脑要识别鼠标，必须再插一根线到 **USB-OTG**（或板丝印 `USB`/`OTG` 口）。只插 COM4 那根不够。

## 行为

- 主机挂载成功后，光标自动画正方形
- 按板载 **BOOT** 键：暂停 / 继续移动
- 设备名：`ESP32-S3 Ergo Mouse`

## 环境

```powershell
$env:IDF_PATH = "C:\Espressif\v5.5.2\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
. C:\Espressif\v5.5.2\esp-idf\export.ps1
cd <repo>\esp32s3
```

## 编译烧录

```bash
idf.py set-target esp32s3   # 仅首次
idf.py -p COM4 build flash monitor
```

退出监视器：`Ctrl+]`。

## 改移动参数

见 `main/main.c`：

- `DELTA_SCALAR`：单步位移
- `DISTANCE_MAX`：单边长度
- `MOVE_INTERVAL_MS`：步进间隔
- `mouse_move(dx, dy)`：相对移动封装
