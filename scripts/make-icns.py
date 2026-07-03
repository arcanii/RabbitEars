# SPDX-License-Identifier: GPL-3.0-or-later
# Build mac/packaging/RabbitEars.icns from art/macos_icon.png — the macOS peer of
# scripts/make_ico.py (which builds the Windows packaging/app.ico).
#
# macOS app icons are the rounded "squircle" sitting on a TRANSPARENT canvas (the
# system draws its own shadow). The source art has an opaque light background, so
# we flood-fill the exterior from the four corners to transparency; the interior
# (the bunny) is enclosed by the dark squircle and is left untouched. If the source
# already has transparent corners, it's used as-is. Requires Pillow; assembles the
# .icns with the system `iconutil`.
#   python3 scripts/make-icns.py
import os
import subprocess
import tempfile

from PIL import Image, ImageDraw

SIZES = [16, 32, 128, 256, 512]  # each also emits an @2x (double-resolution) variant
SENTINEL = (255, 0, 255)         # magenta marker for the flood-filled exterior
FLOOD_THRESH = 190               # sum-of-channel tolerance: covers the light bg + its
                                 # soft shadow, stays well clear of the dark squircle
FILL_FRAC = float(os.environ.get("RE_ICON_FILL", "0.97"))  # squircle's share of the
                                 # tile — crop the transparent margin + scale to the edge


def make_transparent(im: Image.Image) -> Image.Image:
    """Return im (RGBA) with the exterior background knocked out to alpha 0."""
    im = im.convert("RGBA")
    w, h = im.size
    corners = [(1, 1), (w - 2, 1), (1, h - 2), (w - 2, h - 2)]
    if all(im.getpixel(c)[3] < 8 for c in corners):
        return im  # already transparent outside — nothing to do

    work = im.convert("RGB")
    for c in corners:
        ImageDraw.floodfill(work, c, SENTINEL, thresh=FLOOD_THRESH)
    # Keep the original RGB everywhere (avoids dark halos on downscale); only the
    # alpha of the flood-filled exterior goes to 0.
    alpha = Image.new("L", (w, h))
    alpha.putdata([0 if p == SENTINEL else 255 for p in work.getdata()])
    im.putalpha(alpha)
    return im


def fit_squircle(im: Image.Image) -> Image.Image:
    """Crop the transparent margin and scale the squircle to FILL_FRAC of a square tile."""
    box = im.getbbox()
    if not box:
        return im
    shape = im.crop(box)
    sw, sh = shape.size
    tile = 1024
    scale = (tile * FILL_FRAC) / max(sw, sh)
    shape = shape.resize((max(1, round(sw * scale)), max(1, round(sh * scale))), Image.LANCZOS)
    canvas = Image.new("RGBA", (tile, tile), (0, 0, 0, 0))
    px, py = shape.size
    canvas.paste(shape, ((tile - px) // 2, (tile - py) // 2))
    return canvas


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = os.path.join(root, "art", "macos_icon.png")
    out = os.path.join(root, "mac", "packaging", "RabbitEars.icns")

    im = Image.open(src).convert("RGBA")
    w, h = im.size
    if w != h:  # centre-pad non-square art on transparency
        side = max(w, h)
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.paste(im, ((side - w) // 2, (side - h) // 2), im)
        im = square

    im = make_transparent(im)
    im = fit_squircle(im)  # crop the transparent margin + scale the squircle to fill the tile

    preview = os.environ.get("RE_ICON_PREVIEW")
    if preview:  # composite over orange so transparency is obvious in a viewer
        bg = Image.new("RGBA", im.size, (255, 140, 0, 255))
        Image.alpha_composite(bg, im).convert("RGB").save(preview)
        print(f"wrote preview {preview}")

    with tempfile.TemporaryDirectory() as td:
        iconset = os.path.join(td, "RabbitEars.iconset")
        os.makedirs(iconset)
        for s in SIZES:
            im.resize((s, s), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{s}x{s}.png"))
            im.resize((s * 2, s * 2), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{s}x{s}@2x.png"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)

    print(f"wrote {out} from art/macos_icon.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
