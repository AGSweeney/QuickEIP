"""Draw eds_builder.ico: a data sheet on a blue tile, with the network link it describes.

Run it again after changing the drawing:  python make_icon.py
Needs Pillow. Each size is drawn on its own, at 8x and scaled down, so the
small ones stay sharp: under 48 pixels the "EDS" label is left off and the
page shows plain lines instead.
"""
import os

from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
BLUE_TOP = (37, 99, 235)
BLUE_BOTTOM = (23, 58, 160)
PAGE = (255, 255, 255)
FOLD = (191, 207, 240)
INK = (23, 58, 160)
LINE = (120, 144, 196)
NODE = (251, 191, 36)
SCALE = 8


def draw(size):
    n = size * SCALE
    u = n / 64.0  # the drawing is laid out on a 64 x 64 grid

    # Tile: a vertical gradient cut to a rounded square.
    tile = Image.new("RGBA", (n, n))
    px = ImageDraw.Draw(tile)
    for y in range(n):
        t = y / (n - 1)
        px.line([(0, y), (n, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(BLUE_TOP, BLUE_BOTTOM)) + (255,))
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, n - 1, n - 1], radius=round(13 * u), fill=255)
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    img.paste(tile, (0, 0), mask)
    d = ImageDraw.Draw(img)

    # Page with a folded corner.
    left, top, right, bottom, fold = 15 * u, 9 * u, 49 * u, 55 * u, 11 * u
    d.polygon([(left, top), (right - fold, top), (right, top + fold), (right, bottom), (left, bottom)], fill=PAGE)
    d.polygon([(right - fold, top), (right - fold, top + fold), (right, top + fold)], fill=FOLD)

    if size >= 48:
        font = ImageFont.truetype(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "segoeuib.ttf"), round(13.5 * u))
        box = d.textbbox((0, 0), "EDS", font=font)
        d.text(((left + right) / 2 - (box[0] + box[2]) / 2, 25 * u - (box[1] + box[3]) / 2), "EDS", font=font, fill=INK)
        rows = [34, 39]
    else:
        rows = [22, 29, 36]
    thick = max(1, round((2.2 if size >= 48 else 3.4) * u))
    for i, y in enumerate(rows):
        end = right - 5 * u if i < len(rows) - 1 else right - 14 * u
        d.line([(left + 5 * u, y * u), (end, y * u)], fill=LINE, width=thick)

    # Three nodes on a bus across the foot of the page: the device on its network.
    y = 47 * u
    d.line([(left + 6 * u, y), (right - 6 * u, y)], fill=INK, width=max(1, round(2.2 * u)))
    r = (3.4 if size >= 48 else 4.2) * u
    for x in (left + 6 * u, (left + right) / 2, right - 6 * u):
        d.ellipse([x - r, y - r, x + r, y + r], fill=NODE, outline=INK, width=max(1, round(1.3 * u)))

    return img.resize((size, size), Image.LANCZOS)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    images = [draw(s) for s in SIZES]
    # Pillow writes every image in append_images as its own entry, so each size is the one drawn for it.
    images[-1].save(os.path.join(here, "eds_builder.ico"), format="ICO", sizes=[(s, s) for s in SIZES],
                    append_images=images[:-1])
    sheet = Image.new("RGBA", (sum(SIZES) + 8 * (len(SIZES) + 1), 272), (236, 239, 243, 255))
    x = 8
    for im in images:
        sheet.paste(im, (x, 264 - im.height), im)
        x += im.width + 8
    sheet.save(os.path.join(here, "preview_sizes.png"))


if __name__ == "__main__":
    main()
