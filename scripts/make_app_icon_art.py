"""Draws the UAM app icon artwork (chosen by David 2026-10-16): the original robot, simplified to flat shapes,
in a browser window whose outline is an inset copy of the icon tile, on a blue-to-orange gradient rim.
Output is flat square artwork; scripts/compose_macos_icon.swift clips it to the macOS icon grid.
Usage: python3 scripts/make_app_icon_art.py [assets/app_icon.png]"""
import math, sys
from PIL import Image, ImageDraw, ImageFilter
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
NAVY, BAR, WHITE, SHADE, VISOR = (29, 27, 51), (220, 221, 232), (248, 248, 252), (206, 208, 224), (23, 21, 39)
RED, TEAL, GREEN, BLUE, PINK, GLINT = (255, 95, 87), (38, 166, 154), (40, 200, 90), (56, 189, 248), (236, 72, 153), (90, 88, 118)
def hgrad(cols):
    im = Image.new('RGB', (len(cols), 1))
    for i, c in enumerate(cols): im.putpixel((i, 0), c)
    return im.resize((B, B), Image.BILINEAR).convert('RGBA')
def icon(scale, prompt, cy):
    l = hgrad([(37, 99, 235), (70, 60, 170), (214, 51, 108), (234, 120, 60)]); d = ImageDraw.Draw(l)
    INSET, BARH = 36, 92
    WX0, WY0, WX1, WY1 = INSET, INSET, 824 - INSET, 824 - INSET
    win = squircle(B - 2 * P(INSET))          # same superellipse as the tile, so the curves stay concentric
    full = Image.new('L', (B, B), 0); full.paste(win, (P(INSET), P(INSET)))
    l.paste(Image.new('RGBA', (B, B), NAVY + (255,)), (0, 0), full)
    bar = Image.new('L', (B, B), 0); ImageDraw.Draw(bar).rectangle((0, 0, B, P(WY0 + BARH)), fill=255)
    from PIL import ImageChops
    l.paste(Image.new('RGBA', (B, B), BAR + (255,)), (0, 0), ImageChops.multiply(full, bar))
    for x, c in ((150, RED), (210, TEAL), (270, GREEN)): d.ellipse((P(x - 21), P(WY0 + 50 - 21), P(x + 21), P(WY0 + 50 + 21)), fill=c)
    if prompt:
        d.line((P(108), P(WY0 + BARH + 50), P(152), P(WY0 + BARH + 80), P(108), P(WY0 + BARH + 110)), fill=GREEN, width=P(22), joint='curve')
    # head drawn around its own centre (412, 417) then scaled and placed at (412, cy)
    def T(x0, y0, x1, y1):
        return (P(412 + (x0 - 412) * scale), P(cy + (y0 - 417) * scale), P(412 + (x1 - 412) * scale), P(cy + (y1 - 417) * scale))
    for x0 in (188, 576): d.rounded_rectangle(T(x0, 352, x0 + 60, 486), radius=P(30 * scale), fill=SHADE)
    d.ellipse(T(218, 222, 606, 612), fill=WHITE)
    d.rounded_rectangle(T(256, 342, 568, 514), radius=P(86 * scale), fill=VISOR)
    d.rounded_rectangle(T(468, 360, 530, 374), radius=P(7 * scale), fill=GLINT)
    d.ellipse(T(314, 388, 378, 476), fill=BLUE)
    d.ellipse(T(446, 388, 510, 476), fill=PINK)
    return l
CONTENT_CY = (36 + 92 + 788) / 2 + 6   # centre of the dark area under the title bar

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'assets/app_icon.png'
    icon(scale=1.32, prompt=True, cy=CONTENT_CY).resize((1024, 1024), Image.LANCZOS).save(out)
    print(f'Wrote {out}; run scripts/compose_macos_icon.swift on it to build the macOS icons.')
