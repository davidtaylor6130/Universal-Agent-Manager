"""Draws the UAM app icon artwork: a white rounded-square robot outline with blue and pink eyes on the
blue-to-magenta brand gradient. Output is flat square artwork; scripts/compose_macos_icon.swift clips it to the
macOS icon grid. Usage: python3 scripts/make_app_icon_art.py [assets/app_icon.png] [robot scale, default 1.32]"""
import math, sys
from PIL import Image, ImageDraw, ImageFilter
OUT = sys.argv[1] if len(sys.argv) > 1 else 'assets/app_icon.png'
SCALE = float(sys.argv[2]) if len(sys.argv) > 2 else 1.32
S, BODY, OFF, K = 1024, 824, 100, 4
B = BODY * K
def squircle(size):
    pts = []
    for i in range(2400):
        t = 2 * math.pi * i / 2400; c, s = math.cos(t), math.sin(t)
        pts.append(((abs(c) ** 0.4 * math.copysign(1, c) + 1) / 2 * (size - 1), (abs(s) ** 0.4 * math.copysign(1, s) + 1) / 2 * (size - 1)))
    m = Image.new('L', (size, size), 0); ImageDraw.Draw(m).polygon(pts, fill=255); return m
def grad(a, b, size=B):
    im = Image.new('RGB', (2, 2)); im.putpixel((0, 0), a); im.putpixel((1, 1), b)
    mid = tuple((a[i] + b[i]) // 2 for i in range(3)); im.putpixel((1, 0), mid); im.putpixel((0, 1), mid)
    return im.resize((size, size), Image.BILINEAR).convert('RGBA')
def P(v): return int(round(v * K))
def finish(layer, name):
    body = layer.resize((BODY, BODY), Image.LANCZOS)
    mask = squircle(B).resize((BODY, BODY), Image.LANCZOS)
    canvas = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    sh = Image.new('RGBA', (S, S), (0, 0, 0, 0)); sh.paste(Image.new('RGBA', (BODY, BODY), (0, 0, 0, 110)), (OFF, OFF + 12), mask)
    canvas = Image.alpha_composite(canvas, sh.filter(ImageFilter.GaussianBlur(14)))
    clipped = Image.new('RGBA', (BODY, BODY), (0, 0, 0, 0)); clipped.paste(body, (0, 0), mask)
    canvas.paste(clipped, (OFF, OFF), clipped); canvas.save(f'{OUT}/{name}.png')
WHITE, BLUE, PINK = (250, 250, 255), (90, 185, 255), (250, 120, 205)
BRAND = ((44, 86, 228), (204, 52, 132))

def robot(d, dy=0, w=34, shoulders=False, eye_fill=(WHITE, WHITE), antenna=True, visor=True):
    def r(x0, y0, x1, y1, rad, fill=None, outline=WHITE, width=w):
        d.rounded_rectangle((P(x0), P(y0 + dy), P(x1), P(y1 + dy)), radius=P(rad), fill=fill, outline=outline if fill is None else None, width=P(width) if fill is None else 0)
    if antenna:
        d.line((P(412), P(206 + dy), P(412), P(150 + dy)), fill=WHITE, width=P(w))
        d.ellipse((P(412 - 30), P(104 + dy), P(412 + 30), P(164 + dy)), outline=WHITE, width=P(w * 0.85))
    r(140, 326, 200, 476, 30)                     # left ear pod
    r(624, 326, 684, 476, 30)                     # right ear pod
    r(196, 206, 628, 604, 140)                    # helmet (rounded square)
    if visor: r(250, 314, 574, 494, 90, width=w * 0.85)
    r(318, 362, 372, 446, 27, fill=eye_fill[0])   # eyes: tall pills
    r(452, 362, 506, 446, 27, fill=eye_fill[1])
    if shoulders:
        r(352, 604, 472, 660, 18, width=w * 0.85)     # neck
        r(172, 656, 652, 900, 120)                    # shoulders, clipped by the tile edge


def T(x, y, s=SCALE, cy0=375):
    return 412 + (x - 412) * s, 412 + (y - cy0) * s
def big_robot(d, w=38):
    def r(x0, y0, x1, y1, rad, fill=None, width=w):
        (a, b), (c, e) = T(x0, y0), T(x1, y1)
        if fill is None: d.rounded_rectangle((P(a), P(b), P(c), P(e)), radius=P(rad * SCALE), outline=WHITE, width=P(width * SCALE))
        else: d.rounded_rectangle((P(a), P(b), P(c), P(e)), radius=P(rad * SCALE), fill=fill)
    (ax, ay0), (_, ay1) = T(412, 206), T(412, 150)
    d.line((P(ax), P(ay0), P(ax), P(ay1)), fill=WHITE, width=P(w * SCALE))
    (bx0, by0), (bx1, by1) = T(382, 104), T(442, 164)
    d.ellipse((P(bx0), P(by0), P(bx1), P(by1)), outline=WHITE, width=P(w * 0.85 * SCALE))
    r(140, 326, 200, 476, 30); r(624, 326, 684, 476, 30)
    r(196, 206, 628, 604, 140)
    r(318, 362, 372, 446, 27, fill=BLUE); r(452, 362, 506, 446, 27, fill=PINK)
l = grad(*BRAND); big_robot(ImageDraw.Draw(l))
l.resize((1024, 1024), Image.LANCZOS).save(OUT)
print(f'Wrote {OUT}; run scripts/compose_macos_icon.swift on it to build the macOS icons.')
