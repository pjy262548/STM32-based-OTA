#ifndef __OTA_PROTOCOL_H
#define __OTA_PROTOCOL_H

#include "stm32f10x.h"

#define PROTOCOL_SOF            0xAA
#define PROTOCOL_MAX_PAYLOAD    512

#define BOOTLOADER_SIZE         0x4000UL
#define APP_PARTITION_SIZE      0x28000UL
#define APP_A_ADDR              (FLASH_BASE + BOOTLOADER_SIZE)
#define APP_B_ADDR              (APP_A_ADDR + APP_PARTITION_SIZE)
#define APP_C_ADDR              (APP_B_ADDR + APP_PARTITION_SIZE)
#define APP_RUN_ADDR            APP_A_ADDR
#define APP_DOWNLOAD_ADDR       APP_B_ADDR
#define APP_BACKUP_ADDR         APP_C_ADDR
#define OTA_INFO_ADDR           (APP_C_ADDR + APP_PARTITION_SIZE)
#define OTA_INFO_MAGIC          0xAA55AA55UL
#define OTA_INFO_STORE_SIZE     64U

#define CMD_QUERY_VERSION       0x01
#define CMD_REPORT_VERSION      0x02
#define CMD_OTA_START           0x10
#define CMD_OTA_START_ACK       0x11
#define CMD_DATA_PACKET         0x12
#define CMD_DATA_ACK            0x13
#define CMD_OTA_FINISH          0x20
#define CMD_OTA_RESULT          0x21
#define CMD_REBOOT              0x30
#define CMD_ERROR               0xFF

#define ACK_OK                  0x00
#define ACK_CRC_ERROR           0x01
#define ACK_FLASH_ERROR         0x02
#define ACK_BUSY                0x03
#define ACK_INVALID_LENGTH      0x04
#define ACK_INVALID_OFFSET      0x05
#define ACK_INVALID_SIZE        0x06
#define ACK_SEQ_ERROR           0x07

#define ERR_UNKNOWN_CMD         0x01
#define ERR_INVALID_STATE       0x02
#define ERR_FLASH_OP_FAILED     0x03

#define OTA_STATE_IDLE          0x00
#define OTA_STATE_DOWNLOADING   0x01
#define OTA_STATE_COMPLETED     0x02
#define OTA_STATE_VERIFIED      0x03
#define OTA_STATE_PENDING_CONFIRM 0x04
#define OTA_STATE_INSTALLING    0x05
#define OTA_STATE_FAILED        0xFF

#define OTA_TRANSFER_TIMEOUT_MS 30000UL

typedef struct {
    uint8_t  cmd;
    uint16_t len;
    uint8_t  payload[PROTOCOL_MAX_PAYLOAD];
    uint16_t crc;
} protocol_frame_t;

typedef struct {
    uint8_t  active_partition;
    uint8_t  ota_state;
    uint8_t  reserved[2];
    uint32_t new_fw_size;
    uint32_t new_fw_crc32;
    uint8_t  new_version[16];
    uint32_t ota_start_time;
    uint32_t packet_count;
    uint32_t backup_fw_size;
    uint32_t backup_fw_crc32;
    uint32_t boot_attempts;
    uint32_t magic;
} ota_info_t;

typedef struct {
    uint8_t  active;
    uint32_t target_partition;
    uint32_t expected_size;
    uint32_t expected_crc32;
    uint32_t bytes_written;
    uint16_t last_seq;
    uint8_t  version[16];
    uint32_t packet_count;
    uint32_t last_activity_ms;
} ota_context_t;

uint16_t frame_crc16_calc(const uint8_t *data, uint16_t len);

void ota_protocol_init(void);
void ota_confirm_running_app(void);
void ota_protocol_tick(uint32_t now_ms);
void ota_context_reset(void);
ota_context_t *ota_context_get(void);
void ota_process_frame(const protocol_frame_t *frame);
void ota_send_response(uint8_t cmd, const uint8_t *payload, uint16_t len);
void ota_send_error(uint8_t error_code);
void ota_send_version(const char *version);
void ota_send_ack(uint8_t status);
void ota_send_data_ack(uint16_t seq, uint8_t status);
void ota_send_result(uint8_t result);

#endif
