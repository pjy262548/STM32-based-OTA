/**
 * @file    ota_client.cpp
 * @brief   OTA HTTP Client 实现
 *
 * 流程:
 *   1. GET version.json → 解析版本号
 *   2. 比较 STM32 当前版本 vs 服务器版本
 *   3. 如果有新版本 → HTTP GET 固件文件 (流式)
 *   4. 逐包转发给 STM32
 *   5. 校验 + 重启
 */

#include "ota_client.h"
#include "stm32_bridge.h"
#include "crc_utils.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <stdlib.h>
#include <stdarg.h>

#undef OTA_MAX_FIRMWARE_SIZE
#define OTA_MAX_FIRMWARE_SIZE   (160UL * 1024UL)

#undef OTA_SERVER_BASE_URL
#define OTA_SERVER_BASE_URL     "http://192.168.0.108:8080"

/*
 * Keep the UART protocol payload below the absolute 512-byte ceiling.
 * Payload = seq(2) + offset(4) + data(N), so 500 data bytes make a
 * 506-byte payload and avoid full-boundary corruption seen at 512 bytes.
 */
#define STM32_OTA_DATA_CHUNK_SIZE  500U

#undef PROTOCOL_MAX_PAYLOAD
#define PROTOCOL_MAX_PAYLOAD       (STM32_OTA_DATA_CHUNK_SIZE + 6U)

/* ============================================================
 * 全局
 * ============================================================ */
static String g_server_base_url = OTA_SERVER_BASE_URL;
static bool g_ota_in_progress = false;
static OtaState g_ota_state = OTA_STATE_IDLE;
static char g_ota_last_error[128] = "";

const char *ota_state_name(OtaState state)
{
    switch (state) {
    case OTA_STATE_IDLE:             return "IDLE";
    case OTA_STATE_CHECKING:         return "CHECKING";
    case OTA_STATE_UPDATE_AVAILABLE: return "UPDATE_AVAILABLE";
    case OTA_STATE_HTTP_PREPARE:     return "HTTP_PREPARE";
    case OTA_STATE_STM32_START:      return "STM32_START";
    case OTA_STATE_TRANSFER:         return "TRANSFER";
    case OTA_STATE_LOCAL_VERIFY:     return "LOCAL_VERIFY";
    case OTA_STATE_STM32_FINISH:     return "STM32_FINISH";
    case OTA_STATE_REBOOT_STM32:     return "REBOOT_STM32";
    case OTA_STATE_SUCCESS:          return "SUCCESS";
    case OTA_STATE_FAILED:           return "FAILED";
    default:                         return "UNKNOWN";
    }
}

OtaState ota_get_state(void)
{
    return g_ota_state;
}

bool ota_is_busy(void)
{
    return g_ota_in_progress || g_ota_state == OTA_STATE_CHECKING;
}

const char *ota_last_error(void)
{
    return g_ota_last_error;
}

static void ota_set_state(OtaState state)
{
    if (g_ota_state != state) {
        DEBUG_PRINT("State: %s -> %s",
                    ota_state_name(g_ota_state), ota_state_name(state));
        g_ota_state = state;
    }
}

static void ota_clear_error(void)
{
    g_ota_last_error[0] = '\0';
}

static bool ota_fail(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_ota_last_error, sizeof(g_ota_last_error), fmt, args);
    va_end(args);

    DEBUG_ERROR("%s", g_ota_last_error);
    g_ota_in_progress = false;
    ota_set_state(OTA_STATE_FAILED);
    return false;
}

static String trim_trailing_slashes(String url)
{
    while (url.endsWith("/")) {
        url.remove(url.length() - 1);
    }
    return url;
}

static bool is_absolute_http_url(const String &url)
{
    return url.startsWith("http://") || url.startsWith("https://");
}

static String resolve_firmware_url(String url)
{
    url.trim();
    if (is_absolute_http_url(url)) {
        return url;
    }

    String base = trim_trailing_slashes(g_server_base_url);
    if (url.startsWith("/")) {
        return base + url;
    }
    return base + "/" + url;
}

static bool parse_crc32_field(JsonObject latest, uint32_t &crc32)
{
    JsonVariant crc_node = latest["crc32"];
    if (crc_node.isNull()) {
        return false;
    }

    if (crc_node.is<const char *>()) {
        const char *crc_text = crc_node.as<const char *>();
        if (!crc_text || !crc_text[0]) {
            return false;
        }

        char *end = NULL;
        crc32 = strtoul(crc_text, &end, 16);
        return end && *end == '\0';
    }

    crc32 = crc_node.as<uint32_t>();
    return true;
}

static bool parse_firmware_info(JsonObject latest, FirmwareInfo &info)
{
    if (latest.isNull()) {
        DEBUG_ERROR("Invalid version.json: missing latest object");
        return false;
    }

    info.version = latest["version"] | "";
    info.url = resolve_firmware_url(latest["url"] | "");
    info.size = latest["size"] | 0;
    info.changelog = latest["changelog"] | "";
    info.target_mcu = latest["target_mcu"] | OTA_TARGET_MCU;
    info.min_boot_ver = latest["min_bootloader_version"] | 1;

    if (!parse_crc32_field(latest, info.crc32)) {
        DEBUG_ERROR("Invalid version.json: missing/invalid crc32");
        return false;
    }

    if (info.version.isEmpty() || !semver_valid(info.version.c_str())) {
        DEBUG_ERROR("Invalid version.json: bad version '%s'", info.version.c_str());
        return false;
    }
    if (info.url.isEmpty() || !is_absolute_http_url(info.url)) {
        DEBUG_ERROR("Invalid version.json: bad firmware URL '%s'", info.url.c_str());
        return false;
    }
    if (info.size == 0 || info.size > OTA_MAX_FIRMWARE_SIZE) {
        DEBUG_ERROR("Invalid version.json: bad size=%u, max=%u",
                    (uint32_t)info.size, (uint32_t)OTA_MAX_FIRMWARE_SIZE);
        return false;
    }
    if (info.crc32 == 0) {
        DEBUG_ERROR("Invalid version.json: crc32 cannot be 0");
        return false;
    }
    if (!info.target_mcu.equalsIgnoreCase(OTA_TARGET_MCU)) {
        DEBUG_ERROR("Firmware target mismatch: got '%s', expected '%s'",
                    info.target_mcu.c_str(), OTA_TARGET_MCU);
        return false;
    }

    return true;
}

/* ============================================================
 * 初始化
 * ============================================================ */
bool ota_client_init(void)
{
    ota_set_state(OTA_STATE_IDLE);
    DEBUG_PRINT("OTA Client initialized, server: %s", g_server_base_url.c_str());
    return true;
}

/* ============================================================
 * 设置服务器 URL
 * ============================================================ */
void ota_set_server_url(const String &base_url)
{
    g_server_base_url = base_url;
    DEBUG_PRINT("Server URL changed to: %s", base_url.c_str());
}

/* ============================================================
 * 检查是否有固件更新
 *
 * 1. HTTP GET version.json
 * 2. 询问 STM32 当前版本
 * 3. 比较版本号
 * ============================================================ */
bool ota_check_update(FirmwareInfo &info)
{
    if (g_ota_in_progress) {
        DEBUG_PRINT("OTA already in progress, skipping check");
        return false;
    }

    ota_clear_error();
    ota_set_state(OTA_STATE_CHECKING);

    String version_url = g_server_base_url + OTA_VERSION_PATH;
    DEBUG_PRINT("Checking for updates: %s", version_url.c_str());
    DEBUG_PRINT("WiFi IP=%s, Gateway=%s, RSSI=%d dBm",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.RSSI());

    HTTPClient http;
    if (!http.begin(version_url)) {
        ota_fail("HTTP begin failed, invalid URL: %s", version_url.c_str());
        return false;
    }
    http.setConnectTimeout(5000);
    http.setTimeout(10000);

    int code = http.GET();
    if (code != 200) {
        String reason = http.errorToString(code);
        http.end();
        ota_fail("HTTP GET failed: code=%d, reason=%s", code, reason.c_str());
        return false;
    }

    String response = http.getString();
    http.end();

    /* 解析 JSON */
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
        ota_fail("JSON parse error: %s", err.c_str());
        return false;
    }

    JsonObject latest = doc["latest"];
    if (!parse_firmware_info(latest, info)) {
        return ota_fail("Invalid firmware manifest");
    }

    /* 查询 STM32 当前版本 */
    String stm32_ver = stm32_query_version();
    if (stm32_ver.isEmpty()) {
        return ota_fail("Failed to query STM32 version");
    }

    /* stm32_ver 即版本号, 如 "1.0.0" */
    String cur_ver = stm32_ver;
    cur_ver.trim();

    DEBUG_PRINT("Current: %s, Latest: %s",
                cur_ver.c_str(), info.version.c_str());

    /* 比较版本号 */
    if (!semver_gt(info.version.c_str(), cur_ver.c_str())) {
        DEBUG_PRINT("Already up to date");
        ota_set_state(OTA_STATE_IDLE);
        return false;
    }

    DEBUG_PRINT("Update available: %s → %s", cur_ver.c_str(), info.version.c_str());
    ota_set_state(OTA_STATE_UPDATE_AVAILABLE);
    return true;
}

/* ============================================================
 * 下载固件并转发给 STM32
 *
 * 使用流式下载 (HTTP GET with stream),
 * 边下载边通过 UART 发送给 STM32, 不需要完整缓存固件。
 *
 * 数据流:
 *   HTTP Server → WiFi → ESP32 RAM buffer (512B) → UART → STM32 Flash
 * ============================================================ */
bool ota_download_and_flash(const FirmwareInfo &info)
{
    if (g_ota_in_progress) {
        return ota_fail("OTA already in progress");
    }

    ota_clear_error();
    g_ota_in_progress = true;

    DEBUG_PRINT("============================================");
    DEBUG_PRINT("Starting OTA Download");
    DEBUG_PRINT("  Version: %s", info.version.c_str());
    DEBUG_PRINT("  Size:    %u bytes", info.size);
    DEBUG_PRINT("  CRC32:   0x%08X", info.crc32);
    DEBUG_PRINT("  URL:     %s", info.url.c_str());
    DEBUG_PRINT("============================================");

    /* ---- Step 1: HTTP 预检查，成功后才让 STM32 进入 OTA ---- */
    ota_set_state(OTA_STATE_HTTP_PREPARE);
    HTTPClient http;
    if (!http.begin(info.url)) {
        return ota_fail("HTTP begin firmware failed, invalid URL: %s", info.url.c_str());
    }
    http.setConnectTimeout(5000);
    http.setTimeout(30000);

    int code = http.GET();
    if (code != 200) {
        String reason = http.errorToString(code);
        http.end();
        return ota_fail("HTTP GET firmware failed: code=%d, reason=%s",
                        code, reason.c_str());
    }

    int content_length = http.getSize();
    if (content_length > 0 && (size_t)content_length != info.size) {
        http.end();
        return ota_fail("Firmware size mismatch: manifest=%u, http=%d",
                        (uint32_t)info.size, content_length);
    }

    /* ---- Step 2: 通知 STM32 开始 OTA ---- */
    ota_set_state(OTA_STATE_STM32_START);
    if (!stm32_ota_start(info.size, info.crc32, info.version)) {
        const char *detail = stm32_last_error();
        http.end();
        if (detail && detail[0]) {
            return ota_fail("STM32 refused OTA start: %s", detail);
        }
        return ota_fail("STM32 refused OTA start");
    }

    /* ---- Step 3: HTTP 流式下载 + 逐包发送 ---- */
    ota_set_state(OTA_STATE_TRANSFER);
    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[PROTOCOL_MAX_PAYLOAD - 6];  /* 减去 header (seq+offset) */
    uint16_t seq = 0;
    uint32_t offset = 0;
    uint32_t total_sent = 0;
    uint32_t last_progress_print = 0;
    uint32_t local_crc = crc32_begin();
    const char *transfer_error = "Incomplete download";

    while (total_sent < info.size) {
        if (WiFi.status() != WL_CONNECTED) {
            DEBUG_ERROR("WiFi disconnected during OTA at offset=%u", offset);
            transfer_error = "WiFi disconnected during OTA";
            break;
        }

        size_t remaining = info.size - total_sent;
        size_t to_read = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;

        /* 从 HTTP stream 读取一块数据 */
        size_t len = stream->readBytes(buf, to_read);
        if (len == 0) {
            DEBUG_ERROR("HTTP read timeout at offset=%u", offset);
            transfer_error = "HTTP read timeout";
            break;
        }

        local_crc = crc32_update(local_crc, buf, len);

        /* 通过 UART 发送给 STM32 */
        if (!stm32_ota_send_packet(seq, offset, buf, len)) {
            DEBUG_ERROR("Failed to send packet seq=%d", seq);
            transfer_error = "UART packet transfer failed";
            break;
        }

        offset += len;
        total_sent += len;
        seq++;

        /* 每 10% 打印进度 */
        uint32_t pct = (total_sent * 100 / info.size);
        if (pct - last_progress_print >= 10) {
            last_progress_print = pct;
            DEBUG_PRINT("Progress: %u%% (%u/%u bytes)",
                        pct, total_sent, info.size);
        }

        yield();
    }

    http.end();

    if (total_sent != info.size) {
        return ota_fail("%s: sent %u / %u bytes",
                        transfer_error, total_sent, (uint32_t)info.size);
    }

    ota_set_state(OTA_STATE_LOCAL_VERIFY);
    uint32_t downloaded_crc = crc32_finish(local_crc);
    if (downloaded_crc != info.crc32) {
        return ota_fail("Downloaded CRC mismatch: manifest=0x%08X, local=0x%08X",
                        info.crc32, downloaded_crc);
    }

    DEBUG_PRINT("Download complete: %u bytes, %d packets", total_sent, seq);

    /* ---- Step 4: 通知 STM32 OTA 完毕 ---- */
    ota_set_state(OTA_STATE_STM32_FINISH);
    if (!stm32_ota_finish(info.size)) {
        const char *detail = stm32_last_error();
        if (detail && detail[0]) {
            return ota_fail("OTA finish verification failed: %s", detail);
        }
        return ota_fail("OTA finish verification failed");
    }

    /* ---- Step 5: 重启到新固件 ---- */
    ota_set_state(OTA_STATE_REBOOT_STM32);
    delay(500);  /* 等 STM32 准备好 */
    if (!stm32_ota_verify_and_reboot()) {
        return ota_fail("Reboot command failed");
    }

    DEBUG_PRINT("OTA SUCCESS! STM32 rebooting to new firmware...");
    g_ota_in_progress = false;
    ota_set_state(OTA_STATE_SUCCESS);
    return true;
}
