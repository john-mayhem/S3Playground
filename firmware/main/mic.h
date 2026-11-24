#pragma once
#include <stdbool.h>

// I2S MEMS microphone (MSM261S4030H0 / INMP441-style, L/R tied to GND = left slot).
// WS = GPIO4, SCK = GPIO5, SD = GPIO6.
void mic_start(void);

typedef struct {
    float rms_db;    // average level since the last read, dBFS
    float peak_db;   // loudest sample since the last read, dBFS
    bool alive;      // the data line is actually toggling (false = wiring / power problem)
    int rate;        // sample rate, Hz
} mic_level_t;

mic_level_t mic_read_level(void);   // resets the accumulators
