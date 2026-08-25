/**
 * @file    ota_client.h
 * @brief   ESP32 OTA HTTP Client - 鐗堟湰妫€鏌ヤ笌鍥轰欢涓嬭浇
 */

#ifndef __OTA_CLIENT_H
#define __OTA_CLIENT_H

#include <Arduino.h>
#include "config.h"

/* ============================================================
 * 鐗堟湰淇℃伅缁撴瀯
 * ============================================================ */
struct FirmwareInfo {
    String version;       /* "1.0.1" */
    String url;           /* 鍥轰欢涓嬭浇鍦板潃 */
    size_t size;          /* 鏂囦欢澶у皬 (瀛楄妭) */
    uint32_t crc32;       /* CRC32 鏍￠獙鍊?*/
    String changelog;     /* 鏇存柊鏃ュ織 */
    String target_mcu;    /* "STM32F103VET6" */
    int min_boot_ver;     /* 鏈€浣?Bootloader 鐗堟湰 */
};

/* ============================================================
 * OTA 杩愯闃舵
 * ============================================================ */
enum OtaState : uint8_t {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_UPDATE_AVAILABLE,
    OTA_STATE_HTTP_PREPARE,
    OTA_STATE_STM32_START,
    OTA_STATE_TRANSFER,
    OTA_STATE_LOCAL_VERIFY,
    OTA_STATE_STM32_FINISH,
    OTA_STATE_REBOOT_STM32,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED
};

/* ============================================================
 * API
 * ============================================================ */
bool ota_client_init(void);
bool ota_check_update(FirmwareInfo &info);
bool ota_download_and_flash(const FirmwareInfo &info);
void ota_set_server_url(const String &base_url);
OtaState ota_get_state(void);
const char *ota_state_name(OtaState state);
bool ota_is_busy(void);
const char *ota_last_error(void);

#endif /* __OTA_CLIENT_H */
