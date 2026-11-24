#pragma once
#include <stdbool.h>
#include <stdint.h>

void usb_start(void);

typedef struct {
    bool mounted;        // the PC configured us
    bool suspended;      // PC asleep / port suspended
    bool cdc_open;       // a terminal has the serial port open (DTR)
    uint32_t key_presses;
} usb_status_t;

usb_status_t usb_status(void);
