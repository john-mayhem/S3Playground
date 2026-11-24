# S3 RGB web UI - MicroPython 1.19 on ESP32-S3 (WS2812 on GPIO48, "RGB" pads bridged).
# Joins home Wi-Fi from secrets.py; without it (or if joining fails) it opens its own AP "S3-RGB".
# Page = index.html; API: GET /api/state, POST /api/set {r,g,b,bri,mode,speed}.
import json
import math
import time

import machine
import neopixel
import network
import uasyncio as asyncio

PIN = 48
MODES = ("solid", "breathe", "rainbow", "off")
np = neopixel.NeoPixel(machine.Pin(PIN), 1)
state = {"r": 255, "g": 140, "b": 40, "bri": 25, "mode": "solid", "speed": 40}
dirty_at = 0


def load():
    try:
        with open("state.json") as f:
            update(json.load(f), mark=False)
    except (OSError, ValueError):
        pass


def update(d, mark=True):
    global dirty_at
    for k in ("r", "g", "b"):
        if k in d:
            state[k] = max(0, min(255, int(d[k])))
    for k in ("bri", "speed"):
        if k in d:
            state[k] = max(0, min(100, int(d[k])))
    if d.get("mode") in MODES:
        state["mode"] = d["mode"]
    if mark:
        dirty_at = time.ticks_ms()


GAMMA = [round(255 * (i / 255) ** 2.2) for i in range(256)]   # sRGB picker value -> linear LED duty
last = None


def show(r, g, b, k=1.0):
    global last
    k *= (state["bri"] / 100) ** 2          # squared: the slider feels linear to the eye
    px = (round(GAMMA[r] * k), round(GAMMA[g] * k), round(GAMMA[b] * k))
    if px != last:                          # only send when it changes: fewer chances for a Wi-Fi-garbled frame
        last = px
        np[0] = px
        np.write()


def wheel(h):
    h = (h % 1.0) * 6
    i, f = int(h), h - int(h)
    q, t = int(255 * (1 - f)), int(255 * f)
    return ((255, t, 0), (q, 255, 0), (0, 255, t), (0, q, 255), (t, 0, 255), (255, 0, q))[i]


async def effects():
    global dirty_at
    phase = 0.0
    while True:
        m = state["mode"]
        step = 0.0005 + state["speed"] / 100 * 0.02
        if m == "solid":
            show(state["r"], state["g"], state["b"])
        elif m == "breathe":
            phase += step * 2
            show(state["r"], state["g"], state["b"], 0.08 + 0.92 * (0.5 - 0.5 * math.cos(phase)))
        elif m == "rainbow":
            phase += step
            show(*wheel(phase))
        else:
            show(0, 0, 0)
        if dirty_at and time.ticks_diff(time.ticks_ms(), dirty_at) > 3000:   # save once sliders settle
            dirty_at = 0
            with open("state.json", "w") as f:
                json.dump(state, f)
        await asyncio.sleep_ms(20)


def wifi():
    try:
        import secrets
        ssid, pw = secrets.SSID, secrets.PASSWORD
    except (ImportError, AttributeError):
        ssid = None
    sta = network.WLAN(network.STA_IF)
    if ssid:
        sta.active(True)
        try:
            sta.config(dhcp_hostname="s3-rgb")
        except Exception:
            pass
        if not sta.isconnected():
            sta.connect(ssid, pw)
            for i in range(75):                 # ~15 s, blue blink while joining
                if sta.isconnected():
                    break
                np[0] = (0, 0, 8 if i % 2 else 0)
                np.write()
                time.sleep_ms(200)
        if sta.isconnected():
            ip = sta.ifconfig()[0]
            print("S3-RGB: http://%s/" % ip)
            return ip
        print("S3-RGB: could not join", ssid)
        sta.active(False)
    ap = network.WLAN(network.AP_IF)
    ap.active(True)
    ap.config(essid="S3-RGB", password="s3rgb1234", authmode=3)
    ip = ap.ifconfig()[0]
    print("S3-RGB: own Wi-Fi 'S3-RGB' (password s3rgb1234), http://%s/" % ip)
    return ip


async def reply(w, code, ctype, body):
    w.write("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n"
            % (code, ctype, len(body)))
    w.write(body)
    await w.drain()


async def handle(r, w):
    try:
        line = await r.readline()
        method, path = line.decode().split(" ")[:2]
        length = 0
        while True:
            h = await r.readline()
            if h in (b"\r\n", b""):
                break
            if h.lower().startswith(b"content-length:"):
                length = int(h[15:])
        body = b""
        while len(body) < min(length, 512):
            chunk = await r.read(length - len(body))
            if not chunk:
                break
            body += chunk
        if path == "/" and method == "GET":
            w.write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n")
            with open("index.html", "rb") as f:
                while True:
                    chunk = f.read(1024)
                    if not chunk:
                        break
                    w.write(chunk)
                    await w.drain()
        elif path == "/api/state":
            await reply(w, "200 OK", "application/json", json.dumps(state))
        elif path == "/api/set" and method == "POST":
            update(json.loads(body))
            await reply(w, "200 OK", "application/json", json.dumps(state))
        else:
            await reply(w, "404 Not Found", "text/plain", "not found")
    except Exception as e:
        print("http:", repr(e))
    finally:
        try:
            w.close()
            await w.wait_closed()
        except Exception:
            pass


async def run():
    load()
    wifi()
    asyncio.create_task(effects())
    await asyncio.start_server(handle, "0.0.0.0", 80)
    while True:
        await asyncio.sleep(3600)


asyncio.run(run())
