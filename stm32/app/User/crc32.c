#include "crc32.h"

/* CRC32 查找表 (Ethernet polynomial 0x04C11DB7, reflected) */
static uint32_t crc32_table[256];
static uint8_t  crc32_initialized = 0;

/**
 * @brief  生成 CRC32 查找表
 */
static void crc32_generate_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320UL;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = 1;
}

/**
 * @brief  初始化 CRC32 模块 (生成查找表)
 */
void crc32_init(void)
{
    if (!crc32_initialized)
        crc32_generate_table();
}

/**
 * @brief  继续 CRC32 计算
 * @param  crc   当前 CRC 值 (初次调用传 0xFFFFFFFF)
 * @param  data  输入数据
 * @param  len   数据长度
 * @return 更新后的 CRC 值 (最终结果需 ^ 0xFFFFFFFF)
 */
uint32_t crc32_calc_continue(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (!crc32_initialized)
        crc32_generate_table();

    for (uint32_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}
