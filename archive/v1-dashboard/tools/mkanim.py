#!/usr/bin/env python3
"""Convert a GIF (or a folder of frames, or a still image) into a .anim file
for the T-Dongle-C5 dashboard.

    pip3 install --user Pillow
    python3 tools/mkanim.py cat.gif                 # -> cat.anim
    python3 tools/mkanim.py cat.gif -o /Volumes/SD/anim/cat.anim
    python3 tools/mkanim.py frames/ --fps 20 --fill
    python3 tools/mkanim.py logo.png --hold 2000

Frames are scaled to fit 160x80 and letterboxed on black; --fill crops to fill
instead. Decoding happens here rather than on the dongle so playback is just a
DMA of raw RGB565 straight from the SD card to the panel.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from animfmt import write_anim, pack_frame  # noqa: E402

W, H = 160, 80
IMAGE_EXT = (".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp")

try:
    from PIL import Image, ImageSequence
except ImportError:
    sys.exit("Pillow is required:  pip3 install --user Pillow")


def fit(img, fill):
    img = img.convert("RGB")
    sw, sh = img.size
    scale = max(W / sw, H / sh) if fill else min(W / sw, H / sh)
    nw, nh = max(1, round(sw * scale)), max(1, round(sh * scale))
    img = img.resize((nw, nh), Image.LANCZOS)

    canvas = Image.new("RGB", (W, H), (0, 0, 0))
    canvas.paste(img, ((W - nw) // 2, (H - nh) // 2))
    if fill:                                    # crop overflow back to the panel
        canvas = Image.new("RGB", (W, H), (0, 0, 0))
        left, top = (nw - W) // 2, (nh - H) // 2
        canvas.paste(img.crop((left, top, left + W, top + H)), (0, 0))
    return canvas


def source_frames(path):
    """Yield (PIL image, native duration in ms)."""
    if os.path.isdir(path):
        names = sorted(n for n in os.listdir(path) if n.lower().endswith(IMAGE_EXT))
        if not names:
            sys.exit(f"no images in {path}")
        for n in names:
            yield Image.open(os.path.join(path, n)), None
        return

    img = Image.open(path)
    n = getattr(img, "n_frames", 1)
    if n > 1:
        for frame in ImageSequence.Iterator(img):
            yield frame, frame.info.get("duration")
    else:
        yield img, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="GIF, image, or directory of frames")
    ap.add_argument("-o", "--out", help="output .anim (default: alongside the source)")
    ap.add_argument("--fps", type=float, help="force a frame rate")
    ap.add_argument("--hold", type=int, help="ms per frame for stills (default 1000)")
    ap.add_argument("--fill", action="store_true", help="crop to fill instead of letterbox")
    ap.add_argument("--max-frames", type=int, default=240)
    args = ap.parse_args()

    out = args.out
    if not out:
        stem = os.path.basename(args.source.rstrip("/")) or "out"
        out = os.path.splitext(stem)[0] + ".anim"

    forced = int(round(1000 / args.fps)) if args.fps else None

    frames = []
    for img, native in source_frames(args.source):
        if len(frames) >= args.max_frames:
            print(f"stopping at --max-frames={args.max_frames}")
            break
        delay = forced or native or args.hold or 1000
        delay = max(20, min(delay, 65535))
        px = list(fit(img, args.fill).getdata())
        frames.append((delay, pack_frame(px, W, H)))

    if not frames:
        sys.exit("no frames produced")

    write_anim(out, W, H, frames, frames[0][0])
    print("copy it to the card's /anim/ folder, then pick the GIF screen on the dongle")


if __name__ == "__main__":
    main()
