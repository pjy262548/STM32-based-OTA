#!/usr/bin/env python3
"""
固件版本信息生成工具
用法: python generate_version.py firmware.bin 1.0.1 [服务器URL]

会自动计算 .bin 文件的 size 和 CRC32,
生成/更新 version.json
"""

import os
import sys
import json
import zlib
import hashlib


def calc_crc32(filepath):
    """计算文件的 CRC32"""
    crc = 0
    with open(filepath, "rb") as f:
        while chunk := f.read(8192):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def calc_sha256(filepath):
    """计算文件的 SHA256"""
    sha = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(8192):
            sha.update(chunk)
    return sha.hexdigest()


def main():
    if len(sys.argv) < 2:
        print("用法: python generate_version.py <firmware.bin> [版本号] [服务器URL]")
        print("示例: python generate_version.py stm32_app_v1.0.1.bin 1.0.1 http://192.168.1.100:8080")
        sys.exit(1)

    filepath    = sys.argv[1]
    version     = sys.argv[2] if len(sys.argv) > 2 else "0.0.0"
    base_url    = sys.argv[3] if len(sys.argv) > 3 else "http://192.168.1.100:8080"
    filename    = os.path.basename(filepath)
    filesize    = os.path.getsize(filepath)
    crc32_val   = calc_crc32(filepath)
    sha256_val  = calc_sha256(filepath)

    version_entry = {
        "version": version,
        "filename": filename,
        "url": f"{base_url.rstrip('/')}/firmware/{filename}",
        "size": filesize,
        "crc32": f"{crc32_val:08X}",
        "sha256": sha256_val,
        "target_mcu": "STM32F103VET6",
        "changelog": "",
        "min_bootloader_version": 1,
    }

    # 读取现有 version.json (如果有的话)
    version_json_path = os.path.join(os.path.dirname(__file__),
                                     "..", "server", "firmware", "version.json")
    version_json_path = os.path.normpath(version_json_path)

    existing = {}
    if os.path.exists(version_json_path):
        with open(version_json_path, "r") as f:
            existing = json.load(f)

    # 保留 history
    history = existing.get("history", [])

    # 不需要重复添加同一版本
    if existing.get("latest", {}).get("version") == version:
        print(f"版本 {version} 已是最新, 跳过")
        return

    # 旧版本移至 history
    if "latest" in existing:
        history.insert(0, existing["latest"])

    # 只保留最近 10 条
    history = history[:10]

    output = {
        "latest": version_entry,
        "history": history,
    }

    with open(version_json_path, "w") as f:
        json.dump(output, f, indent=2, ensure_ascii=False)

    print("=" * 50)
    print("  固件版本信息已更新")
    print("=" * 50)
    print(f"  文件:     {filename}")
    print(f"  版本:     {version}")
    print(f"  大小:     {filesize:,} bytes ({filesize/1024:.1f} KB)")
    print(f"  CRC32:    {crc32_val:08X}")
    print(f"  SHA256:   {sha256_val}")
    print(f"  URL:      {version_entry['url']}")
    print(f"  输出:     {version_json_path}")
    print("=" * 50)


if __name__ == "__main__":
    main()
