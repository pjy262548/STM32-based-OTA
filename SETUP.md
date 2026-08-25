# 开发环境搭建指南

## 你需要安装的软件

### 已有 ✓

- **Keil µVision5** - STM32 编译和调试 (已安装)
- **VSCode** - 代码编辑 + ESP32 开发 (已安装)
- **Python 3.13** - OTA Server 和工具脚本 (D:\python\python.exe)

### 需要安装

- **PlatformIO IDE** - VSCode 插件, 用于 ESP32 开发
- **ESP32 USB 驱动** (CP210x / CH340)
- **Git Bash** (已有)

---

## Step 1: 安装 PlatformIO

### 方法 A: VSCode 插件 (推荐)

1. 打开 VSCode
2. 点击左侧 **扩展** 图标 (Ctrl+Shift+X)
3. 搜索 **PlatformIO IDE**
4. 点击安装
5. 等待安装完成 (首次安装会下载约 500MB, 需要良好网络)

安装完成后, 重启 VSCode, 左侧会出现蚂蚁图标。

### 方法 B: 命令行安装 (备选)

```bash
pip install platformio
```

安装后在项目目录使用 `pio` 命令。

> **网络问题?** PlatformIO 首次启动需要下载 ESP32 工具链 (~200MB),
> 可能需要科学上网。如果失败, 可以多试几次, 或者设置代理:
> ```bash
> set HTTP_PROXY=http://127.0.0.1:7890
> ```

---

## Step 2: 安装 ESP32 USB 驱动

ZY-ESP32 通常使用 **CH340** 或 **CP2102** USB 转串口芯片。

### CH340 驱动

1. 下载: http://www.wch.cn/downloads/CH341SER_EXE.html
2. 安装后插上 ESP32, 设备管理器应出现 `COMx`

### CP210x 驱动

1. 下载: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
2. 安装后同上

### 验证

```bash
# 插上 ESP32 后查看串口
python -c "import serial.tools.list_ports; [print(p) for p in serial.tools.list_ports.comports()]"
```

---

## Step 3: 验证 STM32 开发环境

```bash
# Keil 编译器应该在你的 PATH 中
# 打开 Keil → Project → Open Project → 选择 stm32/bootloader/ 目录

# ST-Link 驱动应该已装 (Keil 安装时会附带)
# 如果没有, 下载: https://www.st.com/en/development-tools/stsw-link009.html
```

---

## Step 4: 编译 ESP32 项目

```bash
cd C:\Users\26895\Desktop\OTA\esp32

# 首次运行会下载依赖
pio run

# 烧录到 ESP32
pio run -t upload

# 查看串口日志
pio device monitor
```

---

## Step 5: 启动 OTA Server

```bash
cd C:\Users\26895\Desktop\OTA\server
python serve.py
```

输出示例:
```
==================================================
  STM32 OTA Firmware Server
==================================================
  Firmware dir : C:\Users\26895\Desktop\OTA\server\firmware
  Listening on : http://192.168.1.100:8080
==================================================
```

把 `.bin` 固件丢到 `server/firmware/` 目录即可。

---

## Step 6: 硬件连接

```
STM32 (正点原子精英版)     ZY-ESP32
   PA2  (TX) ────────────── GPIO16 (RX)    ← USART2
   PA3  (RX) ────────────── GPIO17 (TX)    ← USART2
   GND      ────────────── GND

   另外:
   ST-Link ──────────────── STM32 SWD接口 (烧录+供电)
   STM32 USB口 ──────────── PC (CH340串口, printf调试)

注意: 两个板子共地 (GND)!
      两个板子分别用 USB 供电或用同一电源。
```

---

## 环境验证 Checklist

| 步骤 | 验证方法 |
|------|---------|
| ☐ Keil 能编译 STM32 项目 | 打开一个示例工程 → Build |
| ☐ ST-Link 能烧录 | Flash → Download |
| ☐ PlatformIO 安装成功 | VSCode 左侧有蚂蚁图标 |
| ☐ ESP32 能编译 | `pio run` 成功 |
| ☐ ESP32 能烧录 | `pio run -t upload` |
| ☐ USB 串口驱动正常 | 设备管理器能看到 COM 口 |
| ☐ Python 环境就绪 | `python --version` 输出正常 |
| ☐ UART 通信正常 | 任意一方发数据, 另一方收到 |
