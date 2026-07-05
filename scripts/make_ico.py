# SPDX-License-Identifier: GPL-3.0-or-later
# Regenerate packaging/app.ico from art/clockwork_icon.png as a multi-resolution
# Windows .ico (16/24/32/48/64/128/256). Requires Pillow.
#   python scripts/make_ico.py
import os
import sys

from PIL import Image

SIZES = [16, 24, 32, 48, 64, 128, 256]


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = os.path.join(root, "art", "clockwork_icon.png")
    dst = os.path.join(root, "packaging", "app.ico")

    img = Image.open(src).convert("RGBA")
    # Pad to a centered square on transparency so non-square art isn't distorted.
    w, h = img.size
    if w != h:
        side = max(w, h)
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.paste(img, ((side - w) // 2, (side - h) // 2), img)
        img = square

    img.save(dst, format="ICO", sizes=[(s, s) for s in SIZES])
    print(f"wrote {dst} from {os.path.basename(src)} ({', '.join(map(str, SIZES))})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
