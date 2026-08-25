#!/usr/bin/env python3
"""
通过串口烧录 STM32 Bootloader (使用 STM32 内建 Bootloader)

依赖:
    pip install pyserial

STM32F103 内建 Bootloader (系统存储器):
    USART1: PA9(TX), PA10(RX)
    需要将 BOOT0 拉高, 然后复位进入

或者使用 ST-Link / J-Link 烧录

用法:
    python flash_bootloader.py <COM端口> <bootloader.bin>
    python flash_bootloader.py COM3 stm32_bootloader.bin
"""

import serial
import sys
import time
import struct


# STM32 Bootloader 协议命令
CMD_INIT     = 0x7F
CMD_GET      = 0x00
CMD_GET_ID   = 0x02
CMD_WRITE    = 0x31
CMD_ERASE    = 0x43
CMD_GO       = 0x21
ACK          = 0x79
NACK         = 0x1F


def stm32_bootloader_connect(port, baudrate=115200):
    """连接到 STM32 内建 Bootloader"""
    # STM32 Bootloader 自动波特率检测
    ser = serial.Serial(port, baudrate, parity=serial.PARITY_EVEN, timeout=1)

    # 发送初始化命令
    ser.write(bytes([CMD_INIT]))
    response = ser.read(1)

    if response and response[0] == ACK:
        print("STM32 Bootloader connected ✓")
        return ser
    else:
        ser.close()
        raise Exception(f"Bootloader not responding (got: {response.hex() if response else 'nothing'})")


def stm32_get_version(ser):
    """读取 Bootloader 版本"""
    ser.write(bytes([CMD_GET]))
    ser.read(1)  # ACK
    count = ser.read(1)[0]
    version = ser.read(count)
    return version


def stm32_get_id(ser):
    """读取芯片 ID"""
    ser.write(bytes([CMD_GET_ID]))
    ser.read(1)
    count = ser.read(1)[0]
    pid = ser.read(count)
    return pid


def stm32_erase_all(ser):
    """擦除全部 Flash (可选, 谨慎使用)"""
    ser.write(bytes([CMD_ERASE]))
    ser.write(bytes([0xFF]))  # 全擦除
    response = ser.read(1)
    return response[0] == ACK


def stm32_write_memory(ser, addr, data):
    """写入内存"""
    cmd = bytes([CMD_WRITE])
    addr_bytes = struct.pack(">I", addr)  # 大端
    checksum = (addr_bytes[0] ^ addr_bytes[1] ^ addr_bytes[2] ^ addr_bytes[3])

    ser.write(cmd)
    ser.read(1)  # ACK

    ser.write(addr_bytes + bytes([checksum]))
    ser.read(1)  # ACK

    length = len(data) - 1
    ser.write(bytes([length]))
    ser.write(data)
    ser.write(bytes([sum(data) & 0xFF]))  # checksum

    return ser.read(1)[0] == ACK


def stm32_go(ser, addr):
    """跳转到指定地址执行"""
    ser.write(bytes([CMD_GO]))
    ser.read(1)  # ACK
    addr_bytes = struct.pack(">I", addr)
    checksum = addr_bytes[0] ^ addr_bytes[1] ^ addr_bytes[2] ^ addr_bytes[3]
    ser.write(addr_bytes + bytes([checksum]))
    return ser.read(1)[0] == ACK


def flash_bootloader(port, bin_file):
    """烧录 Bootloader 到 STM32F103"""

    with open(bin_file, "rb") as f:
        fw_data = f.read()

    print(f"Firmware size: {len(fw_data)} bytes")

    ser = stm32_bootloader_connect(port)

    try:
        ver = stm32_get_version(ser)
        print(f"Bootloader version: {ver.hex()}")

        pid = stm32_get_id(ser)
        print(f"Device ID: {pid.hex()}")

        boot_addr = 0x08000000

        # 每 256 字节写一次
        BLOCK_SIZE = 256
        total_blocks = (len(fw_data) + BLOCK_SIZE - 1) // BLOCK_SIZE

        for i in range(0, len(fw_data), BLOCK_SIZE):
            block = fw_data[i:i + BLOCK_SIZE]
            addr = boot_addr + i
            if stm32_write_memory(ser, addr, block):
                block_num = i // BLOCK_SIZE + 1
                if block_num % 10 == 0 or block_num == total_blocks:
                    print(f"  Writing... {block_num}/{total_blocks} blocks")
            else:
                raise Exception(f"Write failed at address 0x{addr:08X}")

        print(f"Flash completed! {total_blocks} blocks written.")

        # 跳转到 Bootloader
        stm32_go(ser, boot_addr)
        print("Bootloader started!")

    finally:
        ser.close()


def main():
    if len(sys.argv) < 3:
        print("用法: python flash_bootloader.py <COM端口> <bootloader.bin>")
        print("示例: python flash_bootloader.py COM3 stm32_bootloader.bin")
        print()
        print("注意:")
        print("  1. STM32 BOOT0 接高电平")
        print("  2. 按一下复位按钮")
        print("  3. 然后再运行此脚本")
        sys.exit(1)

    port = sys.argv[1]
    bin_file = sys.argv[2]

    flash_bootloader(port, bin_file)


if __name__ == "__main__":
    main()
