#pragma once
#include <Arduino.h>

namespace WebUI {
    void init(uint16_t port = 80);

    void handle();

    // Publish outside device callbacks; live uses the chart-only connection.
    void push_event(const char *event, const char *json);
    void push_event(const char *event, const String &json);

    void push_status_event();
}
