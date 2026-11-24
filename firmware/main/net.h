#pragma once
#include <stdbool.h>

#define HOSTNAME "s3-playground"

void net_start(void);                 // Wi-Fi STA + mDNS + HTTP server (status page, /api/info, /ota)
bool net_wait_ip(int timeout_ms);     // true once we have an IP
