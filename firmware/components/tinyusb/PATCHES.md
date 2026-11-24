# Local TinyUSB copy

Copied from the managed `espressif/tinyusb` 0.21.0~6 (commit ab2826e0) and trimmed to what the ESP32-S3 build
compiles (`src/` with only `portable/synopsys/dwc2`, `lib/networking`). `main/idf_component.yml` points the
`espressif/tinyusb` dependency here with `override_path`.

## Patches
1. `src/portable/synopsys/dwc2/dcd_dwc2.c`, `calc_device_grxfsiz()`: the RX FIFO reserves room for **one** largest
   OUT packet instead of two. The S3's USB controller has 256 words (1 KB) of FIFO RAM shared by RX and every
   IN endpoint, and the stock 2-packet rule doesn't leave room for a 96 kHz / 24-bit stereo isochronous endpoint
   (582-byte packets) next to CDC + HID. The trade-off is less slack if the RX interrupt runs late; the firmware
   counts missing/short packets so the effect is visible.

To update TinyUSB: copy the new version over this folder the same way and re-apply the patch.
