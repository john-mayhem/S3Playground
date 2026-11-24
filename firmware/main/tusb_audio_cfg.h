// TinyUSB audio-class settings. esp_tinyusb's tusb_config.h has no UAC knobs, so main/CMakeLists.txt
// force-includes this header into TinyUSB, esp_tinyusb and main (it must be identical everywhere).
// Plain #defines only: nothing here may include other headers.
#pragma once

// "S3 Audio": UAC2 stereo speaker at USB full speed.
#define S3A_MAX_RATE      96000           // highest rate offered (44.1 / 48 / 88.2 / 96 kHz)
#define S3A_CHANNELS      2
// wMaxPacketSize at the top rate: (96 + 1 spare frame) * bytes * channels
#define S3A_EP_SIZE(bytes) ((S3A_MAX_RATE / 1000 + 1) * (bytes) * S3A_CHANNELS)

#define CFG_TUD_AUDIO                         1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT           1
#define CFG_TUD_AUDIO_ENABLE_EP_IN            0
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP      0
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP     0
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX    S3A_EP_SZ_MAX
#define S3A_EP_SZ_MAX                         S3A_EP_SIZE(3)                      // 24-bit packed = 582 B
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (8 * S3A_EP_SZ_MAX)                 // ~8 ms of slack
