#pragma once
// "S3 Audio": a UAC2 stereo speaker that analyses what the PC sends instead of playing it (no DAC yet).
#include <stdbool.h>
#include <stdint.h>

#include "tusb.h"

// Interface numbers and endpoint are given by usb.c, which owns the whole configuration descriptor.
#define S3A_ENTITY_CLOCK   0x04
#define S3A_ENTITY_IT_USB  0x01   // input terminal: audio from the USB host
#define S3A_ENTITY_FU      0x02   // feature unit: mute + volume (master)
#define S3A_ENTITY_OT_SPK  0x03   // output terminal: "desktop speaker"

#define S3A_CHANNEL_CFG (AUDIO20_CHANNEL_CONFIG_FRONT_LEFT | AUDIO20_CHANNEL_CONFIG_FRONT_RIGHT)

#define S3A_AS_ALT_LEN (TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN \
                        + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)
#define S3A_AC_BODY_LEN (TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN \
                         + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2) + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN)
#define S3A_DESC_LEN (TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN + TUD_AUDIO20_DESC_CS_AC_LEN + S3A_AC_BODY_LEN \
                      + TUD_AUDIO20_DESC_STD_AS_LEN + 2 * S3A_AS_ALT_LEN)

// One streaming alternate setting: _bytes per sample (2 = 16-bit, 3 = 24-bit packed)
#define S3A_AS_ALT(_itf, _alt, _bytes, _bits, _ep, _stridx) \
    TUD_AUDIO20_DESC_STD_AS_INT(_itf, _alt, 0x01, _stridx), \
    TUD_AUDIO20_DESC_CS_AS_INT(S3A_ENTITY_IT_USB, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I, AUDIO20_DATA_FORMAT_TYPE_I_PCM, \
                               S3A_CHANNELS, S3A_CHANNEL_CFG, 0x00), \
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(_bytes, _bits), \
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(_ep, (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA), \
                                   S3A_EP_SIZE(_bytes), 0x01), \
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE, \
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001)

#define S3A_DESCRIPTOR(_itf_ctrl, _itf_stream, _ep_out, _stridx) \
    TUD_AUDIO20_DESC_IAD(_itf_ctrl, 2, 0x00), \
    TUD_AUDIO20_DESC_STD_AC(_itf_ctrl, 0x00, _stridx), \
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_DESKTOP_SPEAKER, S3A_AC_BODY_LEN, 0x00), \
    /* internal programmable clock; sample rate R/W, validity R */ \
    TUD_AUDIO20_DESC_CLK_SRC(S3A_ENTITY_CLOCK, 3, 7, 0x00, 0x00), \
    TUD_AUDIO20_DESC_INPUT_TERM(S3A_ENTITY_IT_USB, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, S3A_ENTITY_CLOCK, S3A_CHANNELS, \
                                S3A_CHANNEL_CFG, 0x00, 0x0000, 0x00), \
    TUD_AUDIO20_DESC_FEATURE_UNIT(S3A_ENTITY_FU, S3A_ENTITY_IT_USB, 0x00, \
                                  (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), \
                                  0, 0), \
    TUD_AUDIO20_DESC_OUTPUT_TERM(S3A_ENTITY_OT_SPK, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, 0x00, S3A_ENTITY_FU, S3A_ENTITY_CLOCK, \
                                 0x0000, 0x00), \
    TUD_AUDIO20_DESC_STD_AS_INT(_itf_stream, 0x00, 0x00, _stridx), \
    S3A_AS_ALT(_itf_stream, 0x01, 2, 16, _ep_out, _stridx), \
    S3A_AS_ALT(_itf_stream, 0x02, 3, 24, _ep_out, _stridx)

void audio_start(uint8_t itf_stream);

typedef struct {
    bool streaming;          // the PC has the stream open (alt setting != 0)
    uint32_t rate_set;       // sample rate the PC asked for
    float rate_measured;     // what actually arrived, frames per second
    uint8_t bits;            // 16 or 24 (format of the open stream)
    uint8_t bits_used;       // how many of those bits carry signal (24-bit container with 16-bit content -> 16)
    bool muted;
    float volume_db;         // the PC's volume slider, as dB
    uint32_t packets;        // packets since last read
    uint16_t pkt_min, pkt_max;   // frames per packet since last read
    uint32_t dropped;        // total missing packets (gaps) since boot
    uint32_t opens;          // how many times the PC opened the stream
    float rms_db[2], peak_db[2];   // L, R, dBFS since last read
    uint32_t clipped;        // full-scale samples since last read
    bool mono;               // L == R for every sample since last read
    float window_s;          // seconds covered by this read
} audio_stats_t;

audio_stats_t audio_read_stats(void);   // resets the per-read counters

// Recent things the PC did ("set 96000 Hz", "opened 24-bit", "volume -12.5 dB"...), newest last.
// Writes up to max lines of "<uptime_ms> <text>" separated by '\n'; returns length.
int audio_events(char *out, int max);
