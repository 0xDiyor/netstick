#!/usr/bin/env python3
"""Drive the UI over the console and turn framebuffer dumps into PNGs.

Needs a firmware built with FB_DUMP enabled (idf.py menuconfig -> netstick).
Commands sent to the dongle: t = tap, h = hold, d = dump frame, wN = wait N s.

    python3 tools/fbview.py --script "d,t,d,h,w2,d" --out /tmp/screens
    python3 tools/fbview.py --seconds 30 --out /tmp/screens      # just listen
    python3 tools/fbview.py --file capture.txt --out /tmp/screens

Run it with ESP-IDF's python (it has pyserial): `. ~/esp/esp-idf/export.sh`.
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

def render(buf, out, scale, prefix):
    text = buf.decode("utf-8", "replace")
    n = 0
    for m in re.finditer(r"<<<FB (\S+) (\d+) (\d+)\r?\n(.*?)\r?\n>>>FB", text, re.S):
        w, h, body = int(m.group(2)), int(m.group(3)), m.group(4)
        try:
            px = base64.b64decode(re.sub(r"\s", "", body))
        except Exception as e:
            print("decode failed:", e); continue
        if len(px) != w * h * 2:
            print(f"short frame ({len(px)} bytes)"); continue
        path = os.path.join(out, f"{prefix}{n:02d}.png")
        png(path, px, w, h, scale); n += 1
        print("wrote", path)
    if not n:
        print("no framebuffer dumps seen - was the firmware built with FB_DUMP?")
    return n

def open_port(port):
    import serial
    ports = [port] if port else sorted(glob.glob("/dev/cu.usbmodem*"))
    for p in ports:
        try:
            return serial.Serial(p, 115200, timeout=0.2)
        except Exception:
            pass
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--script", help="comma separated: t, h, d, wN")
    ap.add_argument("--file", help="parse a saved capture instead of reading serial")
    ap.add_argument("--save-raw")
    ap.add_argument("--out", default=".")
    ap.add_argument("--prefix", default="frame-")
    ap.add_argument("--scale", type=int, default=4)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.file:
        return render(open(a.file, "rb").read(), a.out, a.scale, a.prefix)

    ser = None
    deadline = time.time() + 10
    while ser is None and time.time() < deadline:
        ser = open_port(a.port)
        if ser is None: time.sleep(0.5)
    if ser is None:
        print("no serial port"); sys.exit(1)

    buf = b""
    def pump(seconds):
        nonlocal buf
        end = time.time() + seconds
        while time.time() < end:
            try:
                buf += ser.read(16384)
            except Exception:
                time.sleep(0.2)

    if a.script:
        for cmd in a.script.split(","):
            cmd = cmd.strip()
            if not cmd: continue
            if cmd[0] == "w":
                pump(float(cmd[1:] or 1))
            else:
                ser.write(cmd.encode()); ser.flush()
                # a dump is ~34 kB of base64; give it time to arrive
                pump(1.2 if cmd == "d" else 0.4)
        pump(1.0)
    else:
        pump(a.seconds)
    ser.close()
    if a.save_raw:
        open(a.save_raw, "wb").write(buf)
    # Show the log lines too - handy when something failed on the dongle.
    log = re.sub(r"<<<FB.*?>>>FB", "", buf.decode("utf-8", "replace"), flags=re.S)
    tail = [l for l in log.splitlines() if l.strip()][-25:]
    if tail:
        print("--- console tail ---"); print("\n".join(tail))
    render(buf, a.out, a.scale, a.prefix)

if __name__ == "__main__":
    main()
