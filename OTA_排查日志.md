# STM32 + ESP32 OTA 详细排查复盘报告

工程路径示例：`C:\Users\xx\Desktop\OTA`

这份报告用于交接和复盘本次 STM32 + ESP32 OTA 从“不响应/升级失败”到“最终 success”的完整排查过程。

文档不是按聊天顺序记录，而是按“故障卡片”整理。每个 ISSUE 都按同一套阅读顺序写，方便后来接手的人直接判断故障位置：

```text
这个问题到底是什么？
用户现场看到什么？
为什么要这样排查？
关键日志或证据是什么？
这些证据排除了哪些方向？
根因是什么？
改了哪里？
怎么验证已经修好？
```

阅读时不要先盯代码细节，先看每个 ISSUE 前三段：

```text
先理解现象。
再理解为什么排查方向要转移。
最后再看证据、根因和修复。
```

## 1. 最终结论

本次 OTA 已经跑通。最后一次 MQTT 触发状态为：

```text
online
check_requested
checking
update_available
success
```

当前发布固件：

```text
firmware: <OTA_ROOT>\server\firmware\stm32_app.bin
version : 1.0.3
size    : 8948 bytes
crc32   : 77BAF9AE
url     : http://192.168.0.108:8080/firmware/stm32_app.bin
```

最终确认：本次不是单一故障，而是多个问题叠加。真正导致 OTA finish 失败的核心故障是：

```text
ESP32 旧分包策略把 UART 协议 payload 顶到 512 字节上限。
STM32 B 区写入后，每个 506 字节数据包末尾 4 字节被固定污染。
最终 STM32 计算 B 区 CRC 与服务器 bin CRC 不一致，因此 finish 返回失败。
```

最终修复：

```text
ESP32 每包固件数据从 506 字节降到 500 字节。
协议 payload 从 512 字节降到 506 字节。
避开满 payload 边界后 OTA 成功。
```

## 2. 当前稳定架构

当前项目采用：

```text
单 App Target + A/B/C 三槽 Flash 存储
```

它不是“双运行区 A/B 直接跳转”架构。

### 2.1 Flash 分区示意

```text
STM32 Flash: 512KB

0x08000000
┌────────────────────────────────────┐
│ Bootloader                         │ 16KB
│ MCU 上电入口，负责 OTA 安装和回滚    │
└────────────────────────────────────┘
0x08004000
┌────────────────────────────────────┐
│ App A / run                        │ 160KB
│ 唯一真正运行的 App 区               │
│ App 永远链接到这里                  │
└────────────────────────────────────┘
0x0802C000
┌────────────────────────────────────┐
│ App B / cache                      │ 160KB
│ OTA 下载缓存区，不直接运行           │
│ 保存“将来要复制到 A 区”的镜像        │
└────────────────────────────────────┘
0x08054000
┌────────────────────────────────────┐
│ App C / backup                     │ 160KB
│ 升级前备份旧 A 区，用于失败回滚       │
└────────────────────────────────────┘
0x0807C000
┌────────────────────────────────────┐
│ OTA_INFO                           │ 16KB
│ 保存 OTA 状态、版本、大小、CRC、次数  │
└────────────────────────────────────┘
0x08080000
```

最重要的规则：

```text
App 永远链接到 0x08004000。
B 区只是下载缓存。
C 区只是旧 App 备份。
Bootloader 负责把 B 区新固件复制到 A 区运行。
```

### 2.2 OTA 正常数据流

```text
电脑 HTTP/MQTT server
        |
        | MQTT: stm32/ota/check
        v
ESP32 收到 check
        |
        | HTTP GET version.json
        | HTTP GET stm32_app.bin
        v
ESP32 下载固件并计算本地 CRC
        |
        | UART 分包发送
        v
STM32 App 写入 B/cache
        |
        | CMD_OTA_FINISH
        v
STM32 App 校验 B 区 CRC
        |
        | OTA_INFO = VERIFIED
        v
STM32 软复位
        |
        v
Bootloader 上电安装 B -> A
        |
        v
新 App 从 A 区运行并确认
```

## 3. 排查原则

这类 OTA 问题不能混在一起查。必须按层排除：

```text
1. STM32 是否运行。
2. UART 基础收发是否存在。
3. 协议帧是否能收发。
4. STM32 当前版本是否能查询。
5. Keil 链接地址是否正确。
6. HTTP 固件服务器是否可访问。
7. MQTT 是否能触发 ESP32 检查。
8. ESP32 是否完整下载 bin。
9. STM32 是否 ACK 每个数据包。
10. STM32 B 区实际内容是否等于服务器 bin。
11. Bootloader 是否安装和跳转。
```

核心原则：

```text
硬件通，不代表协议通。
协议通，不代表 Flash 写入正确。
每包 ACK，不代表整包镜像 CRC 正确。
ESP32 本地 CRC 正确，不代表 STM32 B 区 CRC 正确。
最终必须以 STM32 finish 校验或 ST-LINK 读回校验为准。
```

## 4. 排查节点-01：把 `STM32 not responding` 拆成“无字节”和“协议无效”两类

### 触发日志

ESP32 曾打印：

```text
WARNING: STM32 not responding, check UART wiring
```

这条日志的字面提示是检查 UART 接线，但它实际只表达一件事：

```text
ESP32 没拿到它期望的 STM32 协议响应。
```

因此这里必须先拆成两类问题：

```text
第一类：ESP32 RX 完全收不到任何 STM32 字节。
这类才优先查 TX/RX/GND/跳帽/波特率。

第二类：ESP32 RX 能收到 STM32 字节，但这些字节不是 OTA 协议期望的响应。
这类要继续查协议解析、版本格式、状态机和 Flash 写入。
```

本次现场属于第二类，所以这一节只是排查分叉点，不是最终根因。

### 现场现象

```text
STM32 上电快闪三下。
之后进入呼吸灯或常亮状态。
ESP32 日志一度提示 STM32 not responding。
```

### 现场验证

当时做了基础验证：

```text
ESP32 被动 RX probe。
ESP32 GPIO 电平 probe。
ESP32 GPIO22 -> GPIO21 回环测试。
抓 UART raw bytes。
确认 STM32 自发串口字节能进入 ESP32 RX。
```

关键 raw bytes：

```text
50 41 32 20 4F 4B 0D 0A
```

ASCII：

```text
PA2 OK..
```

### 判断结果

这说明：

```text
STM32 PA2 有输出。
ESP32 RX 能收到字节。
STM32 App 至少有代码在运行。
```

注意：这只能证明“单向字节能到达 ESP32”，不能证明 OTA 协议已经完整跑通。

后续又能收到 STM32 version response，因此排查方向从“完全无串口字节”转向“协议和 OTA 上层逻辑”：

```text
版本解析。
Keil 链接地址。
服务器地址。
OTA 状态机。
STM32 B 区 Flash 实际写入内容。
```

如果以后重新遇到 `STM32 not responding`，正确判断方式是：

```text
先看 raw bytes 和版本响应。
能收到 raw bytes：先不要反复改接线，继续看协议解析。
能收到 version response：硬件链路基本排除，继续看 OTA 上层逻辑。
完全没有 raw bytes：再回到 TX/RX/GND/电平/跳帽检查。
```

## 5. ISSUE-01：ESP32 已经收到 STM32 版本回复，但被新解析代码误判为无效

### 这个问题到底是什么

ESP32 向 STM32 发送“查询当前版本”的命令后，STM32 实际上已经回复了。

问题不在“STM32 没回”，而在“ESP32 收到了回复，却没有按旧格式解析出来”。

可以把它理解成：

```text
旧 STM32 App 回复：A1.0.2
新 ESP32 代码只认识：1.0.2
```

前面的 `A` 不是版本号本身，而是旧 A/B 分区逻辑留下来的分区标记。ESP32 新代码删除 A/B 逻辑后，只按纯版本号解析，于是把这条有效回复误判为无效。

### 用户现场看到什么

```text
ESP32 能收到 STM32 响应。
但版本解析失败。
OTA 检查流程会误报 STM32 not responding 或 Failed to query STM32 version。
```

典型日志：

```text
No valid protocol response (ret=17)
Raw response:
AA 02 00 11 41 31 2E 30 2E 32 ...
```

### 为什么要这样排查

这一步的核心判断是：

```text
ret=17 说明 ESP32 收到了 STM32 的 payload。
如果 STM32 完全没响应，通常不会拿到 17 字节 payload。
```

所以这时不能继续只查接线，而要看“收到的 payload 是什么格式”。

排查顺序是：

```text
1. 先看有没有 raw response。
2. 再看 raw response 的 CMD 是否是版本回复。
3. 再看 payload 长度是多少。
4. 最后把 payload 按 ASCII 翻译成人能读懂的内容。
```

### 现场日志

```text
No valid protocol response (ret=17)
Raw response:
AA 02 00 11 41 31 2E 30 2E 32 ...
```

### 证据拆解

```text
AA       SOF
02       CMD_REPORT_VERSION
00 11    payload 长度 = 17
41       ASCII 'A'
31       ASCII '1'
2E       ASCII '.'
30       ASCII '0'
2E       ASCII '.'
32       ASCII '2'
```

payload 实际含义不是乱码，而是：

```text
A + 1.0.2
```

ESP32 新解析器期望：

```text
1.0.2
```

### 根因

ESP32 侧做“单 App Target”改造时，删除了 A/B 分区字段；但现场 STM32 里还跑着旧 App，旧 App 的版本回复仍然带 A/B 分区前缀。

新旧格式如下：

```text
旧 App: A/B 前缀 + 版本号
新 App: 纯版本号
```

现场 STM32 还在运行旧 App，所以 ESP32 必须同时认识旧格式和新格式。否则会出现一个悖论：

```text
想通过 OTA 把旧 App 升级成新 App。
但 ESP32 不认识旧 App 的版本回复。
于是 OTA 还没开始就失败。
```

### 修复

ESP32 `version_from_payload()` 增加兼容逻辑：

```text
如果 payload 长度 >= 17，且第一个字节是 A 或 B，则跳过第一个字节再解析版本。
否则按 16 字节纯版本号解析。
```

修改位置：

```text
<OTA_ROOT>\esp32\src\stm32_bridge.cpp
```

### 验证

修复后：

```text
旧格式 A1.0.2 -> ESP32 解析为 1.0.2。
新格式 1.0.3  -> ESP32 解析为 1.0.3。
```

看到下面日志时，说明这个问题已解决：

```text
Valid STM32 version response: 1.0.2
```

如果仍然出现 `ret=17` 但版本无效，就继续检查 `version_from_payload()` 是否还兼容首字节 `A` 或 `B`。

## 6. ISSUE-02：App bin 看起来烧录成功，但它不是从 Bootloader 期望的地址运行

### 这个问题到底是什么

代码里定义 `APP_A_ADDR = 0x08004000` 只会影响运行时逻辑，不会强制 Keil 把 App 链接到 `0x08004000`。

当前架构要求 App 只能作为 A 区运行镜像。因此 App 的向量表必须在 `0x08004000`，Reset_Handler 必须是 `0x08004xxx`。如果 Keil Linker 或 scatter file 指到其他地址，Bootloader 跳转 A 区后就会进入错误入口。

换成更直白的话：

```text
Bootloader 会跳到 0x08004000 找 App 入口。
所以 App 必须在编译链接时就认为自己运行在 0x08004000。
```

如果 App 被链接到 `0x08000000` 或 `0x08040000`，即使 bin 文件存在、Keil 烧录也成功，Bootloader 仍然不能按当前架构正确启动它。

### 用户现场看到什么

```text
Bootloader 可以正常运行。
App 即使烧录成功，也不是有效 OTA 运行镜像。
但 Bootloader 跳转后 App 跑飞、无响应或异常复位。
OTA 生成的 bin 复制到 A 区后无法稳定启动。
```

常见误判是：

```text
代码里明明写了 APP_A_ADDR。
为什么 Keil 还要单独设置 IROM1 或 scatter file？
```

答案是：C 代码里的宏不会改变链接器放置向量表的位置。

### 为什么要这样排查

Bootloader 跳 App 的动作不是“按函数名调用 main”，而是读 A 区开头的向量表：

```text
0x08004000：初始栈顶地址
0x08004004：Reset_Handler 地址
```

所以排查跳转失败时，必须先确认 App bin 的向量表是不是真的从 `0x08004000` 开始。

正确排查顺序：

```text
1. 打开 App 工程，而不是旧的无关工程。
2. 检查 Keil Target 或 scatter file。
3. 编译后看 map 文件。
4. 确认 __Vectors = 0x08004000。
5. 确认 Reset_Handler = 0x08004xxx。
```

### 容易误解的点

```c
#define APP_A_ADDR 0x08004000
```

这只是运行时代码使用的地址，不决定链接地址。

真正决定 App 向量表和 Reset_Handler 在哪里的是：

```text
Keil Linker 设置。
Scatter File。
```

### 错误示例

如果 App 错误链接到 `0x08000000`：

```text
App 以为自己从 0x08000000 运行。
Reset_Handler 落在 0x08000xxx。
Bootloader 跳到 0x08004000 后找不到正确入口。
```

如果 App 错误链接到 B 区：

```text
Reset_Handler 落在 B 区地址。
Bootloader 把 B 复制到 A 后，镜像内部地址仍指向 B。
```

### 正确模型

```text
App 最终运行地址：
0x08004000
┌──────────────────────────────┐
│ vector table                  │
│ Reset_Handler = 0x08004xxx    │
│ RO code                       │
└──────────────────────────────┘

OTA 下载缓存地址：
0x0802C000
┌──────────────────────────────┐
│ 暂存同一个 App 镜像            │
│ vector table 内容仍指向 A 区   │
│ Reset_Handler 仍是 0x08004xxx │
└──────────────────────────────┘
```

### 修复

App 工程固定只链接到：

```text
0x08004000
```

并使用：

```text
STM32F103ZETX_APP.sct
```

Keil 工程入口必须使用：

```text
<OTA_ROOT>\stm32\app\app.uvprojx
```

不要再打开旧的或无关工程：

```text
<OTA_ROOT>\stm32\app\Projects\MDK-ARM\atk_f103.uvprojx
```

判断标准不是“Keil 能不能烧录成功”，而是编译结果是否满足当前 Bootloader 跳转模型。

### 验证

编译后检查 map：

```text
__Vectors      = 0x08004000
Reset_Handler  = 0x08004xxx
ER_RAMFUNC Exec base = 0x20000000
```

如果 `__Vectors` 不是 `0x08004000`，该 App bin 不能用于当前 OTA 架构。

验收时还要确认 Keil Target 配置：

```text
IROM1 Start = 0x08004000
IROM1 Size  = 0x28000
IRAM1 Start = 0x20000000
IRAM1 Size  = 0x10000
```

如果勾选了 Linker scatter file，则最终以 scatter file 为准；Target 页里的 IROM1 主要用于生成/展示默认内存布局，不能覆盖 scatter file。

## 7. ISSUE-03：把 B 区误认为“第二个运行区”，导致误以为需要两个 App Target

### 这个问题到底是什么

项目早期讨论中混用了两种架构：一种是“双运行区 A/B 直接跳转”，另一种是“单运行区 A + B 下载缓存 + C 备份”。两种架构对 Keil Target、bin 生成和 Bootloader 状态机要求完全不同。

本项目最后选择的是：

```text
A 区：唯一运行区。
B 区：下载缓存区。
C 区：旧 App 备份区。
```

所以 B 区不是“另一个可以直接运行的 App 区”。B 区只是先临时存放新固件，等 Bootloader 上电后再复制到 A 区运行。

### 用户现场看到什么

```text
会误以为需要 App A Target 和 App B Target。
会误以为要生成两个 bin。
会误以为 B 区镜像要链接到 B 区地址。
会导致 Bootloader 跳转逻辑和 App 链接地址互相矛盾。
```

### 为什么要这样排查

这个问题不一定会表现成一条明确报错，它会让工程设置越来越乱：

```text
一会儿想让 App 链接到 A。
一会儿又想让 App 链接到 B。
一会儿觉得要两个 bin。
一会儿又觉得 OTA 只发一个 bin。
```

所以排查时不能先改 Keil Target，而要先确认 Bootloader 的设计模型：

```text
Bootloader 最终跳哪里？
如果最终只跳 0x08004000，那么 App 就只能链接到 0x08004000。
```

这一步是为了避免把“下载缓存地址”和“最终运行地址”混在一起。

### 两种架构对比

双运行区 A/B：

```text
App A 链接到 A 区。
App B 链接到 B 区。
Bootloader 可以直接跳 A 或跳 B。
需要两个链接地址，通常也需要两个 Target 或两套输出。
```

当前缓存安装架构：

```text
A = 唯一运行区。
B = OTA 下载缓存。
C = 旧 A 备份区。
Bootloader 永远最终跳 A。
```

### 当前项目选择

当前采用：

```text
单 App Target。
单 stm32_app.bin。
App 永远链接到 0x08004000。
B 区只存放待安装镜像。
```

### 升级过程示意

```text
升级前：
A = 旧 App，正在运行
B = 空或旧缓存
C = 空或旧备份

下载完成：
A = 旧 App，继续运行
B = 新 App，等待安装
C = 空或旧备份

Bootloader 安装：
A -> C，备份旧 App
B -> A，安装新 App

安装后：
A = 新 App
B = 新 App 缓存
C = 旧 App 备份
```

### 结果

```text
不需要 App B Target。
不需要 App B 专用链接地址。
不需要服务器区分 A 包和 B 包。
只需要维护一个 App 工程和一个 stm32_app.bin。
```

接手人判断标准：

```text
如果 Bootloader 最终只跳 0x08004000，就只能有一个 App 链接地址。
如果服务器只发布一个 stm32_app.bin，就不要再为 B 区建立第二个 App Target。
如果 B 区只是 cache，它保存的是“将来复制到 A 区运行”的镜像，不是“直接从 B 区运行”的镜像。
```

## 8. ISSUE-04：MQTT 升级命令到了 ESP32，但 ESP32 去旧电脑 IP 下载固件清单

### 这个问题到底是什么

ESP32 已经收到 MQTT 升级命令，但它检查更新时仍访问旧电脑 IP 上的 HTTP 固件服务器，导致 `version.json` 获取失败，OTA 停在检查更新阶段。

简化成一句话：

```text
MQTT 命令送到了 ESP32，但 ESP32 去错误 IP 下载 version.json。
```

这个问题容易误判成“服务器触发失败”。实际情况是：

```text
MQTT 服务器工作正常。
ESP32 也收到了触发命令。
失败发生在下一步 HTTP 下载 version.json。
```

### 用户现场看到什么

```text
不会进入真正的固件下载。
不会向 STM32 发送 OTA_START。
STM32 端不会收到任何升级数据。
```

### 为什么要这样排查

本项目里电脑同时扮演两个服务器角色：

```text
MQTT 服务器：只负责告诉 ESP32“现在检查升级”。
HTTP 服务器：负责提供 version.json 和 stm32_app.bin。
```

所以看到 `MQTT connected` 只能证明 MQTT 通了，不能证明 HTTP 固件服务器地址正确。

排查顺序必须是：

```text
1. 先确认 ESP32 是否收到 MQTT check。
2. 再看 ESP32 打印的 version.json URL。
3. 对比这个 URL 里的 IP 是否等于电脑当前 IP。
4. 在电脑本机访问同一个 URL，确认是否 HTTP 200。
```

### 背景：本项目有两个电脑服务

| 服务 | 默认端口 | ESP32 用它做什么 | 如果错了会怎样 |
|---|---:|---|---|
| MQTT broker / Mosquitto | 1883 | 接收 `stm32/ota/check` 触发命令 | ESP32 收不到升级触发 |
| HTTP firmware server | 8080 | 下载 `version.json` 和 `stm32_app.bin` | ESP32 收到触发后仍无法升级 |

本次问题发生在第二个：

```text
HTTP firmware server 地址错了。
```

不是 STM32 问题，也不是 UART 问题。

### 触发条件

```text
换电脑。
换 WiFi。
路由器重新分配 IP。
电脑从 192.168.0.104 变成 192.168.0.108。
只改了 MQTT 地址，没有同步改 HTTP URL。
version.json 里的固件 URL 仍然是旧 IP。
```

本次现场：

```text
旧地址：192.168.0.104
当前电脑地址：192.168.0.108
```

### 现场日志

```text
[OTA] MQTT connected, subscribed: stm32/ota/check
[OTA] MQTT OTA check requested
[OTA] State: IDLE -> CHECKING
[OTA] Checking for updates: http://192.168.0.104:8080/firmware/version.json
[OTA] ERROR: HTTP GET failed: code=-1, reason=connection refused
```

正确读法：

```text
MQTT connected
说明 ESP32 已经连上 MQTT。

MQTT OTA check requested
说明升级命令已经到达 ESP32。

Checking for updates: http://192.168.0.104:8080/...
说明 ESP32 正在访问 HTTP 固件服务器。

connection refused
说明这个 HTTP 地址没有服务在响应。
```

最关键的一行：

```text
Checking for updates: http://192.168.0.104:8080/firmware/version.json
```

如果这里的 IP 不是当前电脑 IP，就会失败或访问到错误服务器。

### 证据链

证据 1：电脑当前 IPv4 是：

```text
192.168.0.108
```

证据 2：ESP32 日志里访问的是旧地址：

```text
http://192.168.0.104:8080/firmware/version.json
```

证据 3：访问当前电脑地址可以成功：

```powershell
Invoke-WebRequest http://192.168.0.108:8080/firmware/version.json -UseBasicParsing
```

返回：

```text
HTTP 200
```

证据 4：MQTT 监听也在当前电脑地址：

```text
192.168.0.108:1883 Listen
```

故障链路：

```text
MQTT trigger -> ESP32       成功
ESP32 -> HTTP old IP        失败
ESP32 -> HTTP current IP    成功
```

### 根因

电脑 IP 变化后，工程中和“电脑服务器地址”相关的配置没有统一更新。

这些地址没有保持一致：

```text
ESP32 访问 version.json 的 HTTP base URL。
version.json 里 stm32_app.bin 的下载 URL。
ESP32 连接 MQTT 的 broker host。
Mosquitto 实际监听的 IP。
server.py 默认发布/触发使用的 IP。
```

结果：

```text
ESP32 可以收到升级命令。
但下载固件清单时访问了旧 IP。
因此升级流程在 CHECKING 阶段失败。
```

### 修复内容

统一为当前电脑 IP：

```text
当前电脑 IP：192.168.0.108
HTTP server：http://192.168.0.108:8080
MQTT broker：192.168.0.108:1883
固件 URL：http://192.168.0.108:8080/firmware/stm32_app.bin
```

检查和修改：

| 文件 | 要确认的内容 |
|---|---|
| `esp32/src/ota_client.cpp` | `OTA_SERVER_BASE_URL` 是否是当前电脑 IP |
| `esp32/src/config.h` | 默认 OTA/日志 IP 是否仍是旧 IP |
| `esp32/src/mqtt_ota_trigger.cpp` | `MQTT_BROKER_HOST` 是否是当前电脑 IP |
| `esp32/mosquitto_ota.conf` | `listener 1883 <当前电脑IP>` |
| `server/serve.py` | MQTT broker 默认 host 是否是当前电脑 IP |
| `server/firmware/version.json` | `latest.url` 是否指向当前电脑 IP |

### 验证标准

1. 电脑能访问自己的 HTTP 固件清单：

```powershell
Invoke-WebRequest http://192.168.0.108:8080/firmware/version.json -UseBasicParsing
```

预期：

```text
HTTP 200
```

2. 电脑 MQTT broker 正在监听：

```powershell
Get-NetTCPConnection -LocalPort 1883
```

预期：

```text
192.168.0.108:1883 Listen
```

3. ESP32 收到 MQTT 触发：

```text
MQTT OTA check requested
```

4. ESP32 检查更新时访问的是新地址：

```text
Checking for updates: http://192.168.0.108:8080/firmware/version.json
```

5. 不再出现：

```text
HTTP GET failed: connection refused
```

6. 能继续进入：

```text
update_available
```

### 防复发

以后只要换电脑、换 WiFi、换路由器、电脑 IP 变化，都先执行：

```text
电脑当前 IP 是多少？
HTTP server 是否在这个 IP 的 8080 端口可访问？
MQTT broker 是否在这个 IP 的 1883 端口监听？
ESP32 OTA_SERVER_BASE_URL 是否是这个 IP？
ESP32 MQTT_BROKER_HOST 是否是这个 IP？
version.json 里的固件 url 是否是这个 IP？
```

不要把 ESP32 自己的 IP 当成服务器 IP。

```text
ESP32 IP：只是设备自己的地址。
电脑 IP：才是 HTTP/MQTT 服务器地址。
```

## 9. ISSUE-05：失败日志只说 `finish failed`，无法判断 STM32 是没回应还是主动报错

### 这个问题到底是什么

ESP32 原始日志只打印 `OTA finish verification failed`，没有显示 STM32 是否回应、回应长度是多少、返回码是多少。导致排查无法判断是 UART 超时、STM32 拒绝，还是 STM32 校验失败。

这不是 OTA 协议本身的根因，而是“排查信息不足”的问题。日志太粗，导致每一层都像嫌疑人。

### 用户现场看到什么

```text
故障范围过大。
服务器、ESP32、UART、STM32 App、Flash 校验都会被纳入排查范围。
排查效率很低。
```

### 为什么要这样排查

`CMD_OTA_FINISH` 之后有两种完全不同的失败：

```text
情况 1：ESP32 等不到 STM32 回复。
这说明要查 UART 最后一帧、超时、STM32 卡死。

情况 2：ESP32 收到了 STM32 回复，且返回失败码。
这说明 STM32 活着，而且主动判定 OTA 校验失败。
```

这两种情况的排查方向完全不同，所以日志必须打印：

```text
wait_for_response 返回值 ret。
STM32 返回的 result code。
```

### 原始表现

```text
OTA finish verification failed
```

这个日志没有回答：

```text
STM32 是否回了 CMD_OTA_RESULT？
wait_for_response 返回值是多少？
STM32 result code 是多少？
```

### 修复

在 ESP32 `stm32_bridge.cpp` 增加最近一次 STM32 通信错误缓存：

```text
g_stm32_last_error
stm32_last_error()
```

在失败点记录：

```text
OTA start failed: ret, ack
packet seq failed
finish ret, result
```

在 `ota_client.cpp` 中把这些细节拼进 `ota_last_error()`，再通过 MQTT status 发出。

修改位置：

```text
<OTA_ROOT>\esp32\src\stm32_bridge.cpp
<OTA_ROOT>\esp32\src\stm32_bridge.h
<OTA_ROOT>\esp32\src\ota_client.cpp
<OTA_ROOT>\esp32\src\mqtt_ota_trigger.cpp
```

### 验证

增强后实际看到：

```text
failed:OTA finish verification failed: finish ret=1, result=0x01
```

这条日志带来的判断：

```text
ret=1
ESP32 收到了 1 字节 STM32 payload。

result=0x01
STM32 主动返回 finish 失败。

结论
不是最后一帧超时，也不是 STM32 完全不响应。
```

## 10. ISSUE-06：固件已经发完，但 STM32 在最终校验时返回失败

### 这个问题到底是什么

ESP32 已经下载完整 bin，并且 STM32 ACK 了所有 UART 数据包，但 STM32 在 `CMD_OTA_FINISH` 后返回 `0x01`。这说明失败发生在 STM32 最终校验阶段，而不是 MQTT、HTTP 或普通 UART 传输阶段。

更直白地说：

```text
服务器给了固件。
ESP32 下载了固件。
ESP32 把固件分包发给了 STM32。
STM32 每包都回了 ACK。
但 STM32 最后算总账时说：这份固件不对。
```

### 用户现场看到什么

```text
进度已经到 100%。
数据包已经全部发送。
最后一步 finish 失败。
```

### 为什么要这样排查

OTA finish 是一个分界点。

在 finish 之前，排查重点是：

```text
MQTT 有没有触发。
HTTP 有没有下载。
UART 包有没有发完。
STM32 有没有 ACK。
```

到了 finish 之后，STM32 会检查整份 B 区固件：

```text
写入大小是否正确。
写入字节数是否正确。
B 区 CRC 是否等于服务器 manifest 里的 CRC。
OTA_INFO 是否能写成 VERIFIED。
```

所以看到 `ret=1, code=0x01` 后，下一步不是继续查 MQTT，也不是继续查 HTTP，而是查 STM32 B 区里实际写进去的内容。

### 现场日志

```text
[OTA] STM32 version: 1.0.2
[OTA] Current: 1.0.2, Latest: 1.0.3
[OTA] Update available
[OTA] OTA start ACK received
[OTA] Progress: 100% (8948/8948 bytes)
[OTA] Download complete: 8948 bytes, 18 packets
[OTA] Finishing OTA, total size=8948
[OTA] ERROR: OTA verify FAILED: ret=1, code=0x01
```

### 日志解读

```text
STM32 version: 1.0.2
UART 查询版本成功。

Current 1.0.2, Latest 1.0.3
HTTP version.json 成功，版本比较成功。

OTA start ACK received
STM32 接受 OTA_START，并准备写 B 区。

Progress 100%
ESP32 下载并发送了完整固件。

18 packets
每个 UART 数据包都收到了 STM32 ACK。

ret=1, code=0x01
STM32 finish 阶段主动失败。
```

### 可排除

```text
MQTT 没触发。
HTTP version.json 访问失败。
HTTP bin 下载不完整。
ESP32 没发送完。
UART 完全不通。
STM32 最后一帧没响应。
```

### 剩余排查范围

```text
STM32 B 区实际写入的数据错了。
STM32 bytes_written 不等于 expected_size。
STM32 B 区 CRC 不等于 expected_crc32。
OTA_INFO 写 VERIFIED 失败。
```

当时 STM32 只返回统一 `0x01`，所以不能只靠 ESP32 日志继续猜。下一步必须读回 B 区，把“实际写进 Flash 的内容”与服务器 bin 对比。

## 11. ISSUE-07：读回 STM32 B 区后发现内容和服务器 bin 不一致

### 这个问题到底是什么

使用 ST-LINK 读回 STM32 B/cache 区后，发现读回内容 CRC 与服务器 `stm32_app.bin` CRC 不一致，证明 STM32 实际写入 Flash 的内容已经不是原始固件。

这一步要回答的问题是：

```text
ESP32 发出去的 bin，最后有没有原样落到 STM32 的 B 区？
```

结果是没有。

### 用户现场看到什么

```text
ESP32 下载 CRC 是对的。
UART 每包 ACK 是有的。
STM32 finish 仍然失败。
```

这时最容易误解为“STM32 finish 逻辑有 bug”。所以必须把 B 区读出来，用事实确认 Flash 里到底是什么。

### 为什么要这样排查

前面只能证明“传输流程看起来完成了”，不能证明 Flash 内容正确。

必须做一次端到端对比：

```text
服务器 stm32_app.bin
        |
        | ESP32 下载
        |
        | UART 分包
        |
        v
STM32 B 区实际 Flash 内容
```

如果服务器 bin 和 B 区读回内容 CRC 一致，就继续查 OTA_INFO 或 Bootloader。

如果 CRC 不一致，就说明失败点在“写入 B 区之前或写入 B 区过程中”，不用再猜 Bootloader。

### 读回目标

```text
APP_DOWNLOAD_ADDR = 0x0802C000
bin size          = 8948 bytes = 0x22F4
```

读回范围：

```text
地址：0x0802C000
长度：0x22F4
```

操作意图：

```text
不是读整个 Flash。
只读 OTA 下载缓存 B 区里本次固件占用的 size 字节。
读回后和服务器 stm32_app.bin 做 CRC 和逐字节 diff。
```

### CRC 对比

服务器 bin：

```text
size = 8948
crc  = 77BAF9AE
```

STM32 B 区读回：

```text
size = 8948
crc  = 891AB310
```

### 结论

```text
STM32 B 区内容和服务器 bin 不一致。
STM32 finish 阶段 CRC 失败是合理结果。
问题进入“写入内容为何被污染”的阶段。
```

### 逐字节差异

典型差异：

```text
offset 0x1F6: server=FF flash=F4
offset 0x1F7: server=FF flash=01
offset 0x1F8: server=FF flash=00
offset 0x1F9: server=FF flash=00

offset 0x3F0: server=FF flash=F4
offset 0x3F1: server=FF flash=01
offset 0x3F2: server=FF flash=00
offset 0x3F3: server=FF flash=00
```

直接读 Flash：

```text
0x0802C1F0 : FF FF FF FF FF FF F4 01 00 00 FF FF FF FF FF FF
0x0802C3EA : FF FF FF FF FF FF F4 01 00 00 FF FF FF FF FF FF
```

### 关键判断

这不是随机噪声。原因：

```text
错误位置固定。
每次污染 4 字节。
污染值固定为 F4 01 00 00。
```

`F4 01 00 00` 小端解释：

```text
0x000001F4 = 500
```

这指向：

```text
长度变量。
缓冲区边界。
栈覆盖。
满 payload 边界附近问题。
```

## 12. ISSUE-08：B 区不是随机损坏，而是每个数据包末尾 4 字节固定被改写

### 这个问题到底是什么

旧分包方式让每个 `CMD_DATA_PACKET` 的 payload 刚好达到 512 字节上限。读回 B 区后发现，每个 506 字节数据包的最后 4 字节被固定污染；污染位置和分包边界严格对应，因此排查范围锁定到“满 payload 边界触发的接收/写入侧边界问题”。

这一步不是在看“CRC 错了”这么粗的结果，而是在看“到底错在哪里”。

如果 Flash 内容是随机错误，可能是供电、串口干扰、擦写异常。

但实际错误有明显规律：

```text
每包最后 4 字节错。
错的位置跟 506 字节分包边界对齐。
错的内容还固定。
```

这说明问题不是随机噪声，而是代码边界问题。

### 用户现场看到什么

```text
STM32 B 区 CRC 和服务器 bin 不一致。
逐字节 diff 后发现错误位置有周期。
周期刚好等于旧分包大小 506 字节。
```

### 为什么要这样排查

CRC 只能告诉我们“整体不一样”，不能告诉我们“为什么不一样”。

所以 CRC 不一致后，下一步必须做逐字节 diff：

```text
如果错误随机分布：
优先查供电、串口干扰、Flash 擦写稳定性。

如果错误集中在固定偏移：
优先查协议字段、缓冲区长度、结构体覆盖、边界条件。

如果错误刚好落在每包末尾：
优先查分包长度和接收缓冲区边界。
```

本次错误属于第三种。

### 旧分包方式

ESP32 原代码：

```cpp
uint8_t buf[PROTOCOL_MAX_PAYLOAD - 6];
```

当时：

```text
PROTOCOL_MAX_PAYLOAD = 512
```

所以：

```text
固件数据 data = 506 字节
协议头 seq + offset = 6 字节
payload 总长度 = 512 字节
```

示意：

```text
CMD_DATA_PACKET payload

┌──────────┬────────────┬──────────────────────────────┐
│ seq 2B   │ offset 4B  │ data 506B                    │
└──────────┴────────────┴──────────────────────────────┘
总长度 = 512B，刚好顶满 PROTOCOL_MAX_PAYLOAD
```

### 与污染位置的关系

8948 字节分包：

```text
8948 / 506 = 17 包余 346
```

前几包范围：

```text
第 0 包：offset 0，    len 506，范围 0x0000 - 0x01F9
第 1 包：offset 506，  len 506，范围 0x01FA - 0x03F3
第 2 包：offset 1012， len 506，范围 0x03F4 - 0x05ED
```

污染位置：

```text
第 0 包最后 4 字节：0x01F6 - 0x01F9
第 1 包最后 4 字节：0x03F0 - 0x03F3
第 2 包最后 4 字节：0x05EA - 0x05ED
```

这刚好匹配：

```text
每个 506 字节数据包的最后 4 字节。
```

### ACK 的含义和限制

STM32 每包 ACK 只能证明：

```text
协议帧 CRC16 通过。
seq 和 offset 符合预期。
flash_write_data 返回成功。
```

它不能证明：

```text
Flash 中每个字节都等于服务器 bin。
```

所以出现了：

```text
每包 ACK。
进度 100%。
最终全量 CRC 失败。
```

处理结论：

```text
继续追查 STM32 接收缓冲区或 Flash 写函数可以定位更底层的覆盖点。
但从工程交付角度，先让 ESP32 避开 512 字节满 payload，是最快闭环方案。
```

## 13. ISSUE-09：把单包数据从 506 降到 500 后，验证并修复 512 字节边界问题

### 这个问题到底是什么

为验证 512 payload 边界假设，ESP32 侧将每包固件数据从 506 字节降到 500 字节。修改后 payload 从 512 降到 506，OTA 完成并返回 success。

这不是随便改小分包，也不是为了降低速度。

它是在验证一个明确假设：

```text
旧方案把协议 payload 刚好顶满 512 字节。
B 区污染又刚好发生在每包末尾。
所以先让 payload 不要顶满，看看污染是否消失。
```

### 用户现场看到什么

```text
修改前：
OTA 能发完，但 finish 失败。
B 区 CRC 错。
每个 506 字节包末尾 4 字节被污染。

修改后：
OTA 完成。
MQTT status 返回 success。
```

### 为什么要这样修

有两种修法思路：

```text
彻底修底层：
继续深挖 STM32 接收 buffer、结构体、Flash 写函数，找到具体覆盖点。

工程闭环：
先避开已经被证据锁定的 512 payload 满载边界，让 OTA 流程稳定跑通。
```

当前阶段选择第二种，是因为它满足三个条件：

```text
改动最小。
不改变协议命令格式。
能直接验证“512 满载边界”是不是关键触发条件。
```

### 可选方案

```text
方案 A：继续用 512 payload，深入查 STM32 栈和缓冲区覆盖。
方案 B：修改 STM32 协议结构，让 buffer 更大。
方案 C：改小 ESP32 分包，避开 512 payload 满载边界。
方案 D：重写 STM32 UART 接收为 DMA 或环形缓冲。
```

### 选择方案 C 的原因

```text
改动最小。
不改变协议命令格式。
不需要马上大改 STM32 接收状态机。
500 字节仍接近 512，效率损失很小。
可以最快验证边界问题假设。
```

### 修复前后对比

修复前：

```text
data    = 506
payload = 2 + 4 + 506 = 512
状态    = 顶满协议最大值
```

修复后：

```text
data    = 500
payload = 2 + 4 + 500 = 506
状态    = 保留 6 字节余量
```

示意：

```text
修复前：

┌─────┬────────┬──────────────────────────────┐
│ seq │ offset │ data 506B                    │
└─────┴────────┴──────────────────────────────┘
payload = 512B

修复后：

┌─────┬────────┬──────────────────────────┐
│ seq │ offset │ data 500B                │
└─────┴────────┴──────────────────────────┘
payload = 506B
```

### 验证结果

重新下载 ESP32 后触发 MQTT：

```text
online
check_requested
checking
update_available
success
```

判断闭环：

```text
读回 B 区发现每包末尾固定污染。
污染位置和 506 字节分包强相关。
旧 payload 正好顶到 512 字节。
把单包数据降到 500 后 OTA 成功。
```

因此最终判断：

```text
512 payload 满载边界是导致 STM32 B 区写入污染的关键触发条件。
```

修改位置：

```text
<OTA_ROOT>\esp32\src\ota_client.cpp
```

关键修改：

```cpp
#define STM32_OTA_DATA_CHUNK_SIZE  500U

#undef PROTOCOL_MAX_PAYLOAD
#define PROTOCOL_MAX_PAYLOAD       (STM32_OTA_DATA_CHUNK_SIZE + 6U)
```

这不是随意降低速度，而是为了让 `CMD_DATA_PACKET` payload 留出边界余量，避免再次顶满 512 字节。

## 14. OTA 安装和回滚状态机复盘

### OTA_INFO 是什么

OTA_INFO 是 Flash 里的状态记录区，用来告诉 Bootloader：

```text
当前有没有新固件。
新固件多大。
新固件 CRC 是多少。
当前处于下载、校验完成、安装、待确认还是失败。
```

它不保存完整固件，只保存状态和元数据。

### 关键状态

```text
IDLE
没有待安装固件，正常启动 A 区。

DOWNLOADING
App 正在接收 OTA 数据到 B 区。

VERIFIED
B 区新固件已完整写入并通过 CRC，等待 Bootloader 安装。

INSTALLING
Bootloader 正在安装。安装过程分两步：先 A->C 备份旧 App，再 B->A 安装新 App。

PENDING_CONFIRM
新 App 已经安装到 A 区，但还没有确认自己运行正常。

FAILED
升级失败，Bootloader 应避免盲目安装。
```

### 掉电场景

下载中掉电：

```text
状态停在 DOWNLOADING。
Bootloader 不安装 B 区。
继续启动旧 A 区。
```

B 区校验后掉电：

```text
状态是 VERIFIED。
Bootloader 下次上电继续安装。
```

B -> A 复制中掉电：

```text
状态是 INSTALLING。
Bootloader 需要判断 A 是否安全。
必要时用 C 区恢复。
```

新 App 启动失败：

```text
状态停在 PENDING_CONFIRM。
Bootloader 增加 boot_attempts。
超过阈值后 C -> A 回滚。
```

新 App 死循环但不复位：

```text
Bootloader 没机会重新接管。
需要 IWDG 看门狗配合。
```

## 15. DS1 常亮升级验证复盘

本次用于验证 OTA 的业务改动：

```text
STM32 App 从 1.0.2 升到 1.0.3。
DS1 改为常亮。
服务器发布 stm32_app.bin。
ESP32 通过 MQTT 触发 OTA。
```

验证链：

```text
ESP32 查询 STM32 version = 1.0.2。
ESP32 查询服务器 latest = 1.0.3。
版本比较判断 update_available。
ESP32 下载 size=8948, crc32=77BAF9AE。
ESP32 UART 分包发送。
STM32 finish 校验通过。
ESP32 发布 success。
STM32 重启后运行新 App。
```

验收 OTA 不能只看 `success`，还要继续看：

```text
STM32 版本是否变成 1.0.3。
DS1 是否符合新代码行为。
再次触发 check 时是否显示 already up to date。
```

## 16. 关键转折点

```text
1. 收到有效版本响应
   说明基础协议查询链路已通，继续查版本格式和 OTA 逻辑。

2. HTTP connection refused
   说明 ESP32 进入检查流程，但 HTTP 固件服务器地址或服务状态有问题。

3. MQTT check_requested -> update_available
   说明 MQTT 和 HTTP 清单读取通了，继续查传输和 STM32 finish。

4. finish ret=1 result=0x01
   说明 STM32 主动判定最终校验失败。

5. ST-LINK 读回 B 区 CRC 不一致
   说明 B 区实际数据已经错了。

6. 每包最后 4 字节固定污染
   说明不是随机噪声，而是分包边界相关问题。

7. 分包降到 500 后 success
   说明 512 payload 满载边界判断闭环。
```

## 17. 后续改进建议

### 17.1 STM32 OTA_RESULT 细分错误码

当前 finish 失败统一返回：

```text
0x01
```

建议改成：

```text
0x00 = 成功
0x01 = reported_size 不一致
0x02 = bytes_written 不一致
0x03 = B 区 CRC 不一致
0x04 = OTA_INFO 写 VERIFIED 失败
0x05 = 当前不在 OTA active 状态
```

### 17.2 finish 返回实际 CRC

建议 STM32 finish 失败时返回：

```text
expected_size
bytes_written
expected_crc32
actual_crc32
```

这样日志可以直接显示：

```text
expected_crc32=77BAF9AE
actual_crc32=891AB310
```

### 17.3 ESP32 分包变量命名优化

当前修复有效，但更清晰的写法是：

```cpp
#define PROTOCOL_MAX_PAYLOAD 512
#define STM32_OTA_DATA_CHUNK_SIZE 500
uint8_t buf[STM32_OTA_DATA_CHUNK_SIZE];
```

语义更明确：

```text
协议允许最大 512。
当前 OTA 主动选择 500 数据字节。
```

### 17.4 保留 ST-LINK 读回检查流程

以后只要出现：

```text
OTA finish verification failed
```

最快定位方式：

```text
1. 读回 APP_DOWNLOAD_ADDR 开始的 size 字节。
2. 和 server/firmware/stm32_app.bin 比 CRC。
3. 如果 CRC 不同，做逐字节 diff。
4. 看差异是随机还是周期性。
```

### 17.5 每次发布记录四个值

发布新版本时记录：

```text
version
size
crc32
Reset_Handler
```

示例：

```text
version       = 1.0.3
size          = 8948
crc32         = 77BAF9AE
Reset_Handler = 0x08004C29
```

## 18. 最终排查清单

以后遇到 OTA 失败，按这个顺序查：

```text
1. MQTT 是否收到 check_requested。
2. HTTP version.json 是否返回 200。
3. STM32 当前版本是否能查询。
4. 是否进入 update_available。
5. HTTP bin size 是否和 manifest 一致。
6. ESP32 本地下载 CRC 是否通过。
7. UART 每包是否 ACK。
8. STM32 finish ret/result 是什么。
9. 必要时用 ST-LINK 读回 B 区计算 CRC。
10. 最后再看 Bootloader 安装、跳转和回滚。
```

一句话总结：

```text
这次最终不是服务器问题，不是 MQTT 问题，也不是基础硬件链路问题。
真正导致 OTA finish 失败的核心原因，是 512 字节满 payload 触发的 UART/Flash 写入边界污染。
把单包数据从 506 降到 500 后，问题闭环解决。
```
