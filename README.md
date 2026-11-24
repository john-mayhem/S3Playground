# S3 Playground

Experiments on a cheap ESP32-S3 dev board, working up to a USB audio interface built on TinyUSB.

**Board:** ESP32-S3-DevKitC-1 style, N16R8 (16 MB flash, 8 MB octal PSRAM), a copy of the YD-ESP32-S3 layout:
- **COM** USB-C → CH343 USB-serial → UART0: flashing and logs.
- **USB** USB-C → the S3's native USB: whatever the firmware makes it (TinyUSB).
- WS2812 RGB LED on **GPIO48**. The "RGB" pads must be bridged first.

## firmware/ (ESP-IDF 5.5)

What v0.1 does:
- **USB:** one composite device "S3 Playground" (VID:PID 303A:4010):
  - a CDC serial port with a tiny shell (`help`, `info`, `led r g b`)
  - an HID consumer-control interface: the **BOOT** button sends Play/Pause to the PC
  - **"S3 Audio"**, a USB Audio Class 2.0 speaker (Windows' built-in driver, no install). It has no DAC: it shows what the PC sends.
    - Formats: stereo, 16 or 24-bit, at 44.1 / 48 / 88.2 / 96 kHz.
    - The status page shows the rate the PC chose and the rate it actually delivers, PC volume and mute, L/R levels, how many bits carry signal (padded 16-bit shows up), clipping, lost packets (counted against USB frame numbers) and a log of everything the PC does with the stream. `/api/audio` serves the same as JSON.
    - The LED turns into a VU meter while music plays.
    - 96 kHz/24-bit needs a patched TinyUSB in `firmware/components/tinyusb` (the S3's 1 KB USB FIFO is too small for the stock RX sizing); `PATCHES.md` there explains it.
    - `firmware/tools/audio_test.py` plays a known tone in every format (WASAPI exclusive) and compares it with what the board measured. Last run: all 6 formats exact, 0 lost packets.
- **Microphone:** I2S MEMS mic (MSM261S4030H0) at 48 kHz. Wiring: V→3V3, G and LR→GND, WS→GPIO4, CK→GPIO5, DA→GPIO6. The status page shows a live level meter (RMS + held peak); `/api/mic` serves the levels as JSON.
- **Wi-Fi:** reachable at `s3-playground.local`. The status page at `/` shows USB state, uptime, memory and the running slot, and `/api/info` serves the same as JSON.
- **OTA:** two 4 MB app slots, upload with `POST /ota`. A new image must reach Wi-Fi within 60 s or the bootloader rolls back to the previous one. Images from other projects are refused.
- **Status LED:**
  - blue blink: joining Wi-Fi
  - dim green: OK
  - red blink: Wi-Fi failed (USB still works)
  - purple blink: OTA in progress

### Setup
1. Copy `firmware/main/secrets.h.example` to `firmware/main/secrets.h`, then set your Wi-Fi details and a random `OTA_TOKEN`. The file is gitignored.
2. Build: `firmware\tools\idf.ps1 build`. The script loads ESP-IDF 5.5.4 from `C:\Espressif\frameworks\esp-idf-v5.5.4`; edit the path if yours differs.
3. First flash, by cable on the **COM** port: `firmware\tools\idf.ps1 -p COM9 flash monitor`
4. Every later update goes over Wi-Fi: `firmware\tools\idf.ps1 build; firmware\tools\ota.ps1`

## web-rgb/ (MicroPython)

A phone-friendly page for the RGB LED: colour picker, presets, Solid, Breathe and Rainbow modes, brightness. Runs on the factory MicroPython 1.19.1. Upload the files with `mpremote`; Wi-Fi details go in a gitignored `secrets.py`. Colours are gamma-corrected so they match the screen.

`factory-backup/` holds the board's original MicroPython demo.

## Roadmap
1. ✅ Hello USB: CDC + HID, OTA over Wi-Fi; I2S mic level meter
2. ✅ "S3 Audio": a UAC2 sound card up to 96 kHz/24-bit. Analyses what the PC sends (rate, format, volume, mute, levels); the LED reacts to the music.
3. Ideas: advertise AC-3/DTS (IEC 61937) or 5.1 and see what Windows sends; the mic as a USB microphone too.
4. ES8311 codec for real headphone or speaker output, then the full USB audio interface board.

**Status (2026-11-24):** everything above tested and working. The board is stashed for later, with the mic desoldered.
