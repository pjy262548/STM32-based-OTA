#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include "config.h"
#include "ota_client.h"

#undef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "192.168.0.108"

#ifndef MQTT_TRIGGER_ENABLE
#define MQTT_TRIGGER_ENABLE 1
#endif

#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "192.168.0.108"
#endif

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif

#ifndef MQTT_OTA_TOPIC
#define MQTT_OTA_TOPIC "stm32/ota/check"
#endif

#ifndef MQTT_STATUS_TOPIC
#define MQTT_STATUS_TOPIC "stm32/ota/status"
#endif

#if MQTT_TRIGGER_ENABLE

static WiFiClient mqtt_net;
static PubSubClient mqtt_client(mqtt_net);
static volatile bool ota_check_requested = false;
static uint32_t next_mqtt_connect_ms = 0;

static bool mqtt_payload_requests_check(const uint8_t *payload, unsigned int len)
{
    if (len == 0) {
        return true;
    }

    char text[16];
    unsigned int copy_len = (len < sizeof(text) - 1) ? len : sizeof(text) - 1;
    for (unsigned int i = 0; i < copy_len; i++) {
        char c = (char)payload[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        text[i] = c;
    }
    text[copy_len] = '\0';

    return strcmp(text, "1") == 0 ||
           strcmp(text, "true") == 0 ||
           strcmp(text, "check") == 0 ||
           strcmp(text, "update") == 0 ||
           strcmp(text, "ota") == 0;
}

static void mqtt_publish_status(const char *status)
{
    if (mqtt_client.connected() && status) {
        mqtt_client.publish(MQTT_STATUS_TOPIC, status, true);
    }
}

static void mqtt_publish_status_detail(const char *status, const char *detail)
{
    char payload[192];

    if (!status) {
        return;
    }
    if (!detail || detail[0] == '\0') {
        mqtt_publish_status(status);
        return;
    }

    snprintf(payload, sizeof(payload), "%s:%s", status, detail);
    mqtt_publish_status(payload);
}

static void mqtt_callback(char *topic, uint8_t *payload, unsigned int len)
{
    if (!topic || strcmp(topic, MQTT_OTA_TOPIC) != 0) {
        return;
    }

    if (mqtt_payload_requests_check(payload, len)) {
        ota_check_requested = true;
        mqtt_publish_status("check_requested");
        DEBUG_PRINT("MQTT OTA check requested");
    } else {
        mqtt_publish_status("ignored_command");
        DEBUG_PRINT("MQTT OTA command ignored");
    }
}

static void mqtt_connect_if_needed(void)
{
    if (WiFi.status() != WL_CONNECTED || mqtt_client.connected()) {
        return;
    }

    uint32_t now = millis();
    if ((int32_t)(now - next_mqtt_connect_ms) < 0) {
        return;
    }
    next_mqtt_connect_ms = now + 5000UL;

    mqtt_client.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqtt_client.setCallback(mqtt_callback);

    String client_id = "esp32-ota-";
    client_id += WiFi.macAddress();
    client_id.replace(":", "");

    if (!mqtt_client.connect(client_id.c_str())) {
        DEBUG_PRINT("MQTT connect failed: host=%s port=%u state=%d",
                    MQTT_BROKER_HOST, MQTT_BROKER_PORT, mqtt_client.state());
        return;
    }

    mqtt_client.subscribe(MQTT_OTA_TOPIC);
    mqtt_publish_status("online");
    DEBUG_PRINT("MQTT connected, subscribed: %s", MQTT_OTA_TOPIC);
}

static void run_requested_ota_check(void)
{
    if (!ota_check_requested) {
        return;
    }

    ota_check_requested = false;
    if (WiFi.status() != WL_CONNECTED) {
        mqtt_publish_status("wifi_not_connected");
        DEBUG_PRINT("MQTT OTA check skipped: WiFi not connected");
        return;
    }

    if (ota_is_busy()) {
        mqtt_publish_status("ota_busy");
        DEBUG_PRINT("MQTT OTA check skipped: OTA busy");
        return;
    }

    mqtt_publish_status("checking");
    FirmwareInfo info;
    if (!ota_check_update(info)) {
        if (ota_get_state() == OTA_STATE_FAILED) {
            mqtt_publish_status_detail("check_failed", ota_last_error());
            DEBUG_PRINT("MQTT OTA check failed: %s", ota_last_error());
        } else {
            mqtt_publish_status("no_update");
            DEBUG_PRINT("MQTT OTA check complete: no update");
        }
        return;
    }

    mqtt_publish_status("update_available");
    DEBUG_PRINT("MQTT OTA update available, starting download");
    if (ota_download_and_flash(info)) {
        mqtt_publish_status("success");
        DEBUG_PRINT("MQTT OTA completed successfully");
    } else {
        mqtt_publish_status_detail("failed", ota_last_error());
        DEBUG_PRINT("MQTT OTA failed: %s", ota_last_error());
    }
}

static void mqtt_ota_task(void *arg)
{
    (void)arg;
    delay(5000);

    while (true) {
        mqtt_connect_if_needed();
        if (mqtt_client.connected()) {
            mqtt_client.loop();
        }
        run_requested_ota_check();
        delay(100);
    }
}

void initVariant(void)
{
    xTaskCreatePinnedToCore(mqtt_ota_task,
                            "mqtt_ota",
                            8192,
                            NULL,
                            1,
                            NULL,
                            1);
}

#endif
