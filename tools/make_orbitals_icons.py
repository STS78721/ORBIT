"""Generate original Orbitals PC icons & emblem artwork (zero copyrighted assets).

Draws procedural retro-anime sci-fi twin-orbit + starburst vector artwork at 4x
supersampling and emits:
  - runtime/orbitals.png         (512x512 rounded tile for Setup header & window icon)
  - runtime/orbitals_emblem.png  (512x512 transparent floating twin-orbit starburst emblem)
  - runtime/orbitals.ico         (multi-size Windows icon for orbitals.rc & Setup)
  - launcher/assets/orbitals.ico / orbitals.png / orbitals_emblem.png
"""

from __future__ import annotations

import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent


def _rot(x: float, y: float, cx: float, cy: float, angle: float) -> tuple[float, float]:
    ca, sa = math.cos(angle), math.sin(angle)
    dx, dy = x - cx, y - cy
    return (cx + dx * ca - dy * sa, cy + dx * sa + dy * ca)


def _ellipse_points(cx: float, cy: float, rx: float, ry: float, angle: float,
                    a0: float = 0.0, a1: float = math.tau, steps: int = 160) -> list[tuple[float, float]]:
    pts: list[tuple[float, float]] = []
    for i in range(steps + 1):
        t = a0 + (a1 - a0) * (i / steps)
        x = cx + math.cos(t) * rx
        y = cy + math.sin(t) * ry
        pts.append(_rot(x, y, cx, cy, angle))
    return pts


def _draw_thick_arc(img: Image.Image, pts: list[tuple[float, float]], width: int,
                    color: tuple[int, int, int, int], blur: float = 0.0) -> None:
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    if len(pts) >= 2:
        d.line(pts, fill=color, width=width, joint="curve")
        r = width * 0.5
        for x, y in (pts[0], pts[-1]):
            d.ellipse((x - r, y - r, x + r, y + r), fill=color)
    if blur > 0:
        layer = layer.filter(ImageFilter.GaussianBlur(blur))
    img.alpha_composite(layer)


def _draw_star4(draw: ImageDraw.ImageDraw, cx: float, cy: float, rx: float, ry: float,
                inner: float, fill: tuple[int, int, int, int]) -> None:
    pts = [
        (cx, cy - ry),
        (cx + inner, cy - inner),
        (cx + rx, cy),
        (cx + inner, cy + inner),
        (cx, cy + ry),
        (cx - inner, cy + inner),
        (cx - rx, cy),
        (cx - inner, cy - inner),
    ]
    draw.polygon(pts, fill=fill)


def _draw_glow_circle(img: Image.Image, cx: float, cy: float, r: float,
                      color: tuple[int, int, int, int], blur: float) -> None:
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)
    if blur > 0:
        layer = layer.filter(ImageFilter.GaussianBlur(blur))
    img.alpha_composite(layer)


def _draw_orbital_core(img: Image.Image, cx: float, cy: float, scale: float) -> None:
    """Draw twin interlocking cyan + coral-orange orbital rings, planetoid, moons, and starburst."""
    rx = 390 * scale
    ry = 148 * scale
    ang1 = math.radians(-27.0)  # Cyan orbital ring (Player 1)
    ang2 = math.radians(27.0)   # Coral-orange orbital ring (Player 2)
    planet_r = 164 * scale

    # Ambient cosmic aura behind rings & core
    _draw_glow_circle(img, cx - 55 * scale, cy - 20 * scale, 285 * scale, (56, 186, 255, 78), 64 * scale)
    _draw_glow_circle(img, cx + 65 * scale, cy + 25 * scale, 265 * scale, (255, 96, 52, 72), 64 * scale)

    # Back halves of both orbital rings (pi .. 2*pi = upper/back arc)
    back1 = _ellipse_points(cx, cy, rx, ry, ang1, math.pi * 0.94, math.pi * 2.06, 120)
    back2 = _ellipse_points(cx, cy, rx, ry, ang2, math.pi * 0.94, math.pi * 2.06, 120)
    _draw_thick_arc(img, back1, int(36 * scale), (38, 178, 255, 115), blur=16 * scale)
    _draw_thick_arc(img, back1, int(22 * scale), (58, 198, 255, 240))
    _draw_thick_arc(img, back2, int(36 * scale), (255, 92, 46, 110), blur=16 * scale)
    _draw_thick_arc(img, back2, int(22 * scale), (255, 116, 62, 240))

    # Central stylized cosmic planetoid / station core
    pl_layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    pd = ImageDraw.Draw(pl_layer)
    # Outer dark steel border
    pd.ellipse((cx - planet_r - 7 * scale, cy - planet_r - 7 * scale,
                cx + planet_r + 7 * scale, cy + planet_r + 7 * scale),
               fill=(8, 14, 30, 255))
    # Deep indigo sphere base
    pd.ellipse((cx - planet_r, cy - planet_r, cx + planet_r, cy + planet_r),
               fill=(20, 38, 78, 255))
    # Retro anime cel-shaded cyan upper-left crescent
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).ellipse((cx - planet_r, cy - planet_r, cx + planet_r, cy + planet_r), fill=255)
    shade = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shade)
    # Mid-tone oceanic blue band
    sd.ellipse((cx - planet_r * 1.12, cy - planet_r * 1.15,
                cx + planet_r * 0.72, cy + planet_r * 0.72),
               fill=(36, 98, 178, 230))
    # Bright sky-cyan cel highlight
    sd.ellipse((cx - planet_r * 0.92, cy - planet_r * 0.98,
                cx + planet_r * 0.25, cy + planet_r * 0.16),
               fill=(82, 206, 255, 210))
    # Warm coral-orange bounce rim on lower right (cel reflection of second orbit)
    sd.arc((cx - planet_r * 0.92, cy - planet_r * 0.92,
            cx + planet_r * 0.92, cy + planet_r * 0.92),
           start=12, end=105, fill=(255, 128, 72, 215), width=int(22 * scale))
    # Equatorial station band (subtle retro tech stripe)
    band_pts = _ellipse_points(cx, cy, planet_r * 1.05, planet_r * 0.24, math.radians(-12), 0.0, math.pi, 64)
    sd.line(band_pts, fill=(14, 24, 52, 165), width=int(14 * scale))
    pl_layer.paste(shade, (0, 0), mask)
    # Crisp rim stroke around planetoid
    pd.ellipse((cx - planet_r, cy - planet_r, cx + planet_r, cy + planet_r),
               outline=(128, 224, 255, 220), width=int(7 * scale))
    img.alpha_composite(pl_layer)

    # Front halves of both orbital rings (0 .. pi = lower/front arc crossing in front)
    front1 = _ellipse_points(cx, cy, rx, ry, ang1, -0.04, math.pi + 0.04, 120)
    front2 = _ellipse_points(cx, cy, rx, ry, ang2, -0.04, math.pi + 0.04, 120)
    hi1 = _ellipse_points(cx, cy, rx, ry, ang1, math.pi * 0.14, math.pi * 0.86, 96)
    hi2 = _ellipse_points(cx, cy, rx, ry, ang2, math.pi * 0.14, math.pi * 0.86, 96)
    # Soft outer glow
    _draw_thick_arc(img, front1, int(44 * scale), (56, 204, 255, 145), blur=18 * scale)
    _draw_thick_arc(img, front2, int(44 * scale), (255, 98, 48, 145), blur=18 * scale)
    # Crisp solid orbital ribbons (seamless width with back arcs)
    _draw_thick_arc(img, front1, int(23 * scale), (66, 208, 255, 255))
    _draw_thick_arc(img, hi1, int(8 * scale), (222, 248, 255, 220), blur=1.5 * scale)
    _draw_thick_arc(img, front2, int(23 * scale), (255, 108, 54, 255))
    _draw_thick_arc(img, hi2, int(8 * scale), (255, 232, 196, 220), blur=1.5 * scale)

    # Twin co-op satellite moons on opposite orbital nodes
    # Cyan node (upper-left on cyan ring)
    mx1, my1 = _rot(cx + math.cos(math.radians(196)) * rx, cy + math.sin(math.radians(196)) * ry, cx, cy, ang1)
    _draw_glow_circle(img, mx1, my1, 56 * scale, (76, 214, 255, 175), 22 * scale)
    d = ImageDraw.Draw(img)
    r1 = 36 * scale
    d.ellipse((mx1 - r1 - 5 * scale, my1 - r1 - 5 * scale, mx1 + r1 + 5 * scale, my1 + r1 + 5 * scale),
              fill=(10, 18, 38, 255))
    d.ellipse((mx1 - r1, my1 - r1, mx1 + r1, my1 + r1), fill=(72, 212, 255, 255))
    d.ellipse((mx1 - r1 * 0.58, my1 - r1 * 0.62, mx1 + r1 * 0.16, my1 + r1 * 0.12), fill=(232, 250, 255, 255))

    # Coral-rose node (lower-right on coral ring)
    mx2, my2 = _rot(cx + math.cos(math.radians(24)) * rx, cy + math.sin(math.radians(24)) * ry, cx, cy, ang2)
    _draw_glow_circle(img, mx2, my2, 56 * scale, (255, 102, 58, 175), 22 * scale)
    d = ImageDraw.Draw(img)
    r2 = 36 * scale
    d.ellipse((mx2 - r2 - 5 * scale, my2 - r2 - 5 * scale, mx2 + r2 + 5 * scale, my2 + r2 + 5 * scale),
              fill=(10, 18, 38, 255))
    d.ellipse((mx2 - r2, my2 - r2, mx2 + r2, my2 + r2), fill=(255, 114, 72, 255))
    d.ellipse((mx2 - r2 * 0.58, my2 - r2 * 0.62, mx2 + r2 * 0.16, my2 + r2 * 0.12), fill=(255, 234, 224, 255))

    # Iconic retro-anime 4-point starburst cross at the front ring intersection
    sx, sy = cx, cy + 72 * scale
    star_glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sgd = ImageDraw.Draw(star_glow)
    _draw_star4(sgd, sx, sy, 188 * scale, 220 * scale, 26 * scale, (255, 86, 40, 225))
    star_glow = star_glow.filter(ImageFilter.GaussianBlur(18 * scale))
    img.alpha_composite(star_glow)

    star_sharp = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ssd = ImageDraw.Draw(star_sharp)
    _draw_star4(ssd, sx, sy, 172 * scale, 206 * scale, 21 * scale, (255, 92, 44, 255))
    _draw_star4(ssd, sx, sy, 128 * scale, 152 * scale, 14 * scale, (255, 194, 92, 255))
    _draw_star4(ssd, sx, sy, 86 * scale, 102 * scale, 9 * scale, (255, 253, 246, 255))
    img.alpha_composite(star_sharp)


def make_tile_icon(size: int = 512) -> Image.Image:
    ss = size * 4  # 2048x2048 supersampled canvas
    img = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    margin = int(ss * 0.045)
    radius = int(ss * 0.21)

    # Rounded square mask
    mask = Image.new("L", (ss, ss), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (margin, margin, ss - margin, ss - margin), radius=radius, fill=255
    )

    # Deep space gradient backdrop inside rounded tile
    bg = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    bg_d = ImageDraw.Draw(bg)
    top_col = (10, 18, 42)
    mid_col = (17, 30, 66)
    bot_col = (8, 12, 28)
    for y in range(margin, ss - margin):
        f = (y - margin) / max(1, (ss - 2 * margin))
        if f < 0.55:
            k = f / 0.55
            col = tuple(int(top_col[i] * (1 - k) + mid_col[i] * k) for i in range(3)) + (255,)
        else:
            k = (f - 0.55) / 0.45
            col = tuple(int(mid_col[i] * (1 - k) + bot_col[i] * k) for i in range(3)) + (255,)
        bg_d.line([(margin, y), (ss - margin, y)], fill=col)

    # Subtle diagonal retro-anime speed beams in the cosmic background
    beams = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beams)
    for offset, col in [
        (-420, (56, 188, 255, 22)),
        (-180, (56, 188, 255, 16)),
        (260, (255, 102, 54, 22)),
        (480, (255, 128, 72, 16)),
    ]:
        pts = [
            (ss * 0.5 + offset - 95, margin),
            (ss * 0.5 + offset + 95, margin),
            (ss * 0.5 + offset - 320, ss - margin),
            (ss * 0.5 + offset - 510, ss - margin),
        ]
        bd.polygon(pts, fill=col)
    beams = beams.filter(ImageFilter.GaussianBlur(26))
    bg.alpha_composite(beams)

    # Tiny cosmic 4-point star glints
    sd = ImageDraw.Draw(bg)
    for sx, sy, sr, col in [
        (0.18, 0.20, 34, (132, 228, 255, 225)),
        (0.83, 0.19, 28, (255, 198, 118, 220)),
        (0.20, 0.80, 24, (255, 158, 178, 200)),
        (0.82, 0.81, 32, (120, 218, 255, 215)),
        (0.75, 0.28, 18, (240, 248, 255, 195)),
        (0.26, 0.30, 16, (240, 248, 255, 180)),
    ]:
        _draw_star4(sd, ss * sx, ss * sy, sr, sr * 1.15, sr * 0.22, col)

    # Subtle orbital core artwork centered in tile
    _draw_orbital_core(bg, ss * 0.5, ss * 0.485, scale=(ss / 1024.0) * 0.90)

    # Apply rounded tile mask
    img.paste(bg, (0, 0), mask)

    # Clean dual-tone luminous border frame
    border = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    bdr = ImageDraw.Draw(border)
    bdr.rounded_rectangle(
        (margin + 6, margin + 6, ss - margin - 6, ss - margin - 6),
        radius=radius - 6,
        outline=(76, 201, 255, 190),
        width=18,
    )
    bdr.rounded_rectangle(
        (margin + 22, margin + 22, ss - margin - 22, ss - margin - 22),
        radius=radius - 22,
        outline=(255, 118, 66, 95),
        width=8,
    )
    img.alpha_composite(border)
    return img.resize((size, size), Image.Resampling.LANCZOS)


def make_emblem_icon(size: int = 512) -> Image.Image:
    ss = size * 4
    img = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    _draw_orbital_core(img, ss * 0.5, ss * 0.49, scale=(ss / 1024.0) * 1.04)
    # Floating accent stars around the transparent emblem
    d = ImageDraw.Draw(img)
    for sx, sy, sr, col in [
        (0.16, 0.23, 36, (124, 224, 255, 235)),
        (0.85, 0.20, 32, (255, 194, 108, 230)),
        (0.84, 0.78, 26, (255, 152, 174, 210)),
        (0.19, 0.76, 24, (132, 224, 255, 210)),
    ]:
        _draw_star4(d, ss * sx, ss * sy, sr, sr * 1.2, sr * 0.22, col)
    return img.resize((size, size), Image.Resampling.LANCZOS)


def main() -> None:
    tile = make_tile_icon(512)
    emblem = make_emblem_icon(512)

    rt_dir = ROOT / "runtime"
    assets_dir = ROOT / "launcher" / "assets"
    assets_dir.mkdir(parents=True, exist_ok=True)

    tile_png = rt_dir / "orbitals.png"
    emblem_png = rt_dir / "orbitals_emblem.png"
    rt_ico = rt_dir / "orbitals.ico"
    launch_ico = assets_dir / "orbitals.ico"
    launch_png = assets_dir / "orbitals.png"
    launch_emblem = assets_dir / "orbitals_emblem.png"

    tile.save(tile_png, format="PNG")
    emblem.save(emblem_png, format="PNG")
    tile.save(launch_png, format="PNG")
    emblem.save(launch_emblem, format="PNG")

    ico_sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    tile.save(rt_ico, format="ICO", sizes=ico_sizes)
    tile.save(launch_ico, format="ICO", sizes=ico_sizes)

    for p in (tile_png, emblem_png, rt_ico, launch_ico):
        print(f"Wrote {p.relative_to(ROOT)} ({p.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
