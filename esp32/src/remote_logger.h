#ifndef __REMOTE_LOGGER_H
#define __REMOTE_LOGGER_H

#include <Arduino.h>
#include <IPAddress.h>

void remote_log_begin(const char *host, uint16_t port, uint16_t local_port);
void remote_log_write(const char *data, size_t len);
void remote_log_printf(const char *fmt, ...);

inline void remote_log_print(void)
{
}

inline void remote_log_println(void)
{
    remote_log_write("\n", 1);
}

inline void remote_log_print(const IPAddress &value)
{
    String text = value.toString();
    remote_log_write(text.c_str(), text.length());
}

template <typename T>
inline void remote_log_print(const T &value)
{
    String text(value);
    remote_log_write(text.c_str(), text.length());
}

template <typename T>
inline void remote_log_println(const T &value)
{
    remote_log_print(value);
    remote_log_write("\n", 1);
}

#endif
