# OTA 完整操作指南

## 前置条件

- [x] Keil µVision5 已安装
- [x] PlatformIO (VSCode) 已安装
- [x] ST-Link 驱动正常
- [x] Keil 工程已生成, 无需手动创建

---

## 第一步：启动 OTA Server

```bash
cd C:\Users\26895\Desktop\OTA\server
python serve.py
```

看到 `Listening on http://192.168.0.104:8080` 即成功。**保持窗口开着。**

---

## 第二步：编译烧录 STM32 Bootloader

**双击打开**: `stm32/bootloader/bootloader.uvprojx`

工程已配置好，无需任何修改：

| 配置项 | 值 |
|--------|-----|
| 芯片 | STM32F103VE |
| IROM1 | 0x08000000, 0x4000 |
| Defines | STM32F10X_HD |
| Include Paths | Core/Inc + CMSIS + StdPeriph (已设好) |
| Scatter File | STM32F103ZETX_BOOT.sct |

1. **F7** 编译 → 0 Error, 0 Warning
2. **F8** 烧录

---

## 第三步：编译烧录 STM32 App

**双击打开**: `stm32/app/app.uvprojx`

| 配置项 | 值 |
|--------|-----|
| IROM1 | `0x08004000`, `0x3C000` ← App A 地址! |
| Defines | STM32F10X_HD, USE_STDPERIPH_DRIVER |
| After Build | 自动生成 .bin → `server/firmware/stm32_app.bin` |

1. **F7** 编译 → 0 Error
2. **F8** 烧录

---

## 第四步：编译烧录 ESP32

VSCode 打开 `C:\Users\26895\Desktop\OTA\esp32`

PlatformIO → Build → Upload (或 `pio run -t upload`)

串口监视器 (115200 baud)，看到：
```
WiFi connected! IP: 192.168.0.xxx
STM32 connected ✓
```

---

## 第五步：接线验证

```
ESP32 GPIO16 ←── STM32 PA2 (USART2 TX)
ESP32 GPIO17 ──→ STM32 PA3 (USART2 RX)
ESP32 GND    ──── STM32 GND

STM32 ST-Link → PC (烧录)
STM32 USB口  → PC (CH340 串口调试, 可选)
ESP32 USB口  → PC (烧录+供电)
```

---

## 第六步：执行 OTA 升级

### 6.1 修改版本号并编译

修改 `stm32/app/Core/Inc/version.h`:
```c
#define FW_VERSION_STRING    "1.0.1"   // 原来是 "1.0.0"
```

Keil → F7 编译 → .bin 自动生成到 `server/firmware/stm32_app.bin`

### 6.2 更新 version.json

```bash
cd C:\Users\26895\Desktop\OTA
python tools/generate_version.py server/firmware/stm32_app.bin 1.0.1 http://192.168.0.104:8080
```

### 6.3 触发升级

重启 ESP32，或等它自动检查（每小时一次）：

```
[OTA] Update available: 1.0.0 → 1.0.1
[OTA] Download complete: XXXXX bytes
[OTA] OTA verify PASSED
[OTA] OTA SUCCESS! STM32 rebooting to new firmware...
```

STM32 重启 → Bootloader 检测新固件 → 切换分区 → 运行 App B ✓

---

## 注意事项

- **编译 App B (备用)**: 改 Target → IROM1 为 `0x08040000`, Define 加 `VECT_TAB_OFFSET=0x3C000`, Scatter File 需另存一份
- **每次 OTA**: 改 version.h 版本号 → 编译 → generate_version.py → 等 ESP32 自动升级
- **调试**: STM32 USB 口接 CH340 → 串口助手 (115200) → 可看 printf 日志 (USART1)
- **OTA 通信**: 走 USART2 (PA2/PA3)，与 USB 串口互不影响
