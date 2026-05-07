#pragma once

#include <stdbool.h>

bool wifi_link_connect(const char *ssid, const char *password, unsigned int timeout_ms);
