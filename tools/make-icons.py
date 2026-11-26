#!/usr/bin/env python3
"""Generate every moLink bitmap icon from one set of drawing primitives.

The design lives on the same 32-unit grid as ``frontend/src/components/Logo.tsx``:

    rounded square  x=1 y=1 w=30 h=30 rx=10
    gradient        #0EA5A4 -> #3B82F6, projected from (4,3) to (28,30)
    highlight       10% white radial glow centred at (6,5), r=24
    inner stroke    18% white, centreline inset 1 unit
    arch            m at x=8.4/17/25.6, spring y=13.1, baseline y=22, stroke 2.8

16px and 24px are drawn with a small-size variant: stroke 3.0, no inner
stroke, no glow, and a flat #17B8B4 fill.

Everything is rasterised with Pillow only (no SVG renderer, no network).
Images are drawn oversampled and box-filtered down so small sizes stay crisp.

``icon.icns`` is *not* rewritten: Pillow cannot write ICNS and this machine has
no ``png2icns``/``icnsutil``.  The old file is left in place and reported here.
"""

from __future__ import annotations

import io
import math
import shutil
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
ICON_DIR = ROOT / "shell" / "assets" / "icons"
COMPARE_PATH = ROOT / "docs" / "icon-compare.png"

# ---------------------------------------------------------------------------
# Design constants, in 32-unit viewBox coordinates.
# ---------------------------------------------------------------------------
VIEW = 32.0
TILE = (1.0, 1.0, 30.0, 30.0)  # x, y, width, height
RADIUS = 10.0

GRADIENT_FROM = "#15B8AC"
GRADIENT_TO = "#2E9CEB"
GRADIENT_P0 = (4.0, 3.0)
GRADIENT_P1 = (28.0, 30.0)

GLOW_CENTER = (6.0, 5.0)
GLOW_RADIUS = 24.0
GLOW_ALPHA = 0.10

INNER_STROKE_ALPHA = 0.20
INNER_STROKE_INSET = 1.0
INNER_STROKE_WIDTH = 1.0

STEM_X = (8.4, 17.0, 25.6)
ARCH_RADIUS = 4.3
SPRING_Y = 13.1
BASELINE_Y = 22.0
STROKE = 2.8

SMALL_SIZES = (16, 24)
SMALL_STROKE = 3.0
# Keep the small-size fill in the same hue family as the full gradient instead of
# hand-picking a colour that drifts from it.
SMALL_FLAT = "#%02X%02X%02X" % tuple(
    round(
        int(GRADIENT_FROM[i:i + 2], 16)
        + (int(GRADIENT_TO[i:i + 2], 16) - int(GRADIENT_FROM[i:i + 2], 16)) * 0.55
    )
    for i in (1, 3, 5)
)

# The previous mark, reconstructed for docs/icon-compare.png.
OLD_GRADIENT_FROM = "#2dd4bf"
OLD_GRADIENT_TO = "#38bdf8"
OLD_RADIUS = 9.0
OLD_STROKE = 2.6
OLD_BASELINE_Y = 23.0

PNG_SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)


# ---------------------------------------------------------------------------
# Colour / gradient helpers
# ---------------------------------------------------------------------------
def _rgb(hex_colour: str) -> tuple[int, int, int]:
    value = hex_colour.lstrip("#")
    return tuple(int(value[i : i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]


_gradient_cache: dict[tuple[tuple[int, int, int], tuple[int, int, int]], Image.Image] = {}


def _gradient_image(
    colour_from: tuple[int, int, int], colour_to: tuple[int, int, int]
) -> Image.Image:
    """A 512x512 sRGB image of the diagonal gradient (resized when used)."""
    key = (colour_from, colour_to)
    cached = _gradient_cache.get(key)
    if cached is not None:
        return cached

    n = 512
    image = Image.new("RGB", (n, n))
    pixels = image.load()
    x0, y0 = GRADIENT_P0
    x1, y1 = GRADIENT_P1
    dx, dy = x1 - x0, y1 - y0
    denominator = dx * dx + dy * dy

    for y in range(n):
        py = (y + 0.5) / n * VIEW
        for x in range(n):
            px = (x + 0.5) / n * VIEW
            t = ((px - x0) * dx + (py - y0) * dy) / denominator
            t = 0.0 if t < 0.0 else 1.0 if t > 1.0 else t
            pixels[x, y] = (
                round(colour_from[0] + (colour_to[0] - colour_from[0]) * t),
                round(colour_from[1] + (colour_to[1] - colour_from[1]) * t),
                round(colour_from[2] + (colour_to[2] - colour_from[2]) * t),
            )

    _gradient_cache[key] = image
    return image


_glow_cache: dict[int, Image.Image] = {}


def _glow_alpha(size: int) -> Image.Image:
    """Greyscale alpha for the top-left radial highlight."""
    cached = _glow_cache.get(size)
    if cached is not None:
        return cached

    n = 256
    image = Image.new("L", (n, n), 0)
    pixels = image.load()
    cx, cy = GLOW_CENTER
    for y in range(n):
        py = (y + 0.5) / n * VIEW
        for x in range(n):
            px = (x + 0.5) / n * VIEW
            distance = math.hypot(px - cx, py - cy)
            t = min(1.0, distance / GLOW_RADIUS)
            pixels[x, y] = round(255 * GLOW_ALPHA * (1.0 - t))

    if size != n:
        image = image.resize((size, size), Image.Resampling.BILINEAR)
    _glow_cache[size] = image
    return image


# ---------------------------------------------------------------------------
# Shape helpers
# ---------------------------------------------------------------------------
def _px(value: float, scale: float) -> float:
    return value * scale


def _rounded_bounds(inset: float, scale: float) -> tuple[float, float, float, float]:
    x, y, w, h = TILE
    return (
        _px(x + inset, scale),
        _px(y + inset, scale),
        _px(x + w - inset, scale),
        _px(y + h - inset, scale),
    )


def _arc_points(
    centre: tuple[float, float], radius: float, start: float, end: float, steps: int = 96
) -> list[tuple[float, float]]:
    """Upper semicircle in screen coordinates (PIL angles: 0=right, clockwise)."""
    cx, cy = centre
    return [
        (
            cx + radius * math.cos(math.radians(start + (end - start) * i / steps)),
            cy + radius * math.sin(math.radians(start + (end - start) * i / steps)),
        )
        for i in range(steps + 1)
    ]


def _draw_m(
    draw: ImageDraw.ImageDraw,
    scale: float,
    stroke: float,
    baseline: float,
) -> None:
    """Draw the double-arch m with round caps, matching the SVG path.

    PIL's ``line(joint="curve")`` produces hairline spikes on densely sampled
    arcs, so each hump is drawn as an exact half-annulus polygon and each leg
    as a rectangle with a round end cap.
    """
    left_x, middle_x, right_x = STEM_X
    spring = SPRING_Y
    half = stroke / 2.0
    outer = ARCH_RADIUS + half
    inner = ARCH_RADIUS - half
    white = (255, 255, 255, 255)

    def scaled(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
        return [(_px(x, scale), _px(y, scale)) for x, y in points]

    for centre_x in ((left_x + middle_x) / 2.0, (middle_x + right_x) / 2.0):
        centre = (centre_x, spring)
        outer_arc = _arc_points(centre, outer, 180.0, 360.0)
        inner_arc = _arc_points(centre, inner, 360.0, 180.0)
        draw.polygon(scaled(outer_arc + inner_arc), fill=white)

    for x in STEM_X:
        draw.rectangle(
            scaled([(x - half, spring), (x + half, baseline)]),
            fill=white,
        )
        cx, cy = _px(x, scale), _px(baseline, scale)
        radius = half * scale
        draw.ellipse((cx - radius, cy - radius, cx + radius, cy + radius), fill=white)


# ---------------------------------------------------------------------------
# Icon rendering
# ---------------------------------------------------------------------------
def render(size: int, *, variant: str = "new") -> Image.Image:
    """Render one square RGBA icon.

    variant:
      new   - full design (gradient + glow + inner stroke)
      small - 16/24px variant (flat colour, thicker stroke, no inner stroke)
      old   - the previous design, for the comparison sheet
    """
    if size <= 0:
        raise ValueError("size must be positive")
    if variant not in {"new", "small", "old"}:
        raise ValueError(f"unknown variant {variant!r}")

    supersample = 4 if size <= 256 else 2
    work = size * supersample
    scale = work / VIEW

    # RGB is painted across the whole canvas so downsampling does not blend
    # transparent pixels against black (no dark fringe at the rounded corners).
    if variant == "old":
        colour_from, colour_to = _rgb(OLD_GRADIENT_FROM), _rgb(OLD_GRADIENT_TO)
        gradient = _gradient_image(colour_from, colour_to).resize(
            (work, work), Image.Resampling.BICUBIC
        )
        base = gradient.convert("RGBA")
        radius = OLD_RADIUS
        stroke = OLD_STROKE
        baseline = OLD_BASELINE_Y
        add_glow = False
        add_inner_stroke = False
    elif variant == "small":
        base = Image.new("RGBA", (work, work), _rgb(SMALL_FLAT) + (255,))
        radius = RADIUS
        stroke = SMALL_STROKE
        baseline = BASELINE_Y
        add_glow = False
        add_inner_stroke = False
    else:
        colour_from, colour_to = _rgb(GRADIENT_FROM), _rgb(GRADIENT_TO)
        gradient = _gradient_image(colour_from, colour_to).resize(
            (work, work), Image.Resampling.BICUBIC
        )
        base = gradient.convert("RGBA")
        radius = RADIUS
        stroke = STROKE
        baseline = BASELINE_Y
        add_glow = True
        add_inner_stroke = True

    # Clip the tile to the rounded square.
    mask = Image.new("L", (work, work), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        _rounded_bounds(0.0, scale), radius=_px(radius, scale), fill=255
    )
    base.putalpha(mask)

    if add_glow:
        glow = _glow_alpha(work)
        glow = ImageChops.multiply(glow, mask)
        white = Image.new("RGBA", (work, work), (255, 255, 255, 0))
        white.putalpha(glow)
        base = Image.alpha_composite(base, white)

    if add_inner_stroke:
        outer = Image.new("L", (work, work), 0)
        ImageDraw.Draw(outer).rounded_rectangle(
            _rounded_bounds(INNER_STROKE_INSET - INNER_STROKE_WIDTH / 2.0, scale),
            radius=_px(radius - (INNER_STROKE_INSET - INNER_STROKE_WIDTH / 2.0), scale),
            fill=255,
        )
        inner = Image.new("L", (work, work), 0)
        ImageDraw.Draw(inner).rounded_rectangle(
            _rounded_bounds(INNER_STROKE_INSET + INNER_STROKE_WIDTH / 2.0, scale),
            radius=_px(radius - (INNER_STROKE_INSET + INNER_STROKE_WIDTH / 2.0), scale),
            fill=255,
        )
        ring = ImageChops.subtract(outer, inner).point(
            lambda value: round(value * INNER_STROKE_ALPHA)
        )
        white = Image.new("RGBA", (work, work), (255, 255, 255, 0))
        white.putalpha(ring)
        base = Image.alpha_composite(base, white)

    draw = ImageDraw.Draw(base)
    _draw_m(draw, scale, stroke, baseline)

    return base.resize((size, size), Image.Resampling.BOX)


# ---------------------------------------------------------------------------
# ICO container
# ---------------------------------------------------------------------------
def _dib_entry(image: Image.Image) -> bytes:
    """32bpp BMP with an AND mask, as expected inside an ICO."""
    image = image.convert("RGBA")
    width, height = image.size
    pixels = image.load()

    xor_rows: list[bytes] = []
    for y in range(height - 1, -1, -1):
        row = bytearray()
        for x in range(width):
            red, green, blue, alpha = pixels[x, y]
            row.extend((blue, green, red, alpha))
        xor_rows.append(bytes(row))
    xor_bitmap = b"".join(xor_rows)

    mask_stride = ((width + 31) // 32) * 4
    mask_rows = bytearray()
    for y in range(height - 1, -1, -1):
        row = bytearray(mask_stride)
        for x in range(width):
            if pixels[x, y][3] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        mask_rows.extend(row)

    header = struct.pack(
        "<IiiHHIIiiII",
        40,
        width,
        height * 2,
        1,
        32,
        0,
        len(xor_bitmap) + len(mask_rows),
        0,
        0,
        0,
        0,
    )
    return header + xor_bitmap + bytes(mask_rows)


def _png_entry(image: Image.Image) -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def write_ico(images: dict[int, Image.Image], path: Path) -> list[int]:
    entries: list[tuple[int, bytes]] = []
    for size in sorted(images):
        # BMP entries are the most widely compatible; PNG compression is only
        # used for the 256px entry, matching what Pillow itself writes.
        data = _png_entry(images[size]) if size >= 256 else _dib_entry(images[size])
        entries.append((size, data))

    header = struct.pack("<HHH", 0, 1, len(entries))
    directory = bytearray()
    payload = bytearray()
    offset = 6 + 16 * len(entries)
    for size, data in entries:
        dimension = 0 if size >= 256 else size
        directory.extend(
            struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(data), offset)
        )
        payload.extend(data)
        offset += len(data)

    path.write_bytes(header + bytes(directory) + bytes(payload))
    return [size for size, _ in entries]


# ---------------------------------------------------------------------------
# Comparison sheet
# ---------------------------------------------------------------------------
def _font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    names = ["DejaVuSans-Bold.ttf", "DejaVuSans.ttf"] if bold else ["DejaVuSans.ttf"]
    for name in names:
        for directory in (
            Path("/usr/share/fonts/truetype/dejavu"),
            Path("/usr/share/fonts/truetype/liberation"),
        ):
            candidate = directory / name
            if candidate.exists():
                return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def _centred_text(
    draw: ImageDraw.ImageDraw,
    xy: tuple[float, float],
    text: str,
    font: ImageFont.FreeTypeFont | ImageFont.ImageFont,
    fill: tuple[int, int, int, int],
) -> None:
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    draw.text(
        (xy[0] - (right - left) / 2.0, xy[1] - (bottom - top) / 2.0),
        text,
        font=font,
        fill=fill,
    )


def build_comparison() -> Image.Image:
    sizes = (16, 32, 48, 256)
    margin = 40
    gap = 24
    column_widths = {
        16: 140,
        32: 140,
        48: 160,
        256: 560,
    }
    icon_area_height = 256
    band_height = 352
    header_height = 96
    canvas_width = margin * 2 + sum(column_widths[size] for size in sizes) + gap * (len(sizes) - 1)
    canvas_height = header_height + band_height * 2 + margin + 28

    canvas = Image.new("RGBA", (canvas_width, canvas_height), (11, 16, 20, 255))
    draw = ImageDraw.Draw(canvas)
    title_font = _font(24, bold=True)
    subtitle_font = _font(13)
    size_font = _font(14, bold=True)
    label_font = _font(12)

    draw.text((margin, 24), "moLink icon — old vs new", font=title_font, fill=(242, 247, 250, 255))
    draw.text(
        (margin, 56),
        "actual pixel sizes · old (left) / new (right) · dark and light backgrounds",
        font=subtitle_font,
        fill=(143, 163, 174, 255),
    )

    bands = (
        {
            "top": header_height,
            "background": (20, 26, 32, 255),
            "border": (43, 53, 62, 255),
            "text": (147, 166, 178, 255),
            "size_text": (226, 235, 240, 255),
        },
        {
            "top": header_height + band_height + 16,
            "background": (243, 246, 248, 255),
            "border": (214, 222, 228, 255),
            "text": (100, 116, 126, 255),
            "size_text": (38, 54, 64, 255),
        },
    )

    for band in bands:
        top = band["top"]
        draw.rounded_rectangle(
            (margin, top, canvas_width - margin, top + band_height),
            radius=18,
            fill=band["background"],
            outline=band["border"],
            width=1,
        )

        x = margin
        for size in sizes:
            width = column_widths[size]
            centre = x + width / 2.0
            _centred_text(
                draw, (centre, top + 24), f"{size} px", size_font, band["size_text"]
            )

            pair_gap = 24
            pair_width = size * 2 + pair_gap
            old_left = centre - pair_width / 2.0
            new_left = old_left + size + pair_gap
            icon_top = top + 52 + (icon_area_height - size) / 2.0

            old = render(size, variant="old")
            new = render(size, variant="small" if size in SMALL_SIZES else "new")
            canvas.alpha_composite(old, (round(old_left), round(icon_top)))
            canvas.alpha_composite(new, (round(new_left), round(icon_top)))

            label_y = top + band_height - 28
            _centred_text(
                draw, (old_left + size / 2.0, label_y), "old", label_font, band["text"]
            )
            _centred_text(
                draw, (new_left + size / 2.0, label_y), "new", label_font, band["text"]
            )

            x += width + gap

    draw.text(
        (margin, canvas_height - 30),
        "16 px uses the small-size variant (flat #17B8B4, stroke 3.0, no inner stroke); "
        "32/48/256 use the full gradient design",
        font=_font(11),
        fill=(120, 136, 146, 255),
    )

    return canvas.convert("RGB")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def _write_png(image: Image.Image, name: str) -> tuple[Path, int]:
    path = ICON_DIR / name
    image.save(path, format="PNG", optimize=True)
    return path, path.stat().st_size


def main() -> None:
    ICON_DIR.mkdir(parents=True, exist_ok=True)
    COMPARE_PATH.parent.mkdir(parents=True, exist_ok=True)

    rendered = {
        size: render(size, variant="small" if size in SMALL_SIZES else "new")
        for size in PNG_SIZES
    }

    written: list[tuple[Path, int, int]] = []
    for size in PNG_SIZES:
        name = f"{size}x{size}.png"
        path, byte_count = _write_png(rendered[size], name)
        written.append((path, size, byte_count))

    # Compatibility names already used by the shell and packaging scripts.
    aliases = {
        "icon.png": 512,
        "icon-1024.png": 1024,
        "128x128@2x.png": 256,
    }
    for name, size in aliases.items():
        path, byte_count = _write_png(rendered[size], name)
        written.append((path, size, byte_count))

    ico_path = ICON_DIR / "icon.ico"
    ico_sizes = write_ico({size: rendered[size] for size in ICO_SIZES}, ico_path)

    icns_path = ICON_DIR / "icon.icns"
    png2icns = shutil.which("png2icns")
    if not icns_path.exists():
        icns_note = "missing: not generated (Pillow cannot write ICNS; install png2icns)"
    elif png2icns is None:
        icns_note = "kept (unchanged): Pillow cannot write ICNS and png2icns is not installed"
    else:
        icns_note = (
            f"kept (unchanged): png2icns found at {png2icns}, "
            "but this script does not invoke it"
        )

    comparison = build_comparison()
    comparison.save(COMPARE_PATH, format="PNG", optimize=True)

    print("moLink icons — generated from the 32-unit design grid")
    print(f"  root: {ROOT}")
    print("  small variant (16/24): stroke 3.0, flat #17B8B4, no glow/inner stroke")
    print()
    for path, size, byte_count in written:
        relative = path.relative_to(ROOT)
        print(f"  {str(relative):38} {size:4} x {size:<4}  {byte_count:>7} bytes")
    print(f"  {str(ico_path.relative_to(ROOT)):38} {len(ico_sizes):>4} entries   "
          f"{ico_path.stat().st_size:>7} bytes  ({', '.join(str(s) for s in ico_sizes)})")
    print(f"  {str(icns_path.relative_to(ROOT)):38} {icns_note}")
    print(f"  {str(COMPARE_PATH.relative_to(ROOT)):38} "
          f"{comparison.width} x {comparison.height}  {COMPARE_PATH.stat().st_size:>7} bytes")


if __name__ == "__main__":
    main()
