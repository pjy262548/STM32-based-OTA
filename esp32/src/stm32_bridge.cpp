/**
 * @file    stm32_bridge.cpp
 * @brief   STM32 UART 通信实现
 */

#include "stm32_bridge.h"
#include "crc_utils.h"
#include <HardwareSerial.h>
#include "driver/gpio.h"
#include <stdarg.h>

/* ============================================================
 * 全局变量
 * ============================================================ */
static HardwareSerial *stm32_serial = NULL;
static ota_progress_cb_t progress_cb = NULL;
static char g_stm32_last_error[128] = "";

const char *stm32_last_error(void)
{
    return g_stm32_last_error;
}

static void stm32_clear_last_error(void)
{
    g_stm32_last_error[0] = '\0';
}

static void stm32_set_last_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_stm32_last_error, sizeof(g_stm32_last_error), fmt, args);
    va_end(args);
}

/* ============================================================
 * 初始�?UART
 * ============================================================ */
void stm32_bridge_init(void)
{
    stm32_serial = &Serial2;  /* ESP32 HardwareSerial 2 */
    stm32_serial->setRxBufferSize(2048);
    pinMode(STM32_UART_RX, INPUT_PULLUP);
    stm32_serial->begin(STM32_UART_BAUD, SERIAL_8N1,
                        STM32_UART_RX, STM32_UART_TX);
    stm32_serial->setTimeout(STM32_UART_TIMEOUT_MS);
    DEBUG_PRINT("STM32 Bridge initialized (TX=%d, RX=%d, Baud=%d)",
                STM32_UART_TX, STM32_UART_RX, STM32_UART_BAUD);
    DEBUG_PRINT("=== DIAG: pin-level test ===");
    DEBUG_PRINT("Please confirm wiring: PA2(TX)->GPIO%d(RX), PA3(RX)->GPIO%d(TX), GND->GND",
                STM32_UART_RX, STM32_UART_TX);
    DEBUG_PRINT("If using Elite V2 P5, remove both RS485 jumpers and use the pins labeled PA2/PA3, not 485R/485T");
    DEBUG_PRINT("STM32 PB5 should be blinking (heartbeat)");
}

/* ============================================================
 * 发送协议帧
 * ============================================================ */
static void send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    if (!stm32_serial) return;
    if (len > PROTOCOL_MAX_PAYLOAD) {
        DEBUG_ERROR("Frame payload too large: %u", len);
        return;
    }

    uint16_t crc = (len > 0) ? crc16_ccitt(payload, len) : crc16_ccitt(NULL, 0);

    stm32_serial->write(PROTOCOL_SOF);
    stm32_serial->write(cmd);
    stm32_serial->write((len >> 8) & 0xFF);
    stm32_serial->write(len & 0xFF);
    if (len > 0) {
        stm32_serial->write(payload, len);
    }
    stm32_serial->write((crc >> 8) & 0xFF);
    stm32_serial->write(crc & 0xFF);
    stm32_serial->flush();
}

static void flush_stm32_rx(void)
{
    if (!stm32_serial) return;
    while (stm32_serial->available()) {
        stm32_serial->read();
    }
}

static void probe_rx_gpio_level_named(const char *label, uint32_t timeout_ms)
{
    uint32_t start = millis();
    uint32_t high_count = 0;
    uint32_t low_count = 0;
    uint32_t edge_count = 0;
    int last_level = gpio_get_level((gpio_num_t)STM32_UART_RX);

    while (millis() - start < timeout_ms) {
        int level = gpio_get_level((gpio_num_t)STM32_UART_RX);
        if (level) {
            high_count++;
        } else {
            low_count++;
        }
        if (level != last_level) {
            edge_count++;
            last_level = level;
        }
        delayMicroseconds(100);
    }

    DEBUG_PRINT("[DIAG] GPIO%d %s level probe: high=%lu low=%lu edges=%lu last=%d",
                STM32_UART_RX,
                label,
                (unsigned long)high_count,
                (unsigned long)low_count,
                (unsigned long)edge_count,
                last_level);
}

static void probe_rx_gpio_level(uint32_t timeout_ms)
{
    probe_rx_gpio_level_named("uart", timeout_ms);
}

static void scan_uart_rx_candidates(void)
{
    static const int pins[] = {3, 16, 17, 21, 22, 25, 26, 27, 32, 33};

    DEBUG_PRINT("[DIAG] Scanning GPIO pins for PA2 low pulses...");
    if (stm32_serial) {
        stm32_serial->end();
        delay(20);
    }

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        int pin = pins[i];
        pinMode(pin, INPUT_PULLUP);
        delay(10);
        int idle = digitalRead(pin);
        uint32_t low_pulse_us = pulseIn(pin, LOW, 700000UL);
        DEBUG_PRINT("[DIAG] GPIO%d scan: idle=%d low_pulse=%lu us",
                    pin, idle, (unsigned long)low_pulse_us);
        delay(20);
    }

    if (stm32_serial) {
        stm32_serial->begin(STM32_UART_BAUD, SERIAL_8N1,
                            STM32_UART_RX, STM32_UART_TX);
        stm32_serial->setTimeout(STM32_UART_TIMEOUT_MS);
        flush_stm32_rx();
    }
}
static void probe_rx_gpio_drive(void)
{
    if (!stm32_serial) {
        return;
    }

    DEBUG_PRINT("[DIAG] RX drive probe: temporarily detaching UART from GPIO%d", STM32_UART_RX);
    stm32_serial->end();
    delay(20);

    pinMode(STM32_UART_RX, INPUT_PULLDOWN);
    delay(20);
    probe_rx_gpio_level_named("pulldown", 500);

    pinMode(STM32_UART_RX, INPUT_PULLUP);
    delay(20);
    probe_rx_gpio_level_named("pullup", 500);

    stm32_serial->begin(STM32_UART_BAUD, SERIAL_8N1,
                        STM32_UART_RX, STM32_UART_TX);
    stm32_serial->setTimeout(STM32_UART_TIMEOUT_MS);
    flush_stm32_rx();
}

/* ============================================================
 * 等待并解析响应帧
 *
 * @param expected_cmd: 期望的命令字
 * @param timeout_ms:   超时时间
 * @param payload:      输出 Payload 缓冲�? * @param max_len:      Payload 缓冲区最大长�? * @return 实际 Payload 长度, 0=超时, -1=CRC 错误
 * ============================================================ */
static int wait_for_response(uint8_t expected_cmd, uint32_t timeout_ms,
                              uint8_t *payload, uint16_t max_len)
{
    if (!stm32_serial) return 0;

    uint32_t start = millis();
    enum { IDLE, CMD, LEN_H, LEN_L, PAYLOAD, CRC_H, CRC_L } state = IDLE;
    uint8_t  rx_cmd = 0;
    uint16_t rx_len = 0;
    uint16_t rx_idx = 0;
    uint8_t  rx_payload[PROTOCOL_MAX_PAYLOAD + 10];
    uint16_t rx_crc = 0;

    while (millis() - start < timeout_ms) {
        if (!stm32_serial->available()) {
            delay(1);
            continue;
        }

        uint8_t b = stm32_serial->read();

        switch (state) {
        case IDLE:
            if (b == PROTOCOL_SOF) state = CMD;
            break;
        case CMD:
            rx_cmd = b; state = LEN_H;
            break;
        case LEN_H:
            rx_len = ((uint16_t)b << 8); state = LEN_L;
            break;
        case LEN_L:
            rx_len |= b;
            if (rx_len > PROTOCOL_MAX_PAYLOAD) { state = IDLE; continue; }
            if (rx_len == 0) state = CRC_H;
            else { rx_idx = 0; state = PAYLOAD; }
            break;
        case PAYLOAD:
            rx_payload[rx_idx++] = b;
            if (rx_idx >= rx_len) state = CRC_H;
            break;
        case CRC_H:
            rx_crc = ((uint16_t)b << 8); state = CRC_L;
            break;
        case CRC_L:
            rx_crc |= b;
            /* 帧接收完�?*/
            if (rx_cmd == expected_cmd || expected_cmd == 0) {
                /* CRC 校验 */
                uint16_t calc_crc = crc16_ccitt(rx_payload, rx_len);
                if (calc_crc == rx_crc) {
                    uint16_t copy_len = (rx_len < max_len) ? rx_len : max_len;
                    if (payload && copy_len > 0) {
                        memcpy(payload, rx_payload, copy_len);
                    }
                    return rx_len;  /* 成功 */
                } else {
                    return -1;  /* CRC 错误 */
                }
            }
            /* 不匹配期望的命令, 继续等待 */
            state = IDLE;
            break;
        }
    }

    return 0;  /* 超时 */
}

static int capture_raw_response(uint8_t *raw, int max_len, uint32_t timeout_ms)
{
    if (!stm32_serial || !raw || max_len <= 0) return 0;

    int raw_len = 0;
    uint32_t start = millis();
    while (millis() - start < timeout_ms && raw_len < max_len) {
        if (stm32_serial->available()) {
            raw[raw_len++] = stm32_serial->read();
        } else {
            delay(1);
        }
    }
    return raw_len;
}

static void print_raw_response(const uint8_t *raw, int raw_len, int max_len)
{
    if (!raw || raw_len <= 0) return;

    LOG_PRINTF("[DIAG] Raw response (%d bytes):", raw_len);
    for (int i = 0; i < raw_len; i++) {
        if (i % 16 == 0) LOG_PRINTLN();
        LOG_PRINTF(" %02X", raw[i]);
    }
    LOG_PRINTLN();

    bool has_sof = false;
    int zero_or_ff_count = 0;
    for (int i = 0; i < raw_len; i++) {
        if (raw[i] == PROTOCOL_SOF) {
            has_sof = true;
        }
        if (raw[i] == 0x00 || raw[i] == 0xFF) {
            zero_or_ff_count++;
        }
    }

    if (!has_sof) {
        DEBUG_PRINT("[DIAG] No SOF (0x%02X) found in response", PROTOCOL_SOF);
    }
    if (raw_len >= max_len) {
        DEBUG_PRINT("[DIAG] Raw buffer filled; RX line may be noisy or connected to the wrong signal");
    }
    if (!has_sof && raw_len >= 8 && zero_or_ff_count * 100 / raw_len >= 80) {
        DEBUG_PRINT("[DIAG] Mostly 0x00/0xFF: check GND, baud, RX pin, P5 side, and RS485 jumpers");
    }
}

bool stm32_rx_probe(uint32_t timeout_ms)
{
    if (!stm32_serial) return false;

    DEBUG_PRINT("[DIAG] Passive RX probe on GPIO%d for %lu ms...",
                STM32_UART_RX, (unsigned long)timeout_ms);

    flush_stm32_rx();

    uint8_t raw[128];
    int raw_len = capture_raw_response(raw, sizeof(raw), timeout_ms);
    if (raw_len <= 0) {
        DEBUG_PRINT("[DIAG] Passive RX probe: no bytes received");
        probe_rx_gpio_level(500);
        probe_rx_gpio_drive();
        scan_uart_rx_candidates();
        return false;
    }

    LOG_PRINTF("[DIAG] Passive RX raw (%d bytes):", raw_len);
    for (int i = 0; i < raw_len; i++) {
        if (i % 16 == 0) LOG_PRINTLN();
        LOG_PRINTF(" %02X", raw[i]);
    }
    LOG_PRINTLN();

    LOG_PRINT("[DIAG] Passive RX ascii: ");
    for (int i = 0; i < raw_len; i++) {
        char c = (char)raw[i];
        LOG_PRINT((c >= 32 && c <= 126) ? c : '.');
    }
    LOG_PRINTLN();

    static const char needle[] = "PA2 OK";
    for (int i = 0; i <= raw_len - (int)(sizeof(needle) - 1); i++) {
        if (memcmp(raw + i, needle, sizeof(needle) - 1) == 0) {
            DEBUG_PRINT("[DIAG] Passive RX probe OK: saw PA2 OK on GPIO%d",
                        STM32_UART_RX);
            return true;
        }
    }

    DEBUG_PRINT("[DIAG] Passive RX probe did not see PA2 OK; GPIO%d may be on the wrong wire",
                STM32_UART_RX);
    return false;
}

static String version_from_payload(const uint8_t *buf, uint16_t len)
{
    if (!buf) {
        return "";
    }

    const uint8_t *version_ptr = buf;
    if (len >= 17 && (buf[0] == 'A' || buf[0] == 'B')) {
        version_ptr = buf + 1;  /* Backward compatible with old "A/B + version" payload. */
    } else if (len < 16) {
        return "";
    }

    char version[17];
    memcpy(version, version_ptr, 16);
    version[16] = '\0';

    if (!semver_valid(version)) {
        return "";
    }

    return String(version);
}

/* ============================================================
 * 测试 STM32 连接
 * ============================================================ */
bool stm32_ping(void)
{
    flush_stm32_rx();
    delay(50);

    DEBUG_PRINT("[DIAG] Sending CMD_QUERY_VERSION frame...");
    send_frame(CMD_QUERY_VERSION, NULL, 0);

    uint8_t buf[32];
    int ret = wait_for_response(CMD_REPORT_VERSION, 1000, buf, sizeof(buf));
    String ver = version_from_payload(buf, ret > 0 ? ret : 0);
    if (!ver.isEmpty()) {
        DEBUG_PRINT("[DIAG] Valid STM32 version response: %s", ver.c_str());
        return true;
    }

    DEBUG_PRINT("[DIAG] No valid protocol response (ret=%d), capturing raw bytes...", ret);
    flush_stm32_rx();
    delay(20);
    send_frame(CMD_QUERY_VERSION, NULL, 0);

    uint8_t raw[64];
    int raw_len = capture_raw_response(raw, sizeof(raw), 1000);
    if (raw_len > 0) {
        print_raw_response(raw, raw_len, sizeof(raw));
    } else {
        DEBUG_PRINT("[DIAG] NO response within 1 second");
    }
    return false;
}

/* ============================================================
 * 查询 STM32 当前固件版本
 * ============================================================ */
String stm32_query_version(void)
{
    flush_stm32_rx();
    delay(20);
    send_frame(CMD_QUERY_VERSION, NULL, 0);

    uint8_t buf[32];
    int ret = wait_for_response(CMD_REPORT_VERSION, STM32_UART_TIMEOUT_MS,
                                buf, sizeof(buf));
    String ver = version_from_payload(buf, ret > 0 ? ret : 0);
    if (!ver.isEmpty()) {
        DEBUG_PRINT("STM32 version: %s", ver.c_str());
        return ver;
    }

    DEBUG_ERROR("Failed to query STM32 version (ret=%d)", ret);
    return "";
}

/* ============================================================
 * 开�?OTA 流程
 * ============================================================ */
bool stm32_ota_start(uint32_t fw_size, uint32_t fw_crc32,
                     const String &version)
{
    stm32_clear_last_error();

    uint8_t payload[24];
    memset(payload, 0, sizeof(payload));

    /* file_size(4) + crc32(4) + version(16) */
    payload[0]  = (fw_size >> 24) & 0xFF;
    payload[1]  = (fw_size >> 16) & 0xFF;
    payload[2]  = (fw_size >> 8)  & 0xFF;
    payload[3]  = fw_size & 0xFF;

    payload[4]  = (fw_crc32 >> 24) & 0xFF;
    payload[5]  = (fw_crc32 >> 16) & 0xFF;
    payload[6]  = (fw_crc32 >> 8)  & 0xFF;
    payload[7]  = fw_crc32 & 0xFF;

    strncpy((char *)(payload + 8), version.c_str(), 16);

    DEBUG_PRINT("Starting OTA: size=%u, crc=0x%08X, ver=%s",
                fw_size, fw_crc32, version.c_str());

    flush_stm32_rx();
    delay(20);
    send_frame(CMD_OTA_START, payload, sizeof(payload));

    uint8_t ack = 0xFF;
    int ret = wait_for_response(CMD_OTA_START_ACK, STM32_UART_TIMEOUT_MS * 2,
                                &ack, 1);
    if (ret == 1 && ack == ACK_OK) {
        DEBUG_PRINT("OTA start ACK received");
        return true;
    }

    DEBUG_ERROR("OTA start failed: ret=%d, ack=0x%02X", ret, ack);
    stm32_set_last_error("OTA start failed: ret=%d, ack=0x%02X", ret, ack);
    return false;
}

/* ============================================================
 * 发送单个数据包
 * ============================================================ */
bool stm32_ota_send_packet(uint16_t seq, uint32_t offset,
                           const uint8_t *data, uint16_t len)
{
    if (!data || len == 0 || len + 6 > PROTOCOL_MAX_PAYLOAD) {
        DEBUG_ERROR("Invalid packet len=%u", len);
        stm32_set_last_error("invalid packet len=%u", len);
        return false;
    }

    /* Payload: seq(2) + offset(4) + data(N) */
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
    payload[0] = (seq >> 8) & 0xFF;
    payload[1] = seq & 0xFF;
    payload[2] = (offset >> 24) & 0xFF;
    payload[3] = (offset >> 16) & 0xFF;
    payload[4] = (offset >> 8)  & 0xFF;
    payload[5] = offset & 0xFF;
    memcpy(payload + 6, data, len);

    for (int retry = 0; retry <= STM32_PACKET_RETRY_MAX; retry++) {
        flush_stm32_rx();
        send_frame(CMD_DATA_PACKET, payload, len + 6);

        uint8_t ack[3];  /* seq(2) + status(1) */
        int ret = wait_for_response(CMD_DATA_ACK, STM32_UART_TIMEOUT_MS,
                                    ack, sizeof(ack));
        if (ret == 3) {
            uint16_t ack_seq = (ack[0] << 8) | ack[1];
            uint8_t  ack_status = ack[2];

            if (ack_seq == seq && ack_status == ACK_OK) {
                if (progress_cb) {
                    progress_cb(offset + len, 0);
                }
                return true;
            }
            if (ack_status == ACK_FLASH_ERROR) {
                DEBUG_ERROR("Flash write error at seq=%d", seq);
                stm32_set_last_error("flash write error at seq=%u", seq);
                return false;
            }
            if (ack_seq != seq) {
                DEBUG_PRINT("Retry seq=%d, ack_seq=%d, status=%d",
                            seq, ack_seq, ack_status);
            }
            /* CRC error �?重传 */
            else {
                DEBUG_PRINT("Retry seq=%d, status=%d", seq, ack_status);
            }
        } else {
            DEBUG_PRINT("Timeout seq=%d (retry %d)", seq, retry);
        }
        delay(50);
    }

    DEBUG_ERROR("Packet seq=%d failed after %d retries", seq,
                STM32_PACKET_RETRY_MAX);
    stm32_set_last_error("packet seq=%u failed after retries", seq);
    return false;
}

/* ============================================================
 * OTA 传输完毕, 请求全量校验
 * ============================================================ */
bool stm32_ota_finish(uint32_t fw_size)
{
    stm32_clear_last_error();

    uint8_t payload[4];
    payload[0] = (fw_size >> 24) & 0xFF;
    payload[1] = (fw_size >> 16) & 0xFF;
    payload[2] = (fw_size >> 8)  & 0xFF;
    payload[3] = fw_size & 0xFF;

    DEBUG_PRINT("Finishing OTA, total size=%u", fw_size);

    flush_stm32_rx();
    delay(20);
    send_frame(CMD_OTA_FINISH, payload, sizeof(payload));

    uint8_t result = 0xFF;
    int ret = wait_for_response(CMD_OTA_RESULT, STM32_UART_TIMEOUT_MS * 5,
                                &result, 1);
    if (ret == 1 && result == 0) {
        DEBUG_PRINT("OTA verify PASSED");
        return true;
    }

    DEBUG_ERROR("OTA verify FAILED: ret=%d, code=0x%02X", ret, result);
    stm32_set_last_error("finish ret=%d, result=0x%02X", ret, result);
    return false;
}

/* ============================================================
 * 校验通过后重启到新固�? * ============================================================ */
bool stm32_ota_verify_and_reboot(void)
{
    DEBUG_PRINT("Sending reboot command...");
    send_frame(CMD_REBOOT, NULL, 0);
    /* STM32 会立刻复�? 不需要等响应 */
    delay(100);
    return true;
}

/* ============================================================
 * 设置进度回调
 * ============================================================ */
void stm32_set_progress_callback(ota_progress_cb_t cb)
{
    progress_cb = cb;
}
