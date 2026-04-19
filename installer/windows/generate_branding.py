"""
Generate Inno Setup brand assets for Giannitune installer.

Outputs into installer/windows/branding/:
  - app.ico                      (256+128+64+48+32+16 multi-res)
  - wizard_image.bmp             (164x314 left-side banner)
  - wizard_image_2x.bmp          (329x629 high-DPI banner)
  - wizard_small.bmp             (55x58 top-right small)
  - wizard_small_2x.bmp          (110x116 high-DPI small)

Colours match the plugin's GianniLookAndFeel palette:
  bgBase      #0C0D10
  bgPanelTop  #1A1C20
  accent      #C8FF3A  (olive neon)
  ink100      #F3EFE7  (warm cream)
  ink60       #8A8578

The monogram disc and wordmark from the plugin UI are recreated here
as Pillow-drawn vector so they match the plugin's visual brand.
"""

from PIL import Image, ImageDraw, ImageFont, ImageFilter
from pathlib import Path
import os

HERE = Path(__file__).parent
OUT = HERE / "branding"
OUT.mkdir(exist_ok=True, parents=True)

# Palette (must match src/gui/GianniLookAndFeel.h)
BG_BASE      = (0x0C, 0x0D, 0x10)
BG_PANEL_TOP = (0x1A, 0x1C, 0x20)
BG_PANEL_BOT = (0x0C, 0x0D, 0x10)
ACCENT       = (0xC8, 0xFF, 0x3A)
ACCENT_GLOW  = (0xE4, 0xFF, 0x78)
INK100       = (0xF3, 0xEF, 0xE7)
INK60        = (0x8A, 0x85, 0x78)
INK40        = (0x5A, 0x57, 0x4E)

# Locate Fraunces Italic (used for plugin wordmark)
FONT_FRAUNCES = HERE / "../../resources/fonts/Fraunces-Italic.ttf"
FONT_INTER    = HERE / "../../resources/fonts/InterTight-Medium.ttf"
FONT_MONO     = HERE / "../../resources/fonts/JetBrainsMono-Medium.ttf"

# -----------------------------------------------------------------
# Primitives
# -----------------------------------------------------------------
def vgrad(w, h, top, bot, mid=None):
    """Vertical gradient, optional midpoint."""
    img = Image.new("RGB", (w, h), top)
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        if mid is not None and t < 0.55:
            u = t / 0.55
            c = tuple(int(top[i] + (mid[i] - top[i]) * u) for i in range(3))
        elif mid is not None:
            u = (t - 0.55) / 0.45
            c = tuple(int(mid[i] + (bot[i] - mid[i]) * u) for i in range(3))
        else:
            c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def radial_halo(size, cx, cy, radius, colour, alpha_center=80):
    """Additive glow spot — returns RGBA."""
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    px = img.load()
    for y in range(size[1]):
        for x in range(size[0]):
            dx = (x - cx) / radius
            dy = (y - cy) / radius
            d = (dx * dx + dy * dy) ** 0.5
            if d < 1.0:
                a = int(alpha_center * (1.0 - d) ** 2)
                px[x, y] = (*colour, a)
    return img


def paste_alpha(base, overlay, pos=(0, 0)):
    if overlay.mode != "RGBA":
        overlay = overlay.convert("RGBA")
    if base.mode != "RGBA":
        base = base.convert("RGBA")
    out = base.copy()
    out.alpha_composite(overlay, pos)
    return out


def monogram_disc(size, dot_radius_ratio=0.22):
    """
    The plugin's MonogramDisc: 30×30 dark disc with a glowing accent dot
    centre. For the installer, we render scalable.
    """
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Dark metallic disc with subtle rim
    cx = cy = s // 2
    r = s // 2 - 1
    # body gradient (approximation: two concentric fills)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(0x16, 0x18, 0x1B))
    # rim highlight
    d.ellipse([cx - r, cy - r, cx + r, cy + r],
              outline=(0x2A, 0x2D, 0x32), width=max(1, s // 30))

    # inner subtle lightness to imitate shine
    inner_r = int(r * 0.75)
    d.ellipse([cx - inner_r, cy - inner_r, cx + inner_r, cy + inner_r],
              fill=(0x1A, 0x1D, 0x20))

    # accent dot with glow
    dot_r = int(s * dot_radius_ratio)
    glow_r = dot_r * 3
    halo = radial_halo((glow_r * 2, glow_r * 2), glow_r, glow_r,
                        glow_r, ACCENT, alpha_center=140)
    img.alpha_composite(halo, (cx - glow_r, cy - glow_r))

    d = ImageDraw.Draw(img)
    d.ellipse([cx - dot_r, cy - dot_r, cx + dot_r, cy + dot_r],
              fill=ACCENT)

    return img


def wordmark(text, font_path, size, colour, letter_spacing=0):
    """Render the wordmark 'Giannitune' in Fraunces Italic."""
    font = ImageFont.truetype(str(font_path), size)
    # measure width with spacing
    dummy = Image.new("RGBA", (10, 10))
    dd = ImageDraw.Draw(dummy)
    if letter_spacing:
        w = sum(dd.textbbox((0, 0), ch, font=font)[2] for ch in text) \
            + letter_spacing * (len(text) - 1)
    else:
        bbox = dd.textbbox((0, 0), text, font=font)
        w = bbox[2] - bbox[0]
    h = int(size * 1.2)
    img = Image.new("RGBA", (int(w + size), h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if letter_spacing:
        x = 0
        for ch in text:
            d.text((x, 0), ch, font=font, fill=colour)
            x += dd.textbbox((0, 0), ch, font=font)[2] + letter_spacing
    else:
        d.text((0, 0), text, font=font, fill=colour)
    return img


# -----------------------------------------------------------------
# Wizard image (164x314 — left sidebar of the installer wizard)
# -----------------------------------------------------------------
def make_wizard_image(scale=1):
    W = 164 * scale
    H = 314 * scale

    # Deep charcoal vertical gradient matching plugin panel
    base = vgrad(W, H, BG_PANEL_TOP, BG_BASE, mid=BG_PANEL_BOT).convert("RGBA")

    # Radial halo at top matching plugin "halo over panel"
    halo = radial_halo((W, H), W // 2, H // 4, H // 2,
                        (0x1A, 0x1C, 0x20), alpha_center=120)
    base.alpha_composite(halo)

    # Giant subtle accent glow low in the image — ties the eye down
    accent_halo = radial_halo((W, H), W // 2, int(H * 0.72), int(H * 0.45),
                               ACCENT, alpha_center=18)
    base.alpha_composite(accent_halo)

    # Monogram disc centered in upper third
    disc_size = int(72 * scale)
    disc = monogram_disc(disc_size)
    dx = (W - disc_size) // 2
    dy = int(H * 0.18)
    base.alpha_composite(disc, (dx, dy))

    # Wordmark "Giannitune" below
    word = wordmark("Giannitune", FONT_FRAUNCES, int(22 * scale), INK100)
    wx = (W - word.size[0]) // 2
    wy = dy + disc_size + int(14 * scale)
    base.alpha_composite(word, (wx, wy))

    # Version caption
    ver_font = ImageFont.truetype(str(FONT_MONO), int(9 * scale))
    vd = ImageDraw.Draw(base)
    ver_text = "V1.2.1"
    bb = vd.textbbox((0, 0), ver_text, font=ver_font)
    vw = bb[2] - bb[0]
    vd.text(((W - vw) // 2, wy + int(34 * scale)),
            ver_text, font=ver_font, fill=INK60)

    # Hairline rule above tagline
    rule_y = int(H * 0.62)
    vd.line([(int(W * 0.28), rule_y), (int(W * 0.72), rule_y)],
            fill=(255, 255, 255, 40), width=1)

    # Tagline in Inter Tight lowercase — vertical stack of short words
    tag_font = ImageFont.truetype(str(FONT_INTER), int(10 * scale))
    lines = ["open source", "autotune", "für deine stimme"]
    cur_y = rule_y + int(18 * scale)
    for line in lines:
        bb = vd.textbbox((0, 0), line, font=tag_font)
        lw = bb[2] - bb[0]
        vd.text(((W - lw) // 2, cur_y), line,
                font=tag_font, fill=INK100)
        cur_y += int(16 * scale)

    # Bottom accent line (olive)
    vd.rectangle([0, H - int(4 * scale), W, H],
                  fill=ACCENT)

    return base.convert("RGB")


# -----------------------------------------------------------------
# Wizard small image (55x58 — top-right of wizard pages)
# -----------------------------------------------------------------
def make_wizard_small(scale=1):
    W = 55 * scale
    H = 58 * scale
    base = Image.new("RGB", (W, H), BG_PANEL_TOP).convert("RGBA")

    # Subtle panel gradient
    grad = vgrad(W, H, BG_PANEL_TOP, BG_PANEL_BOT).convert("RGBA")
    base.alpha_composite(grad)

    # Centred monogram disc
    s = int(min(W, H) * 0.68)
    disc = monogram_disc(s)
    dx = (W - s) // 2
    dy = (H - s) // 2 - int(2 * scale)
    base.alpha_composite(disc, (dx, dy))

    return base.convert("RGB")


# -----------------------------------------------------------------
# .ico — app icon stack
# -----------------------------------------------------------------
def make_icon():
    """
    Build a single .ico containing multi-res frames:
    16, 32, 48, 64, 128, 256 — standard Windows sizes.
    PIL's .ico encoder only uses the "sizes=" kwarg when the base
    image is large enough; it then internally downscales. We render
    at 256 and let PIL downscale for the smaller sizes. For max
    quality at the smallest sizes we sharpen the 256 version slightly.
    """
    sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    base = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    disc = monogram_disc(256)
    base.alpha_composite(disc)

    ico_path = OUT / "app.ico"
    base.save(ico_path, format="ICO", sizes=sizes)
    return ico_path


# -----------------------------------------------------------------
# Orchestrate
# -----------------------------------------------------------------
if __name__ == "__main__":
    # Validate fonts exist
    for f in (FONT_FRAUNCES, FONT_INTER, FONT_MONO):
        if not f.exists():
            raise SystemExit(f"Missing font: {f}")

    # Wizard images (both standard and 2x for HiDPI Inno Setup 6.x)
    wi = make_wizard_image(scale=1)
    wi_hi = make_wizard_image(scale=2)
    wi.save(OUT / "wizard_image.bmp", "BMP")
    wi_hi.save(OUT / "wizard_image_2x.bmp", "BMP")

    ws = make_wizard_small(scale=1)
    ws_hi = make_wizard_small(scale=2)
    ws.save(OUT / "wizard_small.bmp", "BMP")
    ws_hi.save(OUT / "wizard_small_2x.bmp", "BMP")

    ico = make_icon()

    for f in OUT.iterdir():
        print(f"  {f.name:30s}  {f.stat().st_size:>8d} bytes")

    print(f"\nAll assets written to {OUT}")
