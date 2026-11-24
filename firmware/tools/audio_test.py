"""Play a known test signal into "S3 Audio" at every format (WASAPI exclusive mode forces the exact rate and
bit depth) and compare it with what the board measured (GET /api/audio).

Signal: left = 440 Hz sine at -6 dBFS, right = 1 kHz sine at -12 dBFS. The device has no DAC, so nothing is heard.

    .venv\\Scripts\\python.exe firmware\\tools\\audio_test.py [--host s3-playground.local] [--seconds 8]
Needs: sounddevice, numpy (uv pip install sounddevice numpy).
"""
import argparse
import json
import threading
import time
import urllib.request

import numpy as np
import sounddevice as sd

FORMATS = [(44100, 16), (48000, 16), (48000, 24), (88200, 24), (96000, 16), (96000, 24)]


def find_device():
    for i, d in enumerate(sd.query_devices()):
        if "S3 Audio" in d["name"] and sd.query_hostapis(d["hostapi"])["name"] == "Windows WASAPI":
            return i
    raise SystemExit("S3 Audio (WASAPI) not found - is the USB port plugged in?")


def signal(rate, seconds):
    t = np.arange(int(rate * seconds)) / rate
    left = 10 ** (-6 / 20) * np.sin(2 * np.pi * 440 * t)
    right = 10 ** (-12 / 20) * np.sin(2 * np.pi * 1000 * t)
    return np.stack([left, right], axis=1)


def play(dev, rate, bits, seconds):
    x = signal(rate, seconds)
    ws = sd.WasapiSettings(exclusive=True)
    if bits == 16:
        data = np.round(x * 32767).astype("<i2")
        with sd.RawOutputStream(rate, device=dev, channels=2, dtype="int16", extra_settings=ws) as s:
            s.write(data.tobytes())
    else:
        i32 = np.round(x * 8388607).astype("<i4")
        packed = i32.view(np.uint8).reshape(-1, 4)[:, :3].tobytes()   # 24-bit little-endian, 3 bytes per sample
        with sd.RawOutputStream(rate, device=dev, channels=2, dtype="int24", extra_settings=ws) as s:
            s.write(packed)


def poll(host, stop, out):
    while not stop.is_set():
        try:
            with urllib.request.urlopen(f"http://{host}/api/audio", timeout=2) as r:
                out.append(json.load(r))
        except OSError:
            pass
        time.sleep(0.25)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="s3-playground.local")
    ap.add_argument("--seconds", type=float, default=8)
    a = ap.parse_args()
    dev = find_device()
    print(f"{'format':>14} | {'board saw':>14} | {'measured Hz':>11} | {'frames/pkt':>10} | {'L / R peak dBFS':>15} | {'bits':>7} | lost")
    for rate, bits in FORMATS:
        samples, stop = [], threading.Event()
        lost0 = None
        try:
            with urllib.request.urlopen(f"http://{a.host}/api/audio", timeout=2) as r:
                lost0 = json.load(r)["dropped"]
        except OSError:
            pass
        th = threading.Thread(target=poll, args=(a.host, stop, samples), daemon=True)
        th.start()
        err = None
        try:
            play(dev, rate, bits, a.seconds)
        except Exception as e:      # format refused by Windows/driver
            err = str(e).splitlines()[0]
        stop.set()
        th.join()
        live = [s for s in samples if s["streaming"] and s["packets"]][2:]   # skip the start-up reads
        if err or not live:
            print(f"{rate:>8} {bits}-bit | {'FAILED: ' + (err or 'no packets seen')}")
            continue
        frames = sum(s["rate_measured"] * s["window_s"] for s in live)
        secs = sum(s["window_s"] for s in live)
        last = live[-1]
        pmin = min(s["pkt_min"] for s in live)
        pmax = max(s["pkt_max"] for s in live)
        lp = max(s["peak_db"][0] for s in live)
        rp = max(s["peak_db"][1] for s in live)
        bu = max(s["bits_used"] for s in live)
        lost = last["dropped"] - (lost0 or 0)
        saw = f"{last['rate_set']} {last['bits']}-bit"
        print(f"{rate:>8} {bits}-bit | {saw:>14} | {frames / secs:>11.1f} | {str(pmin) + '-' + str(pmax):>10} | "
              f"{lp:>6.1f} / {rp:>6.1f} | {str(bu) + '/' + str(last['bits']):>7} | {lost}")
        time.sleep(1)


if __name__ == "__main__":
    main()
