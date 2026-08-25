#include "ota_protocol.h"
#include "flash_writer.h"
#include "version.h"
#include "crc32.h"
#include "uart_handler.h"
#include <string.h>

static ota_context_t g_ctx;
static uint32_t g_ota_now_ms;

static uint16_t load_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           p[3];
}

static uint32_t target_partition_for_current(void)
{
    return APP_DOWNLOAD_ADDR;
}

static void ota_system_reset(void)
{
    for (volatile uint32_t i = 0; i < 720000UL; i++) {
        __NOP();
    }

    __disable_irq();
    SCB->AIRCR = 0x05FA0004;
    while (1) {
        __NOP();
    }
}

static uint8_t current_partition(void)
{
    return 'A';
}

static int ota_size_valid(uint32_t size)
{
    return size > 0 && size <= APP_PARTITION_SIZE;
}

static int ota_range_valid(uint32_t offset, uint32_t len)
{
    if (len == 0) {
        return 0;
    }
    if (offset > g_ctx.expected_size) {
        return 0;
    }
    if (len > (g_ctx.expected_size - offset)) {
        return 0;
    }
    return 1;
}

static int ota_pending_confirm(void)
{
    uint8_t info_buf[OTA_INFO_STORE_SIZE];
    ota_info_t *info = (ota_info_t *)info_buf;

    memset(info_buf, 0, sizeof(info_buf));
    ota_info_read(info_buf);
    return info->magic == OTA_INFO_MAGIC &&
           info->ota_state == OTA_STATE_PENDING_CONFIRM;
}

static int ota_write_state(uint8_t state)
{
    uint8_t info_buf[OTA_INFO_STORE_SIZE];
    ota_info_t *info = (ota_info_t *)info_buf;

    memset(info_buf, 0, sizeof(info_buf));
    info->active_partition = current_partition();
    info->ota_state = state;
    info->new_fw_size = g_ctx.expected_size;
    info->new_fw_crc32 = g_ctx.expected_crc32;
    memcpy(info->new_version, g_ctx.version, sizeof(info->new_version));
    info->packet_count = g_ctx.packet_count;
    info->magic = OTA_INFO_MAGIC;

    return ota_info_write(info_buf);
}

void ota_confirm_running_app(void)
{
    uint8_t info_buf[OTA_INFO_STORE_SIZE];
    ota_info_t *info = (ota_info_t *)info_buf;

    memset(info_buf, 0, sizeof(info_buf));
    ota_info_read(info_buf);

    if (info->magic != OTA_INFO_MAGIC) {
        return;
    }
    if (info->ota_state != OTA_STATE_PENDING_CONFIRM) {
        return;
    }
    if (info->active_partition != current_partition()) {
        return;
    }

    info->ota_state = OTA_STATE_IDLE;
    info->backup_fw_size = 0;
    info->backup_fw_crc32 = 0;
    info->boot_attempts = 0;
    ota_info_write(info_buf);
}

void ota_protocol_tick(uint32_t now_ms)
{
    g_ota_now_ms = now_ms;

    if (!g_ctx.active) {
        return;
    }
    if ((uint32_t)(now_ms - g_ctx.last_activity_ms) <= OTA_TRANSFER_TIMEOUT_MS) {
        return;
    }

    ota_write_state(OTA_STATE_FAILED);
    ota_context_reset();
}

uint16_t frame_crc16_calc(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

void ota_protocol_init(void)
{
    ota_context_reset();
}

void ota_context_reset(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.last_seq = 0xFFFF;
}

ota_context_t *ota_context_get(void)
{
    return &g_ctx;
}

void ota_process_frame(const protocol_frame_t *frame)
{
    uint16_t calc_crc = frame_crc16_calc(frame->payload, frame->len);
    if (calc_crc != frame->crc) {
        ota_send_error(ACK_CRC_ERROR);
        return;
    }

    switch (frame->cmd) {
    case CMD_QUERY_VERSION:
        ota_send_version(FW_VERSION_STRING);
        break;

    case CMD_OTA_START: {
        if (g_ctx.active) {
            ota_send_ack(ACK_BUSY);
            return;
        }
        if (ota_pending_confirm()) {
            ota_send_ack(ACK_BUSY);
            return;
        }
        if (frame->len < 24) {
            ota_send_ack(ACK_INVALID_LENGTH);
            return;
        }

        uint32_t firmware_size = load_be32(frame->payload);
        uint32_t firmware_crc32 = load_be32(frame->payload + 4);
        if (!ota_size_valid(firmware_size)) {
            ota_send_ack(ACK_INVALID_SIZE);
            return;
        }

        ota_context_reset();
        g_ctx.active = 1;
        g_ctx.target_partition = target_partition_for_current();
        g_ctx.expected_size = firmware_size;
        g_ctx.expected_crc32 = firmware_crc32;
        memcpy(g_ctx.version, frame->payload + 8, sizeof(g_ctx.version));
        g_ctx.last_activity_ms = g_ota_now_ms;

        if (ota_write_state(OTA_STATE_DOWNLOADING) != 0) {
            ota_context_reset();
            ota_send_ack(ACK_FLASH_ERROR);
            return;
        }

        if (flash_erase_app_area(g_ctx.target_partition, firmware_size) != 0) {
            ota_write_state(OTA_STATE_FAILED);
            ota_context_reset();
            ota_send_ack(ACK_FLASH_ERROR);
            return;
        }

        ota_send_ack(ACK_OK);
        break;
    }

    case CMD_DATA_PACKET: {
        if (!g_ctx.active) {
            ota_send_error(ERR_INVALID_STATE);
            return;
        }
        if (frame->len < 6) {
            ota_send_error(ACK_INVALID_LENGTH);
            return;
        }

        uint16_t seq = load_be16(frame->payload);
        uint32_t offset = load_be32(frame->payload + 2);
        uint32_t data_len = frame->len - 6;
        g_ctx.last_activity_ms = g_ota_now_ms;

        if (!ota_range_valid(offset, data_len)) {
            ota_send_data_ack(seq, ACK_INVALID_OFFSET);
            return;
        }

        if (g_ctx.packet_count > 0 && seq == g_ctx.last_seq &&
            (offset + data_len) == g_ctx.bytes_written) {
            ota_send_data_ack(seq, ACK_OK);
            return;
        }

        uint16_t expected_seq = (g_ctx.packet_count == 0) ? 0 : (uint16_t)(g_ctx.last_seq + 1);
        if (seq != expected_seq) {
            ota_send_data_ack(seq, ACK_SEQ_ERROR);
            return;
        }
        if (offset != g_ctx.bytes_written) {
            ota_send_data_ack(seq, ACK_INVALID_OFFSET);
            return;
        }

        if (flash_write_data(g_ctx.target_partition + offset, frame->payload + 6, data_len) != 0) {
            ota_write_state(OTA_STATE_FAILED);
            ota_context_reset();
            ota_send_data_ack(seq, ACK_FLASH_ERROR);
            return;
        }

        g_ctx.bytes_written += data_len;
        g_ctx.last_seq = seq;
        g_ctx.packet_count++;
        ota_send_data_ack(seq, ACK_OK);
        break;
    }

    case CMD_OTA_FINISH: {
        if (!g_ctx.active) {
            ota_send_error(ERR_INVALID_STATE);
            return;
        }
        g_ctx.last_activity_ms = g_ota_now_ms;
        if (frame->len >= 4) {
            uint32_t reported_size = load_be32(frame->payload);
            if (reported_size != g_ctx.expected_size) {
                ota_write_state(OTA_STATE_FAILED);
                ota_send_result(1);
                ota_context_reset();
                return;
            }
        }
        if (g_ctx.bytes_written != g_ctx.expected_size) {
            ota_write_state(OTA_STATE_FAILED);
            ota_send_result(1);
            ota_context_reset();
            return;
        }

        uint32_t actual_crc32 = flash_calc_crc32(g_ctx.target_partition, g_ctx.bytes_written);
        if (actual_crc32 != g_ctx.expected_crc32) {
            ota_write_state(OTA_STATE_FAILED);
            ota_send_result(1);
            ota_context_reset();
            return;
        }

        if (ota_write_state(OTA_STATE_VERIFIED) != 0) {
            ota_send_result(1);
            ota_context_reset();
            return;
        }

        ota_send_result(0);
        ota_context_reset();
        break;
    }

    case CMD_REBOOT:
        ota_system_reset();
        break;

    default:
        ota_send_error(ERR_UNKNOWN_CMD);
        break;
    }
}

void ota_send_response(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uart_send_frame(cmd, payload, len);
}

void ota_send_error(uint8_t error_code)
{
    uart_send_frame(CMD_ERROR, &error_code, 1);
}

void ota_send_version(const char *version)
{
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    strncpy((char *)payload, version, 16);
    uart_send_frame(CMD_REPORT_VERSION, payload, sizeof(payload));
}

void ota_send_ack(uint8_t status)
{
    uart_send_frame(CMD_OTA_START_ACK, &status, 1);
}

void ota_send_data_ack(uint16_t seq, uint8_t status)
{
    uint8_t payload[3];
    payload[0] = (uint8_t)(seq >> 8);
    payload[1] = (uint8_t)(seq & 0xFF);
    payload[2] = status;
    uart_send_frame(CMD_DATA_ACK, payload, sizeof(payload));
}

void ota_send_result(uint8_t result)
{
    uart_send_frame(CMD_OTA_RESULT, &result, 1);
}
