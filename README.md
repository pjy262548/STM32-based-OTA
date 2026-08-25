# STM32 + ESP32 OTA 保姆级教程

这份文档的目标是：即使你刚开始接触 STM32、ESP32、Keil、PlatformIO、MQTT，也能照着一步一步把 OTA 跑通。

本文使用占位符，不绑定某一台电脑：

```text
<OTA_ROOT>    表示本工程根目录，例如 C:\Users\xx\Desktop\OTA
<PC_IP>       表示电脑在当前 WiFi/局域网下的 IPv4 地址，例如 192.168.0.108
<ESP32_COM>   表示 ESP32 下载串口，例如 COM7
<STLINK_COM>  如果你的 STM32 有独立串口日志，这里表示那个串口号
```

如果你只想最快跑通，可以先看“快速成功路径”。如果你想理解每一步为什么这么做，就从头按顺序看。

## 1. 项目最终架构

本项目是：

```text
电脑服务器 -> WiFi -> ESP32 -> UART -> STM32
```

三个角色分别是：

```text
电脑服务器：保存 version.json 和 stm32_app.bin，通过 HTTP 提供固件文件，通过 MQTT 触发升级。
ESP32：联网、检查版本、下载固件、通过 UART 把固件转发给 STM32。
STM32：运行 Bootloader 和 App，接收 OTA 数据，写入 Flash，校验后重启升级。
```

当前采用“单 App Target + 三槽 Flash”的 OTA 架构。

Flash 分区如下：

| 区域 | 地址范围 | 大小 | 作用 |
|---|---:|---:|---|
| Bootloader | `0x08000000 - 0x08003FFF` | 16KB | 上电首先运行，负责安装和回滚 |
| App A/run | `0x08004000 - 0x0802BFFF` | 160KB | 真正运行的 App 区 |
| App B/cache | `0x0802C000 - 0x08053FFF` | 160KB | OTA 下载缓存区，不直接运行 |
| App C/back | `0x08054000 - 0x0807BFFF` | 160KB | 旧 App 备份区，用于回滚 |
| OTA info | `0x0807C000 - 0x0807FFFF` | 16KB | 保存 OTA 状态、大小、CRC、版本 |

最重要的一句话：

```text
App 永远链接到 0x08004000，B 区只是下载缓存，不是运行地址。
```

这意味着：

```text
只需要一个 App Target。
只生成一个 stm32_app.bin。
不要再做 App A Target 和 App B Target。
不要把 App 链接到 B 区地址。
```

## 2. 快速成功路径

如果环境都已经装好了，按这个顺序做：

```text
1. 确认电脑 IP，记为 <PC_IP>。
2. 确认 ESP32 与 STM32 接线正确。
3. Keil 烧录 Bootloader 到 0x08000000。
4. Keil 烧录 App 到 0x08004000。
5. 编译并上传 ESP32。
6. 启动 HTTP 固件服务器。
7. 启动 MQTT broker。
8. 修改 App 版本号并生成新的 stm32_app.bin。
9. 更新 server/firmware/version.json。
10. 发送 MQTT check 命令。
11. 看到 stm32/ota/status 返回 success。
```

手动触发 OTA 的命令：

```powershell
& 'C:\Program Files\Mosquitto\mosquitto_pub.exe' -h <PC_IP> -p 1883 -t stm32/ota/check -m check
```

监听 OTA 状态的命令：

```powershell
& 'C:\Program Files\Mosquitto\mosquitto_sub.exe' -h <PC_IP> -p 1883 -t stm32/ota/status
```

成功时你应该看到：

```text
online
check_requested
checking
update_available
success
```

## 3. 目录说明

工程根目录建议这样理解：

```text
<OTA_ROOT>
├─ esp32
│  ├─ platformio.ini
│  └─ src
│     ├─ main.cpp
│     ├─ ota_client.cpp
│     ├─ mqtt_ota_trigger.cpp
│     ├─ stm32_bridge.cpp
│     └─ config.h
├─ stm32
│  ├─ bootloader
│  │  ├─ bootloader.uvprojx
│  │  └─ STM32F103ZETX_BOOT.sct
│  └─ app
│     ├─ app.uvprojx
│     ├─ STM32F103ZETX_APP.sct
│     └─ User
│        ├─ main.c
│        ├─ ota_protocol.c
│        ├─ flash_writer.c
│        └─ version.h
├─ server
│  ├─ serve.py
│  └─ firmware
│     ├─ version.json
│     └─ stm32_app.bin
├─ tools
│  └─ generate_version.py
└─ README.md
```

注意：

```text
真正的 App 工程是 <OTA_ROOT>\stm32\app\app.uvprojx。
不要打开 app\Projects\MDK-ARM 里的旧工程。
```

## 4. 硬件准备

推荐硬件：

```text
STM32F103 高密度芯片，512KB Flash，例如 STM32F103VET6/ZET6。
ESP32 DevKit，例如 ZY-ESP32 DevKit V4 或兼容 ESP32 Dev Module。
ST-Link，用于第一次烧录 Bootloader 和 App。
杜邦线若干。
电脑和 ESP32 连接到同一个局域网。
```

电平要求：

```text
STM32 USART2 是 3.3V TTL。
ESP32 GPIO 也是 3.3V。
两者可以直接连接 UART，但必须共地。
不要接 5V 串口电平。
```

当前项目默认 UART 接线：

| STM32 | 方向 | ESP32 |
|---|---|---|
| PA2 / USART2_TX | STM32 发给 ESP32 | GPIO21 / ESP32 RX |
| PA3 / USART2_RX | ESP32 发给 STM32 | GPIO22 / ESP32 TX |
| GND | 共地 | GND |

一句话记忆：

```text
TX 接 RX，RX 接 TX，GND 一定要接。
STM32 PA2 -> ESP32 RX
STM32 PA3 <- ESP32 TX
```

如果你换了 ESP32 引脚，例如想用 GPIO16/GPIO17，需要改：

```ini
; <OTA_ROOT>\esp32\platformio.ini
build_flags =
    -D UART_TX_PIN=17
    -D UART_RX_PIN=16
```

这里的命名是站在 ESP32 角度：

```text
UART_TX_PIN：ESP32 发出数据的脚，接 STM32 PA3。
UART_RX_PIN：ESP32 接收数据的脚，接 STM32 PA2。
```

ZY-ESP32 DevKit V4 注意事项：

```text
如果板子有 RS485 跳帽，不要让 PA2/PA3 被 RS485 电路占用。
接 PA2/PA3 本体引脚，不要接错到 485R/485T。
如果你用 GPIO21/GPIO22，确认这两个 GPIO 没被别的外设占用。
```

### 4.1 通用硬件适配声明

本文档里的 STM32F103VET6/ZET6、USART2 PA2/PA3、ESP32 GPIO21/GPIO22 是当前工程已经验证通过的一组默认配置，不代表所有 STM32 和 ESP32 都能直接照抄。

如果你更换 STM32 型号、ESP32 型号、开发板、串口引脚或 Flash 容量，必须重新确认下面这些内容：

| 变化项 | 必须重新确认 | 典型修改位置 |
|---|---|---|
| STM32 型号变化 | Flash 容量、SRAM 容量、Flash 页大小、启动地址、Keil Device Pack | Bootloader/App 的 `.sct`、`boot.h`、`ota_protocol.h`、Keil Target |
| STM32 UART 变化 | TX/RX 引脚、GPIO 时钟、USART 时钟、IRQ、复用映射 | `stm32/app/User/uart_handler.h`、`uart_handler.c` |
| STM32 LED 变化 | LED 所在 GPIO 端口、引脚、有效电平 | `stm32/app/User/main.c` |
| ESP32 开发板变化 | 下载串口、可用 GPIO、启动绑带脚、USB 串口占用脚 | `esp32/platformio.ini` |
| ESP32 UART 引脚变化 | ESP32 TX/RX GPIO 与 STM32 RX/TX 的交叉连接 | `UART_TX_PIN`、`UART_RX_PIN` |
| 电脑或网络变化 | 电脑 IP、HTTP 端口、MQTT 端口、防火墙 | `version.json`、`ota_client.cpp`、`mqtt_ota_trigger.cpp`、`mosquitto_ota.conf` |

通用接线规则永远是：

```text
STM32_TX  -> ESP32_RX
STM32_RX  <- ESP32_TX
STM32_GND -- ESP32_GND
```

不要按“引脚名字相同就相连”的方式接线。UART 永远是发送接接收、接收接发送。

换 STM32 后，要先查该芯片的数据手册或参考手册，确认某个 USART 的 TX/RX 到底在哪些引脚上。不同系列差异很大：

| STM32 系列示例 | 引脚处理重点 |
|---|---|
| STM32F1 | 需要注意 AFIO remap，例如 USART1/USART2/USART3 是否重映射 |
| STM32F4/F7/H7 | GPIO 需要配置 Alternate Function 编号，例如 AF7_USARTx |
| STM32G0/G4/L4 | 引脚复用更多，必须查 datasheet 的 alternate function table |
| 小封装 STM32 | 有些 USART 引脚没有引出，不能只看芯片外设名 |

换 ESP32 后，也要查开发板原理图，而不是只看芯片 GPIO 编号：

| ESP32 系列示例 | 引脚处理重点 |
|---|---|
| ESP32 DevKit | GPIO21/GPIO22 通常可用，但仍要看板载外设 |
| ESP32-S2/S3 | USB、串口、板载 RGB、PSRAM/Flash 相关脚可能被占用 |
| ESP32-C3/C6 | GPIO 数量较少，启动绑带脚和 USB/JTAG 脚要特别小心 |
| 带屏幕/摄像头/传感器开发板 | 很多 GPIO 已接外设，不能随便拿来做 UART |

最稳的移植步骤：

```text
1. 先确认 STM32 使用哪个 USART，例如 USART2。
2. 查 STM32 数据手册，确定 USART_TX 和 USART_RX 引脚。
3. 查 ESP32 开发板原理图，选择两个安全 GPIO。
4. 在 platformio.ini 里设置 UART_TX_PIN 和 UART_RX_PIN。
5. 在 STM32 uart_handler.h/c 里设置 USART、GPIO、时钟和 IRQ。
6. 用最小串口测试先证明双向 UART 通。
7. 再跑 CMD_QUERY_VERSION。
8. 最后再跑 OTA。
```

## 5. 软件环境安装

### 5.1 Keil MDK

用途：

```text
编译和烧录 STM32 Bootloader。
编译和烧录 STM32 App。
生成 App 的 axf/hex/bin。
```

需要安装：

```text
Keil MDK-ARM
STM32F1 Device Family Pack
ST-Link 驱动
```

检查方法：

```text
打开 Keil。
能打开 .uvprojx 工程。
能识别 ST-Link。
点击 Build 不报缺少芯片包。
点击 Download 能烧录 STM32。
```

### 5.2 Python

用途：

```text
运行 HTTP 固件服务器。
生成 version.json。
辅助计算 bin 大小和 CRC。
```

检查命令：

```powershell
python --version
```

如果提示找不到 Python，需要安装 Python，并勾选：

```text
Add Python to PATH
```

### 5.3 VSCode + PlatformIO

用途：

```text
编译和上传 ESP32 固件。
打开串口监视器查看 ESP32 日志。
```

检查命令：

```powershell
pio --version
```

如果 `pio` 命令不能直接用，也可以使用完整路径：

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" --version
```

### 5.4 Mosquitto MQTT

用途：

```text
电脑运行 MQTT broker。
ESP32 订阅 stm32/ota/check。
电脑发布 check 命令触发 OTA。
ESP32 发布 stm32/ota/status 反馈状态。
```

推荐安装路径：

```text
C:\Program Files\Mosquitto
```

检查命令：

```powershell
& 'C:\Program Files\Mosquitto\mosquitto_pub.exe' --help
& 'C:\Program Files\Mosquitto\mosquitto_sub.exe' --help
```

## 6. 获取电脑 IP

OTA 服务器运行在电脑上，ESP32 必须知道电脑 IP。

查看电脑 IPv4：

```powershell
ipconfig
```

找到当前连接的 WiFi 或以太网，例如：

```text
IPv4 地址 . . . . . . . . . . . . : 192.168.0.108
```

这个地址记为：

```text
<PC_IP> = 192.168.0.108
```

常见误区：

```text
ESP32 自己也会有一个 IP，例如 192.168.0.xxx。
这个不是服务器 IP。
ESP32 要访问的是电脑 IP，也就是 <PC_IP>。
```

如果换电脑或换 WiFi，通常 `<PC_IP>` 会变。需要同步修改这些地方：

```text
esp32/src/ota_client.cpp       OTA_SERVER_BASE_URL
esp32/src/config.h             OTA_SERVER_BASE_URL / REMOTE_LOG_HOST
esp32/src/mqtt_ota_trigger.cpp MQTT_BROKER_HOST
esp32/mosquitto_ota.conf       listener 1883 <PC_IP>
server/firmware/version.json   latest.url
```

如果你希望以后少改代码，可以把 IP 放到配置文件或环境变量中，这是后续工程化优化方向。

## 7. Keil 设置总原则

Keil 里有两个容易混淆的东西：

```text
Target Dialog 里的 IROM1/IRAM1
Linker 里的 Scatter File
```

简单理解：

```text
如果 Linker 勾选了 Use Memory Layout from Target Dialog，则 Keil 用 Target 页面的 IROM1/IRAM1。
如果 Linker 没勾选这个选项，并且指定了 Scatter File，则 .sct 文件才是最终链接依据。
```

本项目推荐：

```text
Bootloader 和 App 都使用 .sct 文件。
Linker 页面指定对应的 scatter file。
用 map 文件确认最终地址。
```

代码里的地址定义不等于链接地址。

例子：

```c
#define APP_A_ADDR 0x08004000
```

这只表示程序运行时知道 A 区地址是 `0x08004000`。它不会自动让 Keil 把 App 链接到 `0x08004000`。

真正决定 App 向量表和 Reset_Handler 在哪里的，是 Keil Linker 设置。

## 8. Keil 设置 Bootloader

打开工程：

```text
<OTA_ROOT>\stm32\bootloader\bootloader.uvprojx
```

Bootloader 必须链接到：

```text
0x08000000
```

推荐 scatter file：

```text
<OTA_ROOT>\stm32\bootloader\STM32F103ZETX_BOOT.sct
```

关键布局：

```text
LR_IROM1  0x08000000 0x00000400
LR_RAMFUNC 0x08000400 0x00000800, 执行地址 0x20000000
LR_IROM2  0x08000C00 0x00003400
```

为什么要有 RAMFUNC：

```text
STM32 在擦写 Flash 时，不应该从正在操作的 Flash 执行擦写函数。
所以底层 flash_if.o(.ramfunc) 会被复制到 SRAM 执行。
Bootloader 复制 B->A、A->C、C->A、写 OTA_INFO 时都依赖它。
```

Keil 检查项：

```text
Options for Target -> Target:
IROM1 Start 可以显示 0x08000000，Size 可以显示 0x4000。
IRAM1 Start 0x20000000，Size 0x10000。

Options for Target -> Linker:
使用 STM32F103ZETX_BOOT.sct。
如果使用 scatter file，则以 scatter file 为准。
```

编译：

```text
按 F7。
确认 0 Error。
```

烧录：

```text
连接 ST-Link。
按 F8 Download。
```

map 文件验证：

```text
<OTA_ROOT>\stm32\bootloader\Output\bootloader.map
```

应该能看到：

```text
__Vectors = 0x08000000
```

## 9. Keil 设置 App

打开工程：

```text
<OTA_ROOT>\stm32\app\app.uvprojx
```

App 必须链接到：

```text
0x08004000
```

推荐 scatter file：

```text
<OTA_ROOT>\stm32\app\STM32F103ZETX_APP.sct
```

关键布局：

```text
LR_IROM1   0x08004000 0x00000400
LR_RAMFUNC 0x08004400 0x00000800, 执行地址 0x20000000
LR_IROM2   0x08004C00 0x00027400
RW_IRAM1   0x20000800 0x0000F800
```

为什么 App 也要 RAMFUNC：

```text
App 运行期间接收 OTA 数据。
它需要擦除和写入 B 区 Flash。
这些 Flash 操作函数必须放到 SRAM 执行。
```

Keil Target 页可以这样设置：

```text
IROM1 Start = 0x08004000
IROM1 Size  = 0x28000
IRAM1 Start = 0x20000000
IRAM1 Size  = 0x10000
```

但是如果 Linker 使用 `.sct`，最终还是以 `.sct` 为准。

map 文件验证：

```text
<OTA_ROOT>\stm32\app\Output\app.map
```

应该能看到：

```text
__Vectors     = 0x08004000
Reset_Handler = 0x08004xxx
ER_RAMFUNC Exec base = 0x20000000
```

如果 `__Vectors` 不是 `0x08004000`，App 就不能作为当前架构的 OTA 包。

## 10. 第一次烧录 STM32

第一次必须用 ST-Link 烧录，因为 STM32 还没有 Bootloader 和 App。

推荐顺序：

```text
1. 烧录 Bootloader。
2. 烧录 App。
3. 复位 STM32。
4. 确认 App 正常运行。
```

为什么先烧 Bootloader 再烧 App：

```text
MCU 上电从 0x08000000 开始执行。
这个地址必须有 Bootloader。
Bootloader 再跳转到 0x08004000 的 App。
```

Keil Flash Download 注意：

```text
不要随便 Full Chip Erase。
如果烧 App 时 Full Chip Erase，会把 Bootloader 擦掉。
建议 Bootloader 首次烧录可以全片擦除。
之后烧 App 时使用 Sector Erase 或只擦 App 区。
```

如果不确定是否擦掉 Bootloader：

```text
重新烧一次 Bootloader。
再烧一次 App。
```

## 11. STM32 App 代码逻辑

App 入口主要在：

```text
<OTA_ROOT>\stm32\app\User\main.c
```

关键动作：

```text
设置 SCB->VTOR = APP_RUN_ADDR，也就是 0x08004000。
初始化 LED。
初始化 OTA 协议。
初始化 USART2。
循环处理 UART 数据。
定期调用 ota_protocol_tick。
新 App 启动稳定后调用 ota_confirm_running_app。
```

版本号在：

```text
<OTA_ROOT>\stm32\app\User\version.h
```

示例：

```c
#define FW_VERSION_STRING "1.0.3"
```

每次要发布新版本，都必须提高版本号：

```text
1.0.3 -> 1.0.4
```

ESP32 判断是否升级，是比较：

```text
服务器 version.json 里的 latest.version
STM32 CMD_QUERY_VERSION 返回的当前版本
```

如果服务器版本没有更高，不会触发升级。

## 12. STM32 OTA 协议逻辑

主要文件：

```text
<OTA_ROOT>\stm32\app\User\ota_protocol.c
<OTA_ROOT>\stm32\app\User\uart_handler.c
<OTA_ROOT>\stm32\app\User\flash_writer.c
```

ESP32 和 STM32 的协议帧格式：

```text
SOF       1 字节，固定 0xAA
CMD       1 字节
LEN_H     1 字节
LEN_L     1 字节
PAYLOAD   N 字节
CRC16     2 字节
```

常用命令：

| 命令 | 含义 |
|---|---|
| `CMD_QUERY_VERSION` | ESP32 查询 STM32 当前版本 |
| `CMD_REPORT_VERSION` | STM32 返回版本 |
| `CMD_OTA_START` | ESP32 通知 STM32 开始 OTA |
| `CMD_OTA_START_ACK` | STM32 回复是否允许 OTA |
| `CMD_DATA_PACKET` | ESP32 发送一包固件数据 |
| `CMD_DATA_ACK` | STM32 回复该包写入结果 |
| `CMD_OTA_FINISH` | ESP32 通知 STM32 数据发完，开始最终校验 |
| `CMD_OTA_RESULT` | STM32 返回最终校验结果 |
| `CMD_REBOOT` | ESP32 请求 STM32 重启 |

STM32 收到 OTA_START 后：

```text
检查当前是否已经在 OTA。
检查是否处于 PENDING_CONFIRM。
检查固件大小是否 <= 160KB。
写 OTA_INFO = DOWNLOADING。
擦除 App B/cache。
回复 ACK_OK。
```

STM32 收到数据包后：

```text
检查 offset 是否合法。
检查 seq 是否连续。
写入 APP_DOWNLOAD_ADDR + offset。
更新 bytes_written。
回复 DATA_ACK。
```

STM32 收到 OTA_FINISH 后：

```text
检查 reported_size 是否等于 expected_size。
检查 bytes_written 是否等于 expected_size。
计算 B 区 CRC32。
比较 B 区 CRC32 和 ESP32 发来的 expected_crc32。
校验通过后写 OTA_INFO = VERIFIED。
回复 OTA_RESULT = 0。
```

如果最终回复不是 0，ESP32 会显示失败。

## 13. Bootloader 代码逻辑

主要文件：

```text
<OTA_ROOT>\stm32\bootloader\Core\Src\boot.c
<OTA_ROOT>\stm32\bootloader\Core\Src\ota_info.c
<OTA_ROOT>\stm32\bootloader\Core\Src\flash_if.c
```

Bootloader 上电后的核心逻辑：

```text
读取 OTA_INFO。
如果 OTA_INFO = VERIFIED，说明 B 区有新固件。
校验 B 区向量表。
校验 B 区 CRC。
把当前 A 区备份到 C 区。
把 B 区复制到 A 区。
写 OTA_INFO = PENDING_CONFIRM。
跳转到 A 区运行。
```

新 App 启动后：

```text
App 运行稳定一段时间。
App 写 OTA_INFO = IDLE。
Bootloader 下次上电认为新 App 已确认成功。
```

如果新 App 跑不起来：

```text
Bootloader 看到 PENDING_CONFIRM 没有被确认。
如果启动尝试次数超限，就把 C 区备份恢复到 A 区。
```

这就是回滚。

仍需注意：

```text
如果新 App 死循环但不复位，Bootloader 没有机会重新接管。
这种情况需要加独立看门狗 IWDG。
```

## 14. ESP32 工程配置

打开：

```text
<OTA_ROOT>\esp32
```

PlatformIO 配置文件：

```text
<OTA_ROOT>\esp32\platformio.ini
```

当前关键配置：

```ini
upload_port = COM7
monitor_port = COM7
monitor_speed = 115200

build_flags =
    -D USE_USB_RXTX_FOR_STM32=0
    -D UART_TX_PIN=22
    -D UART_RX_PIN=21
    -D REMOTE_LOG_ENABLE=0
    -D STM32_RX_PROBE_ENABLE=0
    -D UART_BAUD=115200
```

如果你的 ESP32 串口不是 COM7，改成自己的：

```ini
upload_port = <ESP32_COM>
monitor_port = <ESP32_COM>
```

查看串口方法：

```text
Windows 设备管理器 -> 端口 COM 和 LPT。
拔掉 ESP32 看哪个 COM 消失。
插上 ESP32 看哪个 COM 出现。
```

如果你换了 UART 引脚：

```ini
-D UART_TX_PIN=<ESP32_TX_GPIO>
-D UART_RX_PIN=<ESP32_RX_GPIO>
```

然后按这个规则接线：

```text
ESP32 UART_TX_PIN -> STM32 PA3
ESP32 UART_RX_PIN <- STM32 PA2
```

## 15. ESP32 代码逻辑

主要文件：

```text
<OTA_ROOT>\esp32\src\main.cpp
<OTA_ROOT>\esp32\src\ota_client.cpp
<OTA_ROOT>\esp32\src\mqtt_ota_trigger.cpp
<OTA_ROOT>\esp32\src\stm32_bridge.cpp
<OTA_ROOT>\esp32\src\config.h
```

`main.cpp` 负责：

```text
初始化串口日志。
连接 WiFi。
初始化 STM32 UART bridge。
初始化 OTA client。
初始化 MQTT trigger。
循环处理 MQTT 和心跳。
```

`mqtt_ota_trigger.cpp` 负责：

```text
连接 MQTT broker。
订阅 stm32/ota/check。
收到 check 后立刻调用 OTA 检查。
发布 stm32/ota/status 状态。
```

`ota_client.cpp` 负责：

```text
HTTP GET version.json。
解析 version、url、size、crc32。
查询 STM32 当前版本。
比较版本号。
下载固件。
通过 UART 分包发送给 STM32。
ESP32 本地计算下载流 CRC32。
通知 STM32 最终校验。
成功后请求 STM32 重启。
```

`stm32_bridge.cpp` 负责：

```text
UART 协议帧收发。
等待 STM32 ACK。
查询版本。
发送 OTA_START。
发送 DATA_PACKET。
发送 OTA_FINISH。
发送 REBOOT。
保存最近一次 STM32 通信错误。
```

## 16. UART 分包大小

当前 ESP32 发送固件数据时，每包数据大小是：

```text
500 字节
```

协议 payload 是：

```text
seq(2) + offset(4) + data(500) = 506 字节
```

为什么不是 506 字节数据：

```text
协议理论最大 payload 是 512 字节。
如果 data=506，则 payload 正好 512。
实际排查中发现顶到 512 边界时，STM32 B 区写入内容会被污染。
所以工程上保留余量，改为 data=500。
```

当前代码在：

```text
<OTA_ROOT>\esp32\src\ota_client.cpp
```

关键定义：

```cpp
#define STM32_OTA_DATA_CHUNK_SIZE  500U

#undef PROTOCOL_MAX_PAYLOAD
#define PROTOCOL_MAX_PAYLOAD       (STM32_OTA_DATA_CHUNK_SIZE + 6U)
```

不要随意改回 506。

不同大小 bin 的处理方式：

```text
如果 bin = 8948 字节：
17 包 x 500 字节 + 1 包 x 448 字节。

如果 bin = 160000 字节：
320 包 x 500 字节。

如果最后一包不足 500 字节：
发送剩余字节。
```

只要：

```text
bin 大小 > 0
bin 大小 <= 160KB
version.json 的 size 和 crc32 正确
```

就可以正常处理。

## 17. 配置 HTTP 固件服务器

固件服务器在：

```text
<OTA_ROOT>\server\serve.py
```

启动命令：

```powershell
cd <OTA_ROOT>\server
python serve.py
```

默认监听：

```text
0.0.0.0:8080
```

这表示电脑所有网卡都监听 8080。

验证：

```powershell
Invoke-WebRequest -Uri "http://<PC_IP>:8080/firmware/version.json" -UseBasicParsing
```

成功时应该返回 HTTP 200，并显示 JSON。

如果失败：

```text
检查 Python 服务是否在运行。
检查 <PC_IP> 是否写错。
检查 Windows 防火墙是否拦截 8080。
检查电脑和 ESP32 是否在同一个 WiFi。
```

## 18. 配置 MQTT broker

Mosquitto 配置文件：

```text
<OTA_ROOT>\esp32\mosquitto_ota.conf
```

示例：

```text
listener 1883 <PC_IP>
allow_anonymous true
```

启动命令：

```powershell
& 'C:\Program Files\Mosquitto\mosquitto.exe' -c <OTA_ROOT>\esp32\mosquitto_ota.conf -v
```

验证监听：

```powershell
Get-NetTCPConnection -LocalPort 1883
```

应该看到：

```text
<PC_IP> 1883 Listen
```

如果 ESP32 日志显示：

```text
MQTT connect failed
```

重点检查：

```text
Mosquitto 是否启动。
mosquitto_ota.conf 里的 IP 是否是电脑 IP。
Windows 防火墙是否放行 1883。
ESP32 代码里的 MQTT_BROKER_HOST 是否是电脑 IP。
```

## 19. 编译和上传 ESP32

进入 ESP32 工程：

```powershell
cd <OTA_ROOT>\esp32
```

编译：

```powershell
pio run
```

如果 `pio` 不在 PATH：

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run
```

上传：

```powershell
pio run -t upload
```

或：

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t upload
```

打开串口监视器：

```powershell
pio device monitor -b 115200
```

或：

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor -b 115200
```

正常日志示例：

```text
WiFi connected! IP: 192.168.0.xxx
[OTA] MQTT connected, subscribed: stm32/ota/check
[OTA] [DIAG] Valid STM32 version response: 1.0.3
STM32 connected OK
```

## 20. 生成 STM32 OTA bin

OTA 需要的是一个连续的 `stm32_app.bin`。

注意：

```text
App 工程有多个 Load Region：
LR_IROM1
LR_RAMFUNC
LR_IROM2
```

不能随便拿某一个 region 的 bin 当 OTA 包。

正确的 OTA bin 必须：

```text
以 0x08004000 为基准。
包含向量表。
包含 RAMFUNC 的 Flash load 数据。
包含普通 RO 数据。
中间空洞用 0xFF 填充。
```

生成后放到：

```text
<OTA_ROOT>\server\firmware\stm32_app.bin
```

校验 bin 是否合理：

```powershell
python -c "import pathlib,struct,zlib;p=pathlib.Path(r'<OTA_ROOT>\server\firmware\stm32_app.bin');b=p.read_bytes();print(len(b),hex(zlib.crc32(b)&0xffffffff));print([hex(x) for x in struct.unpack('<II',b[:8])])"
```

你应该看到：

```text
第一个 word 是 SRAM 地址，例如 0x2000xxxx。
第二个 word 是 Thumb Reset_Handler，例如 0x08004xxx，最低位通常为 1。
```

判断规则：

```text
SP 必须在 0x20000000 - 0x20010000。
Reset_Handler 去掉最低位后必须在 0x08004000 - 0x0802BFFF。
bin 大小必须 <= 160KB。
```

如果 Reset_Handler 是 `0x080000xx`，说明 App 被错误链接到了 Bootloader 地址。

如果 Reset_Handler 是 B 区地址，说明你错误地把 App 链接到了缓存区。

## 21. 更新 version.json

服务器清单文件：

```text
<OTA_ROOT>\server\firmware\version.json
```

示例：

```json
{
  "latest": {
    "version": "1.0.4",
    "filename": "stm32_app.bin",
    "url": "http://<PC_IP>:8080/firmware/stm32_app.bin",
    "size": 12345,
    "crc32": "A1B2C3D4",
    "target_mcu": "STM32F103VET6",
    "min_bootloader_version": 1,
    "changelog": "Describe what changed"
  },
  "history": []
}
```

字段含义：

| 字段 | 含义 |
|---|---|
| `version` | 新版本号，必须大于 STM32 当前版本 |
| `filename` | 固件文件名 |
| `url` | ESP32 下载 bin 的完整 HTTP URL |
| `size` | bin 文件字节数 |
| `crc32` | bin 文件 CRC32，十六进制字符串 |
| `target_mcu` | 目标芯片 |
| `min_bootloader_version` | 最低 Bootloader 版本，目前固定 1 |
| `changelog` | 更新说明 |

可以用工具生成：

```powershell
cd <OTA_ROOT>
python tools\generate_version.py server\firmware\stm32_app.bin 1.0.4 http://<PC_IP>:8080
```

注意：

```text
如果工具里的默认 IP 不是你的电脑 IP，请手动传入 http://<PC_IP>:8080。
```

## 22. 发布一个新版本的完整例子

目标：

```text
把 STM32 App 从 1.0.3 升到 1.0.4。
```

步骤 1：修改业务代码。

例如修改 LED 行为：

```text
<OTA_ROOT>\stm32\app\User\main.c
```

步骤 2：修改版本号。

```text
<OTA_ROOT>\stm32\app\User\version.h
```

修改：

```c
#define FW_VERSION_STRING "1.0.4"
```

步骤 3：Keil 编译 App。

```text
打开 <OTA_ROOT>\stm32\app\app.uvprojx
按 F7
确认 0 Error
确认 map 文件中 __Vectors = 0x08004000
```

步骤 4：生成新的 `stm32_app.bin`。

```text
输出到 <OTA_ROOT>\server\firmware\stm32_app.bin
```

步骤 5：更新 version.json。

```powershell
cd <OTA_ROOT>
python tools\generate_version.py server\firmware\stm32_app.bin 1.0.4 http://<PC_IP>:8080
```

步骤 6：启动 HTTP 服务器。

```powershell
cd <OTA_ROOT>\server
python serve.py
```

步骤 7：启动 MQTT broker。

```powershell
& 'C:\Program Files\Mosquitto\mosquitto.exe' -c <OTA_ROOT>\esp32\mosquitto_ota.conf -v
```

步骤 8：触发升级。

```powershell
& 'C:\Program Files\Mosquitto\mosquitto_pub.exe' -h <PC_IP> -p 1883 -t stm32/ota/check -m check
```

步骤 9：观察状态。

```powershell
& 'C:\Program Files\Mosquitto\mosquitto_sub.exe' -h <PC_IP> -p 1883 -t stm32/ota/status
```

成功结果：

```text
check_requested
checking
update_available
success
```

步骤 10：确认版本。

再次触发 check，或者看 ESP32 串口日志：

```text
STM32 version: 1.0.4
Already up to date
```

## 23. 不同电脑如何迁移

换电脑时，最容易错的是路径、IP、COM 口。

需要重新确认：

```text
1. <OTA_ROOT> 是否变了。
2. <PC_IP> 是否变了。
3. ESP32 COM 口是否变了。
4. ST-Link 是否能识别。
5. Keil 是否安装 STM32F1 Pack。
6. Python 是否在 PATH。
7. PlatformIO 是否能编译 ESP32。
8. Mosquitto 是否安装。
9. Windows 防火墙是否放行 8080 和 1883。
```

必须修改：

```text
platformio.ini 中的 upload_port 和 monitor_port。
所有写死的 <PC_IP>。
mosquitto_ota.conf 中的 listener IP。
version.json 中的固件 URL。
```

建议不要在文档或代码中写个人用户名路径。

推荐写法：

```text
<OTA_ROOT>\server\firmware\stm32_app.bin
```

不要写：

```text
C:\Users\某个人\Desktop\OTA\server\firmware\stm32_app.bin
```

## 24. 不同 ESP32 硬件如何适配

只要是普通 ESP32 Dev Module，大多可以用 PlatformIO 的：

```ini
board = esp32dev
```

如果 USB 转串口芯片不同：

```text
可能需要安装 CP210x、CH340 或 FTDI 驱动。
COM 口可能不同。
```

如果 GPIO21/GPIO22 被占用：

```text
可以换到 GPIO16/GPIO17、GPIO25/GPIO26、GPIO32/GPIO33 等普通 GPIO。
```

修改：

```ini
-D UART_TX_PIN=<新 ESP32 TX GPIO>
-D UART_RX_PIN=<新 ESP32 RX GPIO>
```

这里的 `<新 ESP32 TX GPIO>` 和 `<新 ESP32 RX GPIO>` 是站在 ESP32 角度命名的：

```text
ESP32 TX GPIO：ESP32 发数据，接 STM32 RX。
ESP32 RX GPIO：ESP32 收数据，接 STM32 TX。
```

举例：

```ini
-D UART_TX_PIN=17
-D UART_RX_PIN=16
```

对应接线：

```text
ESP32 GPIO17 -> STM32 UART_RX
ESP32 GPIO16 <- STM32 UART_TX
```

不要写完 `UART_TX_PIN=17` 后又把 GPIO17 接到 STM32 TX，这样就是 TX 对 TX，会不通。

避免使用：

```text
GPIO0，启动模式相关。
GPIO2，启动状态可能受影响。
GPIO12，部分板子影响 Flash 电压启动配置。
GPIO34-39，只能输入，不能作为 TX。
GPIO1/GPIO3，如果你还想保留 USB 串口日志，就不要拿它们连 STM32。
```

如果你必须用 GPIO1/GPIO3：

```text
ESP32 USB 日志会和 STM32 UART 冲突。
需要关闭 USB 日志或改用 UDP 远程日志。
不建议初学者这么做。
```

ESP32 选引脚时建议按这个表判断：

| GPIO 类型 | 是否推荐做 STM32 UART | 原因 |
|---|---|---|
| 普通可输入输出 GPIO | 推荐 | 最简单稳定 |
| 启动绑带脚 | 不推荐 | 上电电平会影响启动模式 |
| 只输入 GPIO | 只能做 RX，不能做 TX | 无法输出 UART |
| USB 串口默认 GPIO1/GPIO3 | 初学者不推荐 | 会和日志、下载串口冲突 |
| 已连接板载外设的 GPIO | 不推荐 | 可能被 LED、屏幕、传感器、PSRAM 占用 |

换 ESP32 芯片或开发板时，需要改的最小集合：

```text
esp32/platformio.ini:
  board
  upload_port
  monitor_port
  UART_TX_PIN
  UART_RX_PIN

esp32/src/mqtt_ota_trigger.cpp:
  MQTT_BROKER_HOST，如果电脑 IP 变化

esp32/src/ota_client.cpp:
  OTA_SERVER_BASE_URL，如果电脑 IP 变化

esp32/mosquitto_ota.conf:
  listener 1883 <PC_IP>
```

如果你用的是 ESP32-S3、ESP32-C3、ESP32-C6 等非传统 ESP32，还要确认 PlatformIO 的 `board` 是否正确。例如：

```ini
board = esp32-s3-devkitc-1
```

具体 board 名称以 PlatformIO 支持列表为准。不要在硬件已经换成 S3/C3/C6 后还盲目使用 `esp32dev`，否则可能编译、下载、串口或 Flash 参数都不匹配。

## 25. 不同 STM32 硬件如何适配

当前分区只适合：

```text
512KB Flash
64KB SRAM
STM32F103 高密度系列
```

如果你的 STM32 Flash 不是 512KB，不能直接套用。

例如 256KB Flash：

```text
Bootloader 16KB
App A 可能只能 80KB
App B 80KB
App C 可能放不下
OTA_INFO 也要重新安排
```

必须同步修改：

```text
Bootloader boot.h 分区定义
App ota_protocol.h 分区定义
Bootloader scatter file
App scatter file
server/serve.py 中的 APP_PARTITION_SIZE
ESP32 OTA_MAX_FIRMWARE_SIZE
Keil Target/Linker 设置
```

换 STM32 时最重要的是先重新设计 Flash 分区。不要只改 Keil 的 IROM1，也不要只改 C 代码里的宏。

需要同时一致的地方：

| 配置位置 | 必须一致的内容 |
|---|---|
| Bootloader `boot.h` | Bootloader、A、B、C、OTA_INFO 地址和大小 |
| App `ota_protocol.h` | A、B、C、OTA_INFO 地址和大小 |
| Bootloader `.sct` | Bootloader 链接地址和可用 Flash 大小 |
| App `.sct` | App 运行链接地址和可用 Flash 大小 |
| Keil Target | IROM1/IRAM1 显示值，或确认 scatter file 生效 |
| ESP32 `ota_client.cpp` | `OTA_MAX_FIRMWARE_SIZE` |
| server `serve.py` | `APP_RUN_ADDR`、`APP_PARTITION_SIZE`、SRAM 范围 |
| `version.json` | `target_mcu`、size、crc32、url |

一个安全的分区设计规则：

```text
Bootloader 区要能放下 Bootloader。
App A 是最终运行区。
App B 至少要能放下一个完整 App 镜像。
App C 如果要支持回滚，至少要能备份一个完整 App。
OTA_INFO 单独放在最后若干页，避免和 App 区重叠。
所有分区边界最好按 Flash 页大小对齐。
```

示例一：512KB Flash 可以采用当前三槽方案：

```text
Bootloader 16KB
App A      160KB
App B      160KB
App C      160KB
OTA_INFO   16KB
总计       512KB
```

示例二：256KB Flash 不一定适合三槽回滚：

```text
Bootloader 16KB
App A      96KB
App B      96KB
OTA_INFO   8KB 或 16KB
剩余空间很少，可能放不下 App C
```

如果没有 App C：

```text
可以做“下载缓存 + 覆盖安装”，但回滚能力会变弱。
复制 A 区时掉电风险要重新设计。
```

示例三：128KB Flash 通常不建议套用本项目完整三槽方案：

```text
Bootloader + A + B + C + OTA_INFO 很难都放下。
需要缩小 App、取消 C 区、或者改成外部 Flash 缓存。
```

SRAM 也要检查：

```text
当前工程按 64KB SRAM 设计，SRAM_END = 0x20010000。
如果芯片只有 20KB SRAM，例如部分 STM32F103C8T6，IRAM1 大小和栈/堆都要重算。
server/serve.py 里的固件向量校验也要改 SRAM_END。
```

如果你换 UART：

```text
当前 STM32 使用 USART2 PA2/PA3。
如果换 USART1 或 USART3，需要改 uart_handler.h、GPIO/RCC/IRQ。
```

STM32 UART 适配要同时改这些点：

```text
USART 外设编号，例如 USART1、USART2、USART3。
TX GPIO 端口和引脚。
RX GPIO 端口和引脚。
GPIO 时钟。
USART 外设时钟。
USART IRQn。
中断服务函数，例如 USART2_IRQHandler。
是否需要 AFIO remap 或 Alternate Function 配置。
```

以 STM32F1 为例：

```text
USART1 默认常见 PA9/PA10。
USART2 默认常见 PA2/PA3。
USART3 默认常见 PB10/PB11。
部分引脚组合需要 AFIO 重映射。
最终以你手上芯片 datasheet 和原理图为准。
```

以 STM32F4/F7/H7/G4/L4 等系列为例：

```text
不仅要设置 GPIO 模式，还要设置 Alternate Function 编号。
例如某些系列 USARTx 常用 AF7，但不能假设所有型号都一样。
必须查 datasheet 的 AF table。
```

如果你换 LED：

```text
改 main.c 中 led_init 和对应 GPIO。
```

换 STM32 型号后的最低验证流程：

```text
1. 先只烧 Bootloader，确认能从 0x08000000 启动。
2. 再烧 App，确认 App 的 __Vectors = 新的 App 运行地址。
3. 用最小 UART 测试确认 ESP32 能收到 STM32 字符串。
4. 用 CMD_QUERY_VERSION 确认协议收发。
5. 用一个很小的新版本 bin 先测 OTA。
6. 再测试接近最大 App 大小的 bin。
7. 最后测试掉电恢复和回滚。
```

## 26. 常见问题排查

### 26.1 ESP32 提示 STM32 not responding

现象：

```text
WARNING: STM32 not responding, check UART wiring
```

判断：

```text
这条日志表示 ESP32 没收到合法协议响应。
它不直接等于 TX/RX 接错。
先判断“完全没字节”还是“有字节但协议解析失败”。
```

按顺序检查：

```text
1. STM32 是否已经烧 Bootloader 和 App。
2. STM32 App 是否真的运行。
3. PA2 是否接到 ESP32 RX。
4. PA3 是否接到 ESP32 TX。
5. GND 是否共地。
6. platformio.ini 的 UART_TX_PIN/UART_RX_PIN 是否对应实际接线。
7. STM32 是否被 RS485 跳帽占用 PA2/PA3。
8. 波特率是否都是 115200。
```

分层处理：

```text
完全收不到 raw bytes：
回到 TX/RX/GND/跳帽/电平/波特率检查。

能收到 raw bytes，但没有 version response：
检查协议帧格式、版本 payload 长度、CRC16。

能收到 Valid STM32 version response：
UART 基础链路已经通过，不要继续把主因停留在接线。
继续查 HTTP、MQTT、OTA 状态机和 Flash 写入。
```

如果 ESP32 能收到版本：

```text
Valid STM32 version response
```

说明 UART 基本通。

### 26.2 MQTT connected 但 HTTP GET failed

现象：

```text
[OTA] MQTT connected, subscribed: stm32/ota/check
[OTA] MQTT OTA check requested
[OTA] Checking for updates: http://旧IP:8080/firmware/version.json
[OTA] ERROR: HTTP GET failed
```

判断：

```text
MQTT 通，只说明 ESP32 收到了升级触发命令。
HTTP failed 说明 ESP32 没拿到 version.json。
这不是 STM32 问题，也不是 UART 问题。
```

检查：

```text
version.json URL 是否是 http://<PC_IP>:8080。
server/serve.py 是否运行。
Windows 防火墙是否允许 8080。
ESP32 和电脑是否在同一个局域网。
```

电脑上验证：

```powershell
Invoke-WebRequest -Uri "http://<PC_IP>:8080/firmware/version.json" -UseBasicParsing
```

处理标准：

```text
ESP32 日志中的 version.json IP 必须等于电脑当前 IP。
version.json 里的 stm32_app.bin URL 也必须等于电脑当前 IP。
电脑本机访问 version.json 必须返回 HTTP 200。
```

### 26.3 MQTT connect failed

现象：

```text
[OTA] MQTT connect failed
```

判断：

```text
ESP32 没连上 Mosquitto。
此时还没有进入 HTTP 检查，也没有进入 STM32 OTA。
```

检查：

```text
Mosquitto 是否启动。
监听 IP 是否是 <PC_IP>。
ESP32 代码里的 MQTT_BROKER_HOST 是否是 <PC_IP>。
防火墙是否放行 1883。
```

验证：

```powershell
Get-NetTCPConnection -LocalPort 1883
```

预期：

```text
<PC_IP> 1883 Listen
```

### 26.4 update_available 后 OTA finish failed

现象：

```text
[OTA] Update available
[OTA] Progress: 100%
[OTA] Finishing OTA
[OTA] ERROR: OTA verify FAILED
```

判断：

```text
ESP32 已经完成版本比较。
ESP32 已经进入固件发送流程。
失败点在 STM32 最终校验阶段。
下一步不要先改服务器，也不要先改 MQTT。
先确认 STM32 B 区实际内容是否等于服务器 bin。
```

检查：

```text
ESP32 日志里的 finish ret 和 result。
version.json 的 size 和 crc32 是否对应当前 stm32_app.bin。
STM32 B 区读回 CRC 是否和服务器 bin 一致。
UART 分包大小是否仍然是 500。
```

处理：

```text
如果 finish ret=0：
ESP32 没收到 STM32 最终响应，查 UART 最后一帧。

如果 finish ret=1 且 result=0x01：
STM32 主动返回校验失败，读回 B 区做 CRC。

如果 B 区 CRC 和服务器 bin 不同：
查分包边界、Flash 写入、接收缓冲区。

如果 B 区 CRC 和服务器 bin 相同：
查 OTA_INFO 写入、Bootloader 安装和确认状态。
```

### 26.5 版本号改了但不升级

现象：

```text
触发 MQTT 后没有进入 update_available。
或者 ESP32 显示 already up to date。
```

判断：

```text
ESP32 只在服务器版本高于 STM32 当前版本时升级。
只改代码但不改版本号，不会触发 OTA。
只改 version.json 但没有替换 bin，会下载旧固件。
```

检查：

```text
STM32 当前版本是多少。
version.json 里的 latest.version 是多少。
服务器版本必须更高。
```

例子：

```text
当前 STM32 = 1.0.3
服务器 latest = 1.0.3
不会升级。

当前 STM32 = 1.0.3
服务器 latest = 1.0.4
会升级。
```

### 26.6 烧 App 后 Bootloader 不见了

现象：

```text
单独烧录 App 后，STM32 不再先进 Bootloader。
OTA_INFO、Bootloader 或跳转逻辑表现异常。
```

直接原因：

```text
Keil 烧 App 时执行了 Full Chip Erase。
全片擦除会把 0x08000000 的 Bootloader 一起擦掉。
```

处理：

```text
重新烧录 Bootloader。
再烧录 App。
```

建议：

```text
Bootloader 首次烧录可以全片擦除。
App 调试烧录尽量只擦除相关扇区。
```

### 26.7 App 跑飞或 LED 状态异常

现象：

```text
Bootloader 能运行，但跳转 App 后无响应。
LED 状态和 App 代码不一致。
串口版本查询失败或随机复位。
```

判断：

```text
这类问题优先检查 App 链接地址和向量表。
不要只看 Keil 是否烧录成功。
```

重点检查：

```text
App 是否链接到 0x08004000。
SCB->VTOR 是否设置为 APP_RUN_ADDR。
Bootloader 是否正确跳转。
Reset_Handler 是否在 0x08004000 - 0x0802BFFF。
```

验证：

```text
打开 app.map。
确认 __Vectors = 0x08004000。
确认 Reset_Handler = 0x08004xxx。
确认 SCB->VTOR = APP_RUN_ADDR。
```

### 26.8 OTA 显示 success，但新功能没有出现

现象：

```text
MQTT status 显示 success。
但是 STM32 行为看起来还是旧版本。
```

判断：

```text
success 表示 ESP32 到 STM32 App 的下载和 finish 阶段通过。
还要确认 Bootloader 已经安装 B->A，并且新 App 已经从 A 区运行。
```

检查：

```text
STM32 重启后版本是否变成新版本。
新功能是否真的编进 stm32_app.bin。
server/firmware/stm32_app.bin 是否是刚生成的新文件。
version.json 的 size 和 crc32 是否对应这个新 bin。
Bootloader 是否执行了 VERIFIED -> PENDING_CONFIRM -> IDLE。
```

处理：

```text
重新生成 App bin。
重新计算 size 和 crc32。
更新 version.json。
触发 MQTT check。
重启后再查询 STM32 版本。
```

## 27. 推荐验证命令集合

查看电脑 IP：

```powershell
ipconfig
```

检查 HTTP：

```powershell
Invoke-WebRequest -Uri "http://<PC_IP>:8080/firmware/version.json" -UseBasicParsing
```

检查端口监听：

```powershell
Get-NetTCPConnection -LocalPort 8080,1883
```

监听 MQTT 状态：

```powershell
& 'C:\Program Files\Mosquitto\mosquitto_sub.exe' -h <PC_IP> -p 1883 -t stm32/ota/status
```

触发 OTA：

```powershell
& 'C:\Program Files\Mosquitto\mosquitto_pub.exe' -h <PC_IP> -p 1883 -t stm32/ota/check -m check
```

编译 ESP32：

```powershell
cd <OTA_ROOT>\esp32
pio run
```

上传 ESP32：

```powershell
cd <OTA_ROOT>\esp32
pio run -t upload
```

计算 bin CRC：

```powershell
python -c "import pathlib,zlib;p=pathlib.Path(r'<OTA_ROOT>\server\firmware\stm32_app.bin');b=p.read_bytes();print(len(b),f'{zlib.crc32(b)&0xffffffff:08X}')"
```

## 28. 初学者最容易混淆的 8 件事

1. `APP_A_ADDR` 是代码运行时使用的地址，不等于 Keil 链接地址。

2. App B 是下载缓存区，不是当前架构的运行区。

3. 只有一个 App Target，不需要 App A Target 和 App B Target。

4. `version.json` 的 IP 是电脑 IP，不是 ESP32 IP。

5. MQTT 触发升级只是“叫 ESP32 去检查”，真正 bin 文件是通过 HTTP 下载。

6. ESP32 本地 CRC 通过，只能说明 HTTP 下载到 ESP32 的数据正确，不代表 STM32 Flash 写入一定正确。

7. 每包 UART 数据不要顶满 512 payload，当前稳定值是 500 数据字节。

8. App 修改后如果版本号没提高，ESP32 会认为已经是最新，不会升级。

## 29. 建议的开发习惯

每次改动后记录：

```text
改了哪个 STM32 功能。
version.h 从多少改到多少。
生成的 stm32_app.bin 大小。
生成的 CRC32。
version.json 是否同步。
OTA 是否 success。
升级后 STM32 查询版本是多少。
```

每次遇到问题按层排查：

```text
硬件接线
STM32 是否运行
ESP32 串口是否通
WiFi 是否连接
MQTT 是否连接
HTTP 是否能访问
版本号是否更高
bin size/CRC 是否正确
UART 分包是否 ACK
STM32 finish 是否通过
Bootloader 是否安装并跳转
```

不要一上来把所有层都混在一起查。一次只验证一层，会快很多。

## 30. 当前稳定参数汇总

| 参数 | 当前值 |
|---|---|
| STM32 UART | USART2 PA2/PA3 |
| ESP32 RX | GPIO21 |
| ESP32 TX | GPIO22 |
| UART 波特率 | 115200 |
| HTTP 端口 | 8080 |
| MQTT 端口 | 1883 |
| MQTT 触发 topic | `stm32/ota/check` |
| MQTT 状态 topic | `stm32/ota/status` |
| MQTT 触发消息 | `check` |
| App 链接地址 | `0x08004000` |
| OTA 下载缓存 | `0x0802C000` |
| 单包数据大小 | 500 bytes |
| App 最大大小 | 160KB |

如果你只记住三句话：

```text
App 只链接到 0x08004000。
服务器 IP 必须是电脑 IP。
UART OTA 每包数据保持 500 字节，不要顶满 512 payload。
```
