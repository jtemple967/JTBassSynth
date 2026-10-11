#!/usr/bin/env python3
"""Generate the MOD GUI image assets for JTBassSynth.

Writes into ../modgui/:
  panel.png        pedal face (enclosure, silkscreen, footswitch, LED bezel)
  knob.png         64-frame sprite, large knob   (56 px frames)
  knob-small.png   64-frame sprite, trim knob    (34 px frames)
  selector.png     8-frame sprite, voice rotary  (84 px frames)
  screenshot-jtbasssynth.png / thumbnail-jtbasssynth.png

Requires Pillow. Layout constants below must match stylesheet-jtbasssynth.css.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "modgui"))
S = 3  # supersampling factor

W, H = 360, 600
ARC = 135.0  # knob travel: -135 .. +135 degrees

BIG, SMALL, SEL = 56, 34, 84
KNOB_FRAMES = 64

#        symbol         label     cx   cy  size   min    max   default log
KNOBS = [
    ("dry",         "DRY",     52,  70, BIG,    0.0,    1.0,  1.0,  False),
    ("filter",      "FILTER", 136,  70, BIG,   40.0, 8000.0, 300.0, True),
    ("resonance",   "RESO",   224,  70, BIG,    0.0,    1.0,  0.45, False),
    ("synth",       "SYNTH",  308,  70, BIG,    0.0,    1.0,  0.7,  False),
    ("attack",      "ATTACK",  52, 172, BIG,    1.0, 1500.0,  5.0,  True),
    ("decay",       "DECAY",  136, 172, BIG,   20.0, 4000.0, 350.0, True),
    ("env_amount",  "SWEEP",  224, 172, BIG,    0.0,    1.0,  0.6,  False),
    ("glide",       "GLIDE",  308, 172, BIG,    0.0,  500.0,  0.0,  False),
    ("sensitivity", "GATE",   318, 318, SMALL, -80.0, -10.0, -48.0, False),
]
SELECTOR = dict(symbol="voice", cx=180, cy=318, size=SEL, default=2,
                labels=["SAW", "SQUARE", "SAW+SUB", "SQR+SUB",
                        "SUB", "OCT UP", "PWM", "FUZZ"])
LED = (42, 318)
FOOTSWITCH = (180, 530, 30)

ENCLOSURE = (24, 26, 31)
INK = (238, 236, 226)
ACCENT = (64, 214, 196)


def font(size, names=("arialbd.ttf", "DejaVuSans-Bold.ttf", "Arial Bold.ttf")):
    dirs = ["C:/Windows/Fonts", "/usr/share/fonts/truetype/dejavu",
            "/Library/Fonts", "/System/Library/Fonts/Supplemental"]
    for n in names:
        for d in dirs:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default(size)


def logo_font(size):
    return font(size, ("impact.ttf", "Impact.ttf", "DejaVuSans-Bold.ttf"))


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return cx + r * math.sin(a), cy - r * math.cos(a)


def lerp(c1, c2, t):
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c1, c2))


def shaded_disc(d, cx, cy, r, light, dark, steps=40):
    """Disc with a soft highlight toward the top-left."""
    for i in range(steps):
        t = i / (steps - 1)
        rr = r * (1.0 - 0.55 * t)
        ox, oy = -0.25 * r * t, -0.3 * r * t
        d.ellipse([cx + ox - rr, cy + oy - rr, cx + ox + rr, cy + oy + rr],
                  fill=lerp(dark, light, t))


def drop_shadow(size, cx, cy, r, offset, blur, alpha=150):
    sh = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([cx - r, cy - r + offset, cx + r, cy + r + offset],
                               fill=(0, 0, 0, alpha))
    return sh.filter(ImageFilter.GaussianBlur(blur))


# ------------------------------------------------------------------ knobs

def knob_frame(size, angle):
    n = size * S
    c = n / 2
    img = drop_shadow((n, n), c, c, n * 0.44, n * 0.03, n * 0.03)
    d = ImageDraw.Draw(img)

    # knurled skirt
    r = n * 0.45
    d.ellipse([c - r, c - r, c + r, c + r], fill=(12, 12, 14))
    for k in range(36):
        a = angle + k * 10.0
        x1, y1 = polar(c, c, r * 0.80, a)
        x2, y2 = polar(c, c, r * 0.98, a)
        d.line([x1, y1, x2, y2], fill=(46, 46, 52), width=max(1, int(n * 0.018)))

    # cap
    shaded_disc(d, c, c, n * 0.33, (78, 80, 88), (16, 16, 19))

    # pointer
    w = max(2, int(n * 0.055))
    x1, y1 = polar(c, c, n * 0.10, angle)
    x2, y2 = polar(c, c, n * 0.43, angle)
    d.line([x1, y1, x2, y2], fill=INK, width=w)
    for x, y in ((x1, y1), (x2, y2)):
        d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=INK)
    return img.resize((size, size), Image.LANCZOS)


def selector_frame(size, angle):
    n = size * S
    c = n / 2
    img = drop_shadow((n, n), c, c, n * 0.45, n * 0.03, n * 0.03)
    d = ImageDraw.Draw(img)

    # brushed-metal skirt
    shaded_disc(d, c, c, n * 0.46, (220, 224, 230), (120, 124, 132))
    d.ellipse([c - n * 0.46, c - n * 0.46, c + n * 0.46, c + n * 0.46],
              outline=(70, 72, 78), width=S)

    # chicken-head body: tapered pointer plus round hub
    body = (14, 14, 16)
    tip = polar(c, c, n * 0.45, angle)
    tail = polar(c, c, n * 0.30, angle + 180)
    lft = polar(c, c, n * 0.20, angle - 90)
    rgt = polar(c, c, n * 0.20, angle + 90)
    tl = polar(*tail, n * 0.12, angle - 90)
    tr = polar(*tail, n * 0.12, angle + 90)
    d.polygon([tip, rgt, tr, tl, lft], fill=body)
    shaded_disc(d, c, c, n * 0.22, (70, 72, 80), body)

    # white index line
    w = max(2, int(n * 0.04))
    x1, y1 = polar(c, c, n * 0.16, angle)
    x2, y2 = polar(c, c, n * 0.41, angle)
    d.line([x1, y1, x2, y2], fill=INK, width=w)
    return img.resize((size, size), Image.LANCZOS)


def sprite(frames):
    fw, fh = frames[0].size
    out = Image.new("RGBA", (fw * len(frames), fh), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        out.paste(f, (i * fw, 0), f)
    return out


# ------------------------------------------------------------------ panel

def panel():
    n_w, n_h = W * S, H * S
    img = Image.new("RGBA", (n_w, n_h), (0, 0, 0, 0))
    rad = 16 * S

    mask = Image.new("L", (n_w, n_h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, n_w - 1, n_h - 1], rad, fill=255)

    # textured (wrinkle-finish) enclosure
    base = Image.new("RGB", (n_w, n_h), ENCLOSURE)
    noise = Image.effect_noise((n_w // 2, n_h // 2), 26).resize((n_w, n_h)).convert("RGB")
    base = Image.blend(base, noise, 0.06)
    shade = Image.linear_gradient("L").resize((n_w, n_h))
    base = Image.composite(base, Image.new("RGB", (n_w, n_h), (8, 9, 11)),
                           shade.point(lambda v: 255 - v // 3))
    img.paste(base, (0, 0), mask)

    d = ImageDraw.Draw(img)
    d.rounded_rectangle([S, S, n_w - 1 - S, n_h - 1 - S], rad, outline=(60, 63, 72), width=2 * S)
    d.rounded_rectangle([4 * S, 4 * S, n_w - 1 - 4 * S, n_h - 1 - 4 * S], rad - 3 * S,
                        outline=(10, 10, 12), width=S)

    # corner screws
    for sx, sy in ((14, 14), (W - 14, 14), (14, H - 14), (W - 14, H - 14)):
        x, y, r = sx * S, sy * S, 5 * S
        shaded_disc(d, x, y, r, (200, 202, 208), (90, 92, 98), 16)
        d.line([x - r * 0.7, y - r * 0.7, x + r * 0.7, y + r * 0.7], fill=(50, 50, 55), width=S)

    f_lbl = font(11 * S)
    f_sel = font(9 * S)

    # knob ticks + labels
    for _, label, cx, cy, size, *_ in KNOBS:
        x, y = cx * S, cy * S
        r_tick = (size / 2 + 5) * S
        for k in range(11):
            a = -ARC + k * (2 * ARC / 10)
            px, py = polar(x, y, r_tick, a)
            rr = (1.6 if k in (0, 10) else 1.0) * S
            d.ellipse([px - rr, py - rr, px + rr, py + rr], fill=INK)
        d.text((x, y + (size / 2 + 16) * S), label, font=f_lbl if size == BIG else f_sel,
               fill=INK, anchor="mm")

    # voice selector legends
    sx, sy, ssz = SELECTOR["cx"] * S, SELECTOR["cy"] * S, SELECTOR["size"]
    labels = SELECTOR["labels"]
    step = 2 * ARC / (len(labels) - 1)
    for i, label in enumerate(labels):
        a = -ARC + i * step
        p1 = polar(sx, sy, (ssz / 2 + 3) * S, a)
        p2 = polar(sx, sy, (ssz / 2 + 9) * S, a)
        d.line([p1, p2], fill=ACCENT, width=2 * S)
        tx, ty = polar(sx, sy, (ssz / 2 + 14) * S, a)
        sn = math.sin(math.radians(a))
        anchor = "rm" if sn < -0.2 else ("lm" if sn > 0.2 else "mm")
        d.text((tx, ty), label, font=f_sel, fill=INK, anchor=anchor)
    d.text((sx, sy + (ssz / 2 + 34) * S), "VOICE", font=f_lbl, fill=ACCENT, anchor="mm")

    # divider rules
    for yy in (238, 404):
        d.line([24 * S, yy * S, (W - 24) * S, yy * S], fill=(48, 51, 58), width=S)

    # logo
    d.text((W / 2 * S, 428 * S), "J T", font=font(13 * S), fill=ACCENT, anchor="mm")
    d.text((W / 2 * S, 458 * S), "BASS SYNTH", font=logo_font(38 * S), fill=INK, anchor="mm")

    # LED bezel
    lx, ly = LED[0] * S, LED[1] * S
    shaded_disc(d, lx, ly, 8 * S, (210, 212, 218), (100, 102, 108), 16)
    d.ellipse([lx - 5 * S, ly - 5 * S, lx + 5 * S, ly + 5 * S], fill=(40, 6, 6))
    d.text((lx, ly + 18 * S), "ON", font=f_sel, fill=INK, anchor="mm")

    # footswitch: hex nut + domed cap
    fx, fy, fr = (v * S for v in FOOTSWITCH)
    img.alpha_composite(drop_shadow((n_w, n_h), fx, fy, fr, 3 * S, 4 * S, 170))
    d = ImageDraw.Draw(img)
    hexagon = [polar(fx, fy, fr, 30 + 60 * k) for k in range(6)]
    d.polygon(hexagon, fill=(150, 153, 160), outline=(70, 72, 78))
    shaded_disc(d, fx, fy, fr * 0.66, (235, 237, 242), (110, 113, 120))
    d.ellipse([fx - fr * 0.66, fy - fr * 0.66, fx + fr * 0.66, fy + fr * 0.66],
              outline=(80, 82, 88), width=S)

    return img.resize((W, H), Image.LANCZOS)


def frame_for(value, lo, hi, log, frames):
    if log:
        t = math.log(value / lo) / math.log(hi / lo)
    else:
        t = (value - lo) / (hi - lo)
    return int(round(max(0.0, min(1.0, t)) * (frames - 1)))


def main():
    os.makedirs(OUT, exist_ok=True)

    angles = [-ARC + k * (2 * ARC / (KNOB_FRAMES - 1)) for k in range(KNOB_FRAMES)]
    big = sprite([knob_frame(BIG, a) for a in angles])
    small = sprite([knob_frame(SMALL, a) for a in angles])
    nsel = len(SELECTOR["labels"])
    sel = sprite([selector_frame(SEL, -ARC + k * (2 * ARC / (nsel - 1))) for k in range(nsel)])
    face = panel()

    big.save(os.path.join(OUT, "knob.png"), optimize=True)
    small.save(os.path.join(OUT, "knob-small.png"), optimize=True)
    sel.save(os.path.join(OUT, "selector.png"), optimize=True)
    face.save(os.path.join(OUT, "panel.png"), optimize=True)

    # screenshot: face with every control at its default value, LED lit
    shot = face.copy()
    for _, _, cx, cy, size, lo, hi, dflt, log in KNOBS:
        sheet = big if size == BIG else small
        f = frame_for(dflt, lo, hi, log, KNOB_FRAMES)
        tile = sheet.crop((f * size, 0, (f + 1) * size, size))
        shot.alpha_composite(tile, (cx - size // 2, cy - size // 2))
    f = SELECTOR["default"]
    tile = sel.crop((f * SEL, 0, (f + 1) * SEL, SEL))
    shot.alpha_composite(tile, (SELECTOR["cx"] - SEL // 2, SELECTOR["cy"] - SEL // 2))

    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    lx, ly = LED
    gd.ellipse([lx - 9, ly - 9, lx + 9, ly + 9], fill=(255, 40, 30, 120))
    glow = glow.filter(ImageFilter.GaussianBlur(4))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([lx - 5, ly - 5, lx + 5, ly + 5], fill=(255, 60, 45, 255))
    gd.ellipse([lx - 2.5, ly - 3, lx + 1, ly], fill=(255, 190, 180, 255))
    shot.alpha_composite(glow)

    shot.save(os.path.join(OUT, "screenshot-jtbasssynth.png"), optimize=True)
    th_w = 64
    shot.resize((th_w, round(H * th_w / W)), Image.LANCZOS).save(
        os.path.join(OUT, "thumbnail-jtbasssynth.png"), optimize=True)
    print("wrote assets to", OUT)


if __name__ == "__main__":
    main()
