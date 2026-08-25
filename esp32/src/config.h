/**
 * @file    config.h
 * @brief   ESP32 OTA Bridge 配置文件
 *
 * 使用前请修改:
 *   1. WiFi 凭据 (WIFI_SSID / WIFI_PASSWORD)
 *   2. OTA 服务�?URL (OTA_SERVER_BASE_URL)
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <stdio.h>

/* ============================================================
 * WiFi 配置
 * ============================================================ */
#define WIFI_SSID               "ChinaNet-2"
#define WIFI_PASSWORD           "12345678"
#define WIFI_CONNECT_TIMEOUT_MS 15000       /* WiFi 连接超时 (ms) */
#define WIFI_RETRY_INTERVAL_MS  30000       /* WiFi 断线重试间隔 */

/* ============================================================
 * OTA 服务器配�? *
 * 开发阶�? 电脑 IP + 端口
 *   �? "http://192.168.1.100:8080"
 *
 * 生产环境: 云服务器 / OSS 地址
 *   �? "https://ota.example.com"
 * ============================================================ */
#define OTA_SERVER_BASE_URL     "http://192.168.0.108:8080"
#define OTA_VERSION_PATH        "/firmware/version.json"
#define OTA_TARGET_MCU          "STM32F103VET6"
#define OTA_MAX_FIRMWARE_SIZE   (240UL * 1024UL)  /* STM32 OTA App 分区上限 */

/* OTA 检查间�?(毫秒) */
#define OTA_CHECK_INTERVAL_MS   3600000UL   /* 1 小时 */
#define OTA_FAIL_RETRY_INTERVAL_MS 300000UL /* 失败�?5 分钟重试 */

/* 启动后首次检查延�?(毫秒) */
#define OTA_FIRST_CHECK_DELAY_MS 30000     /* 30 �?(�?STM32 启动) */

/* ============================================================
 * STM32 通信配置
 * ============================================================ */
#ifndef UART_TX_PIN
#define UART_TX_PIN             17
#endif
#ifndef UART_RX_PIN
#define UART_RX_PIN             16
#endif
#ifndef UART_BAUD
#define UART_BAUD               115200
#endif

#ifndef USE_USB_RXTX_FOR_STM32
#define USE_USB_RXTX_FOR_STM32  0
#endif

#if USE_USB_RXTX_FOR_STM32
#undef UART_TX_PIN
#undef UART_RX_PIN
#define UART_TX_PIN             1
#define UART_RX_PIN             3
#endif
#define STM32_UART_BAUD         UART_BAUD
#define STM32_UART_TX           UART_TX_PIN
#define STM32_UART_RX           UART_RX_PIN
#define STM32_UART_TIMEOUT_MS   2000       /* UART 响应超时 */
#define STM32_PACKET_RETRY_MAX  3
#ifndef STM32_RX_PROBE_ENABLE
#define STM32_RX_PROBE_ENABLE   1
#endif
#define STM32_RX_PROBE_TIMEOUT_MS 2000

/* ============================================================
 * 调试
 * ============================================================ */
#ifndef REMOTE_LOG_ENABLE
#define REMOTE_LOG_ENABLE       USE_USB_RXTX_FOR_STM32
#endif
#ifndef REMOTE_LOG_HOST
#define REMOTE_LOG_HOST         "192.168.0.108"
#endif
#ifndef REMOTE_LOG_PORT
#define REMOTE_LOG_PORT         4210
#endif
#ifndef REMOTE_LOG_LOCAL_PORT
#define REMOTE_LOG_LOCAL_PORT   4211
#endif

#ifndef DEBUG_ENABLE
#if USE_USB_RXTX_FOR_STM32 && !REMOTE_LOG_ENABLE
#define DEBUG_ENABLE            0
#else
#define DEBUG_ENABLE            1
#endif
#endif

#if REMOTE_LOG_ENABLE
  #include "remote_logger.h"
  #define LOG_BEGIN(baud)       do { (void)(baud); remote_log_begin(REMOTE_LOG_HOST, REMOTE_LOG_PORT, REMOTE_LOG_LOCAL_PORT); } while (0)
  #define LOG_PRINT(...)        remote_log_print(__VA_ARGS__)
  #define LOG_PRINTLN(...)      remote_log_println(__VA_ARGS__)
  #define LOG_PRINTF(...)       remote_log_printf(__VA_ARGS__)
  #define DEBUG_PRINT(fmt, ...) LOG_PRINTF("[OTA] " fmt "\n", ##__VA_ARGS__)
  #define DEBUG_ERROR(fmt, ...) LOG_PRINTF("[OTA] ERROR: " fmt "\n", ##__VA_ARGS__)
#elif DEBUG_ENABLE
  #define LOG_BEGIN(baud)       Serial.begin(baud)
  #define LOG_PRINT(...)        Serial.print(__VA_ARGS__)
  #define LOG_PRINTLN(...)      Serial.println(__VA_ARGS__)
  #define LOG_PRINTF(...)       Serial.printf(__VA_ARGS__)
  #define DEBUG_PRINT(fmt, ...) LOG_PRINTF("[OTA] " fmt "\n", ##__VA_ARGS__)
  #define DEBUG_ERROR(fmt, ...) LOG_PRINTF("[OTA] ERROR: " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_BEGIN(baud)       do { (void)(baud); } while (0)
  #define LOG_PRINT(...)        do { } while (0)
  #define LOG_PRINTLN(...)      do { } while (0)
  #define LOG_PRINTF(...)       do { } while (0)
  #define DEBUG_PRINT(fmt, ...) do { } while (0)
  #define DEBUG_ERROR(fmt, ...) do { } while (0)
#endif
static inline bool semver_parse(const char *ver, int out[3])
{
    char tail = '\0';
    if (!ver || sscanf(ver, "%d.%d.%d%c", &out[0], &out[1], &out[2], &tail) != 3) {
        return false;
    }
    return out[0] >= 0 && out[1] >= 0 && out[2] >= 0;
}

static inline bool semver_valid(const char *ver)
{
    int parsed[3] = {0};
    return semver_parse(ver, parsed);
}

/* 语义化版本比�? 返回 1 如果 a > b, 0 如果 a <= b 或格式非�?*/
static inline int semver_gt(const char *a, const char *b)
{
    int va[3] = {0}, vb[3] = {0};
    if (!semver_parse(a, va) || !semver_parse(b, vb)) {
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] > vb[i];
    }
    return 0; /* equal */
}

#endif /* __CONFIG_H */
