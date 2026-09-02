"""Shared writer for the .anim container the firmware plays.

Layout (see main/anim.h):
    0   magic "ANM1"
    4   u16 width   (LE)
    6   u16 height  (LE)
    8   u16 frame_count
    10  u16 default_delay_ms
    12  u32 flags (reserved)
    16  frames: [u16 delay_ms][u16 reserved][w*h*2 bytes RGB565 BIG-endian]

Pixels are big-endian so the ESP32 can DMA a frame straight to the ST7735
without touching it.
"""
import struct

MAGIC = b"ANM1"


def rgb565_be(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes(((v >> 8) & 0xFF, v & 0xFF))


def pack_frame(pixels, w, h):
    """pixels: flat list of (r,g,b) tuples, row-major, length w*h."""
    out = bytearray(w * h * 2)
    i = 0
    for (r, g, b) in pixels:
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[i] = (v >> 8) & 0xFF
        out[i + 1] = v & 0xFF
        i += 2
    return bytes(out)


def write_anim(path, w, h, frames, default_delay=60):
    """frames: list of (delay_ms, packed_bytes)."""
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<HHHHI", w, h, len(frames), default_delay, 0))
        for delay, data in frames:
            f.write(struct.pack("<HH", delay, 0))
            f.write(data)
    total = 16 + len(frames) * (4 + w * h * 2)
    print(f"wrote {path}: {w}x{h}, {len(frames)} frames, {total/1024:.0f} KB")


def load_font(header_path):
    """Parse main/font5x8.h into {char: [5 column bytes]} so both the firmware
    and these tools draw text with the same glyphs."""
    import re
    src = open(header_path).read()
    body = src[src.index("font5x8[] = {"):]
    vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    font = {}
    for i in range(0, len(vals) - 4, 5):
        font[chr(0x20 + i // 5)] = vals[i:i + 5]
    return font


def draw_text(px, w, h, x, y, text, colour, font, scale=1):
    """Blit text into a flat pixel list. Mutates px."""
    for ch in text:
        glyph = font.get(ch, font.get("?", [0] * 5))
        for col in range(5):
            bits = glyph[col]
            for row in range(8):
                if not (bits >> row) & 1:
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        px_x = x + col * scale + sx
                        px_y = y + row * scale + sy
                        if 0 <= px_x < w and 0 <= px_y < h:
                            px[px_y * w + px_x] = colour
        x += 6 * scale
    return x
