/**
 * @file    main.cpp
 * @brief   ESP32 OTA Bridge - 主程序入�? *
 * 功能:
 *   1. WiFi 连接管理 (自动重连)
 *   2. 定时检�?OTA 服务器是否有新固�? *   3. 有新版本 �?下载 + 转发�?STM32
 *   4. STM32 �?Flash �?校验 �?重启
 *
 * LED 指示:
 *   - 慢闪 (2s): WiFi 未连�? *   - 常亮: 正常工作
 *   - 快闪: OTA 进行�? */

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "ota_client.h"
#include "stm32_bridge.h"

/* ============================================================
 * LED 引脚 (ESP32 板载通常�?GPIO2)
 * ============================================================ */
#define LED_PIN         2
#define LED_ON()        digitalWrite(LED_PIN, LOW)   /* 低电平亮 */
#define LED_OFF()       digitalWrite(LED_PIN, HIGH)
#define LED_TOGGLE()    digitalWrite(LED_PIN, !digitalRead(LED_PIN))

static void ota_progress_indicator(uint32_t bytes_sent, uint32_t total)
{
    (void)bytes_sent;
    (void)total;

    static uint32_t last_toggle = 0;
    uint32_t now = millis();

    if (now - last_toggle > 150) {
        last_toggle = now;
        LED_TOGGLE();
    }
}

/* ============================================================
 * WiFi 事件回调
 * ============================================================ */
static volatile bool wifi_connected = false;
static bool wifi_event_registered = false;

static void wifi_event_handler(WiFiEvent_t event)
{
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        wifi_connected = true;
        LOG_PRINTLN();
        LOG_PRINT("WiFi connected! IP: ");
        LOG_PRINTLN(WiFi.localIP());
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        wifi_connected = false;
        LOG_PRINTLN("WiFi disconnected");
        break;
    default:
        break;
    }
}

/* ============================================================
 * WiFi 连接
 * ============================================================ */
static bool wifi_connect(void)
{
    LOG_PRINTF("\nConnecting to WiFi: %s\n", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    if (!wifi_event_registered) {
        WiFi.persistent(false);
        WiFi.setAutoReconnect(true);
        WiFi.onEvent(wifi_event_handler);
        wifi_event_registered = true;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        LOG_PRINT("WiFi already connected! IP: ");
        LOG_PRINTLN(WiFi.localIP());
        return true;
    }

    WiFi.disconnect(false);
    delay(300);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    /* 等待连接 */
    uint32_t start = millis();
    while (!wifi_connected && (millis() - start < WIFI_CONNECT_TIMEOUT_MS)) {
        delay(500);
        LOG_PRINT(".");
    }
    LOG_PRINTLN();

    return wifi_connected;
}

static bool time_reached(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

/* ============================================================
 * 初始�? * ============================================================ */
void setup(void)
{
    /* ---- 1. 串口 (调试) ---- */
    LOG_BEGIN(115200);
    delay(1000);
    LOG_PRINTLN();
    LOG_PRINTLN("========================================");
    LOG_PRINTLN("  ESP32 OTA Bridge for STM32F103 (Elite Board)");
    LOG_PRINTLN("========================================");

    /* ---- 2. LED ---- */
    pinMode(LED_PIN, OUTPUT);
    LED_OFF();

    /* ---- 3. STM32 UART 初始�?---- */
    stm32_bridge_init();
    stm32_set_progress_callback(ota_progress_indicator);
    ota_client_init();
#if STM32_RX_PROBE_ENABLE
    stm32_rx_probe(STM32_RX_PROBE_TIMEOUT_MS);
#endif

    /* ---- 4. WiFi 连接 ---- */
    if (!wifi_connect()) {
        LOG_PRINTLN("ERROR: WiFi connection failed!");
        /* 即使 WiFi 失败也继�? 后续会定时重�?*/
    }

#if STM32_RX_PROBE_ENABLE
    stm32_rx_probe(STM32_RX_PROBE_TIMEOUT_MS);
#endif


    /* ---- 5. 检�?STM32 连接 ---- */
    delay(2000);
    if (stm32_ping()) {
        LOG_PRINTLN("STM32 connected OK");
    } else {
        LOG_PRINTLN("WARNING: STM32 not responding, check UART wiring");
    }

    LOG_PRINTLN("Setup complete. Starting main loop...");
}

/* ============================================================
 * 主循�? * ============================================================ */
void loop(void)
{
    static uint32_t next_ota_check = 0;
    static uint32_t last_led_toggle = 0;
    static uint32_t last_wifi_retry = 0;
    static uint32_t last_log_heartbeat = 0;

    uint32_t now = millis();

    if (wifi_connected && now - last_log_heartbeat >= 5000) {
        last_log_heartbeat = now;
        LOG_PRINTF("[HEARTBEAT] ESP32 alive, IP=%s, RSSI=%d dBm\n",
                   WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }

    /* ---- LED 指示 ---- */
    if (ota_is_busy()) {
        if (now - last_led_toggle > 150) {
            last_led_toggle = now;
            LED_TOGGLE();
        }
    } else if (wifi_connected) {
        /* 正常: LED 常亮 */
        LED_ON();
    } else {
        /* 未连�? LED 慢闪 */
        if (now - last_led_toggle > 1000) {
            last_led_toggle = now;
            LED_TOGGLE();
        }
        /* 定时重试 WiFi */
        if (now - last_wifi_retry >= WIFI_RETRY_INTERVAL_MS) {
            last_wifi_retry = now;
            wifi_connect();
        }
    }

    /* ---- OTA 检�?---- */
    if (wifi_connected) {
        if (next_ota_check == 0) {
            next_ota_check = now + OTA_FIRST_CHECK_DELAY_MS;
        }

        if (!ota_is_busy() && time_reached(now, next_ota_check)) {
            next_ota_check = now + OTA_CHECK_INTERVAL_MS;

            FirmwareInfo info;
            if (ota_check_update(info)) {
                LOG_PRINTLN("\n>>> New firmware found, starting OTA...");

                if (ota_download_and_flash(info)) {
                    LOG_PRINTLN(">>> OTA completed successfully!");
                    next_ota_check = millis() + OTA_CHECK_INTERVAL_MS;
                } else {
                    LOG_PRINTLN(">>> OTA FAILED!");
                    if (ota_last_error()[0] != '\0') {
                        LOG_PRINTF(">>> Last error: %s\n", ota_last_error());
                    }
                    next_ota_check = millis() + OTA_FAIL_RETRY_INTERVAL_MS;
                }
            } else if (ota_get_state() == OTA_STATE_FAILED) {
                if (ota_last_error()[0] != '\0') {
                    LOG_PRINTF(">>> OTA check failed: %s\n", ota_last_error());
                }
                next_ota_check = millis() + OTA_FAIL_RETRY_INTERVAL_MS;
            }
        }
    }

    delay(100);  /* 降低 CPU 占用 */
}
