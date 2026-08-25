# 通信协议文档

## 物理层

- **接口**: UART (STM32 USART2 ↔ ESP32 Serial2)
- **引脚**: STM32 PA2(TX)→ESP32 GPIO16(RX), STM32 PA3(RX)←ESP32 GPIO17(TX)
- **注**: USART1 (PA9/PA10) 留给板载 CH340 做 printf 调试输出
- **波特率**: 115200, 8N1, 无流控

## 帧格式

```
┌──────┬──────┬───────┬──────┬──────────┬───────┬───────┐
│ SOF  │ CMD  │ LenH  │ LenL │ Payload  │ CRCH  │ CRCL  │
│ 1B   │ 1B   │   2B (大端)   │ 0~N 字节 │  CRC16-CCITT    │
│ 0xAA │      │       │      │          │   2B (大端)      │
└──────┴──────┴───────┴──────┴──────────┴───────┴───────┘
```

- **SOF**: Start Of Frame, 固定 `0xAA`
- **CMD**: 命令字
- **Len**: Payload 长度 (0~512), 大端序
- **Payload**: 命令数据
- **CRC**: CRC16-CCITT 校验值, 对 Payload 部分计算, 大端序

### CRC16-CCITT 参数

- 多项式: `0x1021`
- 初始值: `0xFFFF`
- 输入不反转, 输出不反转

## 命令列表

### 0x01 - 查询版本 (ESP→STM)

```
Payload: 无
```

查询 STM32 当前固件版本和运行分区。

---

### 0x02 - 回复版本 (STM→ESP)

```
Offset  Size  Description
0       1     活动分区: 'A'(0x41) 或 'B'(0x42)
1       16    固件版本字符串 (例: "1.0.0\0")
Total   17
```

---

### 0x10 - 开始 OTA (ESP→STM)

```
Offset  Size  Description
0       4     固件文件大小 (大端 uint32)
4       4     固件 CRC32 校验值 (大端 uint32)
8       16    新固件版本号字符串 (含 '\0')
Total   24
```

---

### 0x11 - ACK OTA 开始 (STM→ESP)

```
Offset  Size  Description
0       1     ACK 状态码
              0x00 = ACK_OK (准备就绪)
              0x01 = CRC_ERROR
              0x02 = FLASH_ERROR
              0x03 = BUSY
Total   1
```

---

### 0x12 - 数据包 (ESP→STM)

```
Offset  Size  Description
0       2     包序号 (seq, 大端 uint16, 从 0 递增)
2       4     Flash 写入偏移地址 (大端 uint32)
6       N     固件数据 (1~506 字节)
Total   6+N
```

建议数据段大小: 256 ~ 506 字节。

---

### 0x13 - ACK 数据包 (STM→ESP)

```
Offset  Size  Description
0       2     确认的包序号 (大端 uint16)
2       1     ACK 状态码
              0x00 = ACK_OK (写入成功)
              0x01 = CRC_ERROR
              0x02 = FLASH_ERROR
              0x03 = SEQ_ERROR (序号错乱)
Total   3
```

收到 NAK 后, ESP32 应重传该序号的数据包。最多重传 3 次。

---

### 0x20 - OTA 结束 (ESP→STM)

```
Offset  Size  Description
0       4     累计发送的总字节数 (大端 uint32)
Total   4
```

通知 STM32 所有数据已发送完毕, 请求进行全量 CRC32 校验。

---

### 0x21 - 校验结果 (STM→ESP)

```
Offset  Size  Description
0       1     校验结果
              0x00 = OTA_RESULT_PASS (校验通过)
              0x01 = CRC32_FAIL
              0x02 = SIZE_MISMATCH
Total   1
```

---

### 0x30 - 重启 (ESP→STM)

```
Payload: 无
```

STM32 收到后执行 NVIC_SystemReset(), 进入 Bootloader。
Bootloader 检测到 OTA_STATE_VERIFIED 后切换启动分区。

---

### 0xFF - 错误响应

```
Offset  Size  Description
0       1     错误码
              0x01 = UNKNOWN_CMD
              0x02 = INVALID_STATE
              0x03 = FLASH_OP_FAILED
Total   1
```

## OTA 时序图

```
ESP32                              STM32
  │                                   │
  ├─ 0x01 Query Version ────────────►│
  │◄────────────── 0x02 Version ─────┤ {"A", "1.0.0"}
  │                                   │
  ├─ 0x10 OTA Start ────────────────►│ → 擦除目标分区
  │◄────────────── 0x11 ACK ─────────┤
  │                                   │
  ├─ 0x12 Data Pkt(seq=0) ──────────►│ → 写 Flash
  │◄────────── 0x13 ACK(seq=0,OK) ───┤
  ├─ 0x12 Data Pkt(seq=1) ──────────►│
  │◄────────── 0x13 NAK(seq=1,CRC) ──┤ → CRC 错误
  ├─ 0x12 Data Pkt(seq=1)(重传) ─────►│
  │◄────────── 0x13 ACK(seq=1,OK) ───┤
  │     ... 重复直到全部发送 ...       │
  │                                   │
  ├─ 0x20 OTA Finish ───────────────►│ → CRC32 全量校验
  │◄─────────── 0x21 Result(0=OK) ───┤
  │                                   │
  ├─ 0x30 Reboot ───────────────────►│ → NVIC_SystemReset()
  │                                   │     ↓
  │                                   │  Bootloader 启动
  │                                   │  → 检测 OTA_STATE_VERIFIED
  │                                   │  → 切换 active_partition
  │                                   │  → 跳转到新固件 ✓
```

## 数据包大小建议

| 场景 | 建议大小 | 原因 |
|------|---------|------|
| 开发调试 | 256 字节 | 容错性好, 重传开销小 |
| 正式使用 | 506 字节 (一帧最大) | 速率最大化 |
| 干扰环境 | 128 字节 | 降低误码率 |

## 超时和重传参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| ACK 等待超时 | 500ms | 等待 STM32 ACK/NAK |
| 最大重传次数 | 3 | 超过后放弃当前 OTA |
| OTA 整体超时 | 无硬限制 | 取决于固件大小 |
| 空闲检测间隔 | 1 小时 | ESP32 检查新版本的间隔 |
