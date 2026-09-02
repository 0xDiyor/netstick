#!/usr/bin/env python3
"""Generate a glitching fsociety-style mask animation as fsociety.anim.

Pure standard library - no Pillow needed. The mask is drawn from geometric
primitives at 160x80 (the T-Dongle-C5 panel in landscape), then a scanline +
slice-glitch + channel-split pass is applied per frame.

    python3 tools/fsociety.py            # -> fsociety.anim
    python3 tools/fsociety.py out.anim
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from animfmt import write_anim, pack_frame, load_font, draw_text  # noqa: E402

W, H = 160, 80
FRAMES = 48
DELAY_MS = 65

# Mask geometry, in pixels relative to the face centre.
CX, CY = 80.0, 30.0
A, B_TOP, B_BOT = 19.5, 26.0, 29.0

BG      = (0, 0, 0)
FACE    = (232, 220, 198)
DARK    = (16, 13, 11)
SHADOW  = (186, 173, 152)
OUTLINE = (60, 52, 44)
LIME    = (0, 255, 65)

random.seed(0xF50C)


def ellipse(dx, dy, ex, ey, rx, ry):
    if rx <= 0 or ry <= 0:
        return False
    u = (dx - ex) / rx
    v = (dy - ey) / ry
    return u * u + v * v <= 1.0


def in_face(dx, dy):
    b = B_TOP if dy < 0 else B_BOT
    t = dy / b
    if abs(t) > 1.0:
        return False
    a_eff = A * (1.0 - 0.26 * max(0.0, t) ** 1.9)   # taper toward the chin
    u = dx / a_eff
    return u * u + t * t <= 1.0


def in_brow(dx, dy):
    # A band between two concentric ellipses, split into a left and right arch.
    # Kept well above the eyes so the two never merge into one dark mass.
    inner = ellipse(dx, dy, 0, -3.0, 17.0, 12.6)
    outer = ellipse(dx, dy, 0, -3.0, 17.0, 15.8)
    return outer and not inner and dy < -12.6 and 3.0 <= abs(dx) <= 15.0


def in_eye(dx, dy):
    return ellipse(dx, dy, -8.6, -5.0, 5.6, 6.6) or ellipse(dx, dy, 8.6, -5.0, 5.6, 6.6)


def in_mustache(dx, dy):
    # Two wings joined by a thin bridge, so a dip stays visible under the nose,
    # plus the upturned curls at the outer ends.
    return (ellipse(dx, dy, -8.5, 9.5, 8.2, 3.9)
            or ellipse(dx, dy, 8.5, 9.5, 8.2, 3.9)
            or ellipse(dx, dy, 0.0, 8.4, 3.8, 2.0)
            or ellipse(dx, dy, -15.8, 5.8, 3.2, 3.6)
            or ellipse(dx, dy, 15.8, 5.8, 3.2, 3.6))


def in_goatee(dx, dy):
    # Sits clear of the mustache with face showing between the two.
    return ellipse(dx, dy, 0, 20.5, 4.0, 4.6) or ellipse(dx, dy, 0, 24.5, 2.3, 4.2)


def build_base():
    px = [BG] * (W * H)
    for y in range(H):
        dy = y - CY
        for x in range(W):
            dx = x - CX
            if not in_face(dx, dy):
                continue
            if in_eye(dx, dy) or in_brow(dx, dy) or in_mustache(dx, dy) \
                    or in_goatee(dx, dy):
                c = DARK
            else:
                c = FACE
            px[y * W + x] = c

    # 1px outline so the pale face reads against the black background.
    edged = list(px)
    for y in range(H):
        for x in range(W):
            if px[y * W + x] != BG:
                continue
            for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                if 0 <= ny < H and 0 <= nx < W and px[ny * W + nx] == FACE:
                    edged[y * W + x] = OUTLINE
                    break
    return edged


def scanline(px):
    for y in range(1, H, 2):
        base = y * W
        for x in range(W):
            r, g, b = px[base + x]
            px[base + x] = (r * 78 // 100, g * 78 // 100, b * 78 // 100)


def slice_glitch(px, strength):
    for _ in range(strength):
        y0 = random.randrange(0, H - 2)
        hh = random.randint(2, 9)
        shift = random.randint(-14, 14)
        if shift == 0:
            continue
        for y in range(y0, min(H, y0 + hh)):
            row = px[y * W:(y + 1) * W]
            px[y * W:(y + 1) * W] = row[-shift:] + row[:-shift]


def channel_split(px, offset):
    src = list(px)
    for y in range(H):
        for x in range(W):
            r = src[y * W + max(0, x - offset)][0]
            g = src[y * W + x][1]
            b = src[y * W + min(W - 1, x + offset)][2]
            px[y * W + x] = (r, g, b)


def noise(px, prob):
    n = int(W * H * prob)
    for _ in range(n):
        i = random.randrange(W * H)
        px[i] = LIME if random.random() < 0.35 else (200, 200, 200)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "fsociety.anim"
    font = load_font(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "..", "main", "font5x8.h"))
    base = build_base()

    frames = []
    for f in range(FRAMES):
        px = list(base)

        # --- wordmark types itself in during the first second ---------------
        letters = "fsociety"
        shown = len(letters) if f >= 16 else max(0, (f - 2) // 2)
        if shown:
            draw_text(px, W, H, 32, 63, letters[:shown], LIME, font, scale=2)
        if 6 <= f < 16 and f % 2 == 0 and shown < len(letters):
            draw_text(px, W, H, 32 + shown * 12, 63, "_", LIME, font, scale=2)

        # --- reveal, hold, then break apart ---------------------------------
        if f < 8:
            cutoff = int(H * (f + 1) / 8.0)
            for y in range(cutoff, H):
                for x in range(W):
                    px[y * W + x] = BG
            noise(px, 0.10)
            slice_glitch(px, 3)
        elif f >= FRAMES - 4:
            slice_glitch(px, 5)
            channel_split(px, 3)
            noise(px, 0.05)
        elif f % 11 in (0, 1):
            slice_glitch(px, 2)
            channel_split(px, 2)
        elif f % 17 == 5:
            noise(px, 0.012)

        scanline(px)
        frames.append((DELAY_MS, pack_frame(px, W, H)))

    write_anim(out, W, H, frames, DELAY_MS)


if __name__ == "__main__":
    main()
