#!/usr/bin/env python3
"""Capture FB_DUMP framebuffer dumps off the dongle's console and write PNGs.

    FB_DUMP=1 idf.py build flash
    python3 tools/fbview.py --seconds 30 --out /tmp/screens
"""
import argparse, base64, glob, os, re, struct, sys, time, zlib

def png(path, px, w, h, scale=4):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            v = (px[(y * w + x) * 2] << 8) | px[(y * w + x) * 2 + 1]
            row += bytes((((v >> 11) & 0x1F) * 255 // 31,
                          ((v >> 5) & 0x3F) * 255 // 63,
                          (v & 0x1F) * 255 // 31)) * scale
        for _ in range(scale):
            rows.append(bytes(row))
    raw = b"".join(b"\x00" + r for r in rows)
    ch = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + ch(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
        + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port"); ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--file", help="parse a previously captured log instead of reading serial")
    ap.add_argument("--save-raw", help="also write the raw capture here")
    ap.add_argument("--out", default="."); ap.add_argument("--scale", type=int, default=4)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.file:
        buf = open(a.file, "rb").read()
        return render(buf, a)
    import serial
    end = time.time() + a.seconds
    buf = b""
    ser = None
    last_data = time.time()
    # The dongle re-enumerates on reset, so the port can vanish mid-capture.
    # Reopen instead of dying, and tolerate it not being there yet.
    while time.time() < end:
        if ser is None:
            ports = [a.port] if a.port else sorted(glob.glob("/dev/cu.usbmodem*"))
            if not ports or not os.path.exists(ports[0]):
                time.sleep(0.5)
                continue
            try:
                ser = serial.Serial(ports[0], 115200, timeout=1)
                last_data = time.time()
            except Exception:
                time.sleep(0.5)
                continue
        try:
            got = ser.read(8192)
            buf += got
            # A reset can leave a file descriptor that is open but permanently
            # silent; reopen if nothing arrives for a while.
            if got:
                last_data = time.time()
            elif time.time() - last_data > 8:
                raise OSError("stale port")
        except Exception:
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(0.5)
    if ser:
        ser.close()
    print(f"captured {len(buf)} bytes")
    if a.save_raw:
        open(a.save_raw, "wb").write(buf)
    return render(buf, a)


def render(buf, a):
    text = buf.decode("utf-8", "replace")
    n = 0
    # The ESP console emits CRLF, so allow an optional \r at every break.
    pat = r"<<<FB (\S+) (\d+) (\d+)\r?\n(.*?)\r?\n>>>FB"
    for m in re.finditer(pat, text, re.S):
        tag, w, h, body = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        try:
            px = base64.b64decode(re.sub(r"\s", "", body))
        except Exception as e:
            print("decode failed:", e); continue
        if len(px) != w * h * 2:
            print(f"{tag}: short frame ({len(px)} bytes)"); continue
        out = os.path.join(a.out, f"{tag}-{n:02d}.png")
        png(out, px, w, h, a.scale); n += 1
        print("wrote", out)
    if not n:
        print("no framebuffer dumps seen - was the firmware built with FB_DUMP=1?")

if __name__ == "__main__":
    main()
