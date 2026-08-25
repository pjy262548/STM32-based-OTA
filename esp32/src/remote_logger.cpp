#include "remote_logger.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <stdarg.h>

static WiFiUDP log_udp;
static IPAddress log_host;
static uint16_t log_port = 4210;
static uint16_t log_local_port = 4211;
static bool log_started = false;

static bool remote_log_ensure_started(void)
{
    if (log_started) {
        return true;
    }

    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    log_started = (log_udp.begin(log_local_port) == 1);
    return log_started;
}

void remote_log_begin(const char *host, uint16_t port, uint16_t local_port)
{
    log_port = port;
    log_local_port = local_port;

    if (!host || !log_host.fromString(host)) {
        log_host = IPAddress(255, 255, 255, 255);
    }

    remote_log_ensure_started();
}

void remote_log_write(const char *data, size_t len)
{
    if (!data || len == 0 || WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!remote_log_ensure_started()) {
        return;
    }

    log_udp.beginPacket(log_host, log_port);
    log_udp.write((const uint8_t *)data, len);
    log_udp.endPacket();
}

void remote_log_printf(const char *fmt, ...)
{
    if (!fmt) {
        return;
    }

    char buffer[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len <= 0) {
        return;
    }

    if (len >= (int)sizeof(buffer)) {
        len = sizeof(buffer) - 1;
        buffer[len] = '\0';
    }

    remote_log_write(buffer, (size_t)len);
}
