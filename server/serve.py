#!/usr/bin/env python3
"""Development OTA firmware server for the ESP32 STM32-bridge OTA flow."""

import http.server
import json
import os
import shutil
import socket
import struct
import subprocess
import urllib.parse
import zlib

HOST = "0.0.0.0"
PORT = int(os.environ.get("OTA_SERVER_PORT", "8080"))
SERVER_ROOT = os.path.dirname(__file__)
FIRMWARE_DIR = os.path.join(SERVER_ROOT, "firmware")
TARGET_MCU = os.environ.get("STM32_TARGET_MCU", "STM32F103VET6")
DEFAULT_VERSION = os.environ.get("STM32_FW_VERSION", "1.0.1")
COMPAT_FIRMWARE = "stm32_app.bin"
APP_RUN_ADDR = 0x08004000
APP_PARTITION_SIZE = 0x00028000
SRAM_BASE = 0x20000000
SRAM_END = 0x20010000
MQTT_BROKER_HOST = os.environ.get("MQTT_BROKER_HOST", "192.168.0.108")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
MQTT_OTA_TOPIC = os.environ.get("MQTT_OTA_TOPIC", "stm32/ota/check")
MQTT_TRIGGER_MESSAGE = os.environ.get("MQTT_TRIGGER_MESSAGE", "check")


def get_local_ip():
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        sock.close()
        return ip
    except OSError:
        return "127.0.0.1"


def load_existing_version():
    manifest_path = os.path.join(FIRMWARE_DIR, "version.json")
    try:
        with open(manifest_path, "r", encoding="utf-8") as file:
            data = json.load(file)
        return str(data.get("latest", {}).get("version") or DEFAULT_VERSION)
    except (OSError, ValueError, TypeError):
        return DEFAULT_VERSION


def firmware_entry(filename, local_ip):
    path = os.path.join(FIRMWARE_DIR, filename)
    if not os.path.exists(path):
        return None
    with open(path, "rb") as file:
        payload = file.read()
    if not firmware_vector_valid(payload):
        return None
    return {
        "filename": filename,
        "url": f"http://{local_ip}:{PORT}/firmware/{filename}",
        "size": len(payload),
        "crc32": f"{zlib.crc32(payload) & 0xFFFFFFFF:08X}",
    }


def firmware_vector_valid(payload):
    if len(payload) < 8:
        print("[OTA] Firmware invalid: file too small for vector table")
        return False
    if len(payload) > APP_PARTITION_SIZE:
        print(f"[OTA] Firmware invalid: size={len(payload)} exceeds {APP_PARTITION_SIZE}")
        return False

    sp, reset_handler = struct.unpack_from("<II", payload, 0)
    reset_addr = reset_handler & ~1
    app_end = APP_RUN_ADDR + APP_PARTITION_SIZE

    if not (SRAM_BASE <= sp <= SRAM_END):
        print(f"[OTA] Firmware invalid: SP=0x{sp:08X} outside SRAM")
        return False
    if (reset_handler & 1) == 0:
        print(f"[OTA] Firmware invalid: Reset_Handler=0x{reset_handler:08X} is not Thumb")
        return False
    if not (APP_RUN_ADDR <= reset_addr < app_end):
        print(
            "[OTA] Firmware invalid: Reset_Handler="
            f"0x{reset_handler:08X}, expected linked for 0x{APP_RUN_ADDR:08X}-0x{app_end:08X}"
        )
        return False

    return True


def refresh_manifest(local_ip):
    os.makedirs(FIRMWARE_DIR, exist_ok=True)
    manifest_path = os.path.join(FIRMWARE_DIR, "version.json")

    default_entry = firmware_entry(COMPAT_FIRMWARE, local_ip)

    if default_entry is None:
        print(f"[OTA] Firmware missing, expected: {os.path.join(FIRMWARE_DIR, COMPAT_FIRMWARE)}")
        return None

    version = load_existing_version()
    latest = {
        "version": version,
        "filename": default_entry["filename"],
        "url": default_entry["url"],
        "size": default_entry["size"],
        "crc32": default_entry["crc32"],
        "target_mcu": TARGET_MCU,
        "min_bootloader_version": 1,
        "changelog": "Generated from local firmware image",
    }

    manifest = {"latest": latest, "history": []}
    with open(manifest_path, "w", encoding="utf-8") as file:
        json.dump(manifest, file, indent=2, ensure_ascii=False)
        file.write("\n")

    return manifest


def find_mosquitto_pub():
    exe = shutil.which("mosquitto_pub")
    if exe:
        return exe

    candidates = [
        r"C:\Program Files\Mosquitto\mosquitto_pub.exe",
        r"C:\Program Files (x86)\Mosquitto\mosquitto_pub.exe",
    ]
    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate
    return None


def publish_ota_trigger():
    exe = find_mosquitto_pub()
    if not exe:
        return False, "mosquitto_pub not found"

    cmd = [
        exe,
        "-h", MQTT_BROKER_HOST,
        "-p", str(MQTT_BROKER_PORT),
        "-t", MQTT_OTA_TOPIC,
        "-m", MQTT_TRIGGER_MESSAGE,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        return False, detail or f"mosquitto_pub exited {result.returncode}"
    return True, "published"


class CORSHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=SERVER_ROOT, **kwargs)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/ota/trigger":
            ok, message = publish_ota_trigger()
            body = json.dumps({
                "ok": ok,
                "message": message,
                "broker": MQTT_BROKER_HOST,
                "port": MQTT_BROKER_PORT,
                "topic": MQTT_OTA_TOPIC,
            }, ensure_ascii=False).encode("utf-8")

            self.send_response(200 if ok else 500)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if parsed.path == "/api/ota/refresh":
            manifest = refresh_manifest(get_local_ip())
            body = json.dumps({
                "ok": manifest is not None,
                "manifest": manifest,
            }, ensure_ascii=False).encode("utf-8")

            self.send_response(200 if manifest is not None else 500)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        super().do_GET()

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        super().end_headers()

    def log_message(self, fmt, *args):
        print(f"[OTA] {self.client_address[0]} - {fmt % args}")


def main():
    local_ip = get_local_ip()
    manifest = refresh_manifest(local_ip)

    print("=" * 50)
    print("  STM32 OTA Firmware Server")
    print("=" * 50)
    print(f"  Server root  : {SERVER_ROOT}")
    print(f"  Firmware dir : {FIRMWARE_DIR}")
    print(f"  Listening on : http://{local_ip}:{PORT}")
    print(f"  Version URL  : http://{local_ip}:{PORT}/firmware/version.json")
    if manifest:
        latest = manifest["latest"]
        print(f"  Default URL  : {latest['url']}")
        print(f"  Size / CRC   : {latest['size']} bytes / 0x{latest['crc32']}")
    print("=" * 50)
    print()

    server = http.server.HTTPServer((HOST, PORT), CORSHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")
        server.shutdown()


if __name__ == "__main__":
    main()
