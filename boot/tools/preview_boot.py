#!/usr/bin/env python3
"""Desktop preview of bw_boot_anim.inc (same layout/timeline). Font2 is approximated."""
import math, sys, os
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "out"
CX, CY, SCENE_R, RING_IN, RING_OUT = 84, 85, 74, 73, 81
COL_X, WM_Y, HUD_Y, HUD_DY = 176, 16, 100, 20
T_IRIS, T_LOCK, T_WIPE, DUR = 250, 1300, 2000, 2500
BG, GREEN, AMB = (0, 0, 0), (34, 197, 94), (251, 191, 36)
RING, RED, RET = (75, 85, 99), (239, 68, 68), (17, 24, 39)
FONT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 12)

scene = Image.open(os.path.join(OUT, "bw_scene.png"))
wm = Image.open(os.path.join(OUT, "bw_wordmark.png"))

def ease(x):
    x = min(max(x, 0), 1); return 1 - (1 - x) ** 3

def annulus(d, r0, r1, col):
    im = Image.new("L", (320, 170), 0); m = ImageDraw.Draw(im)
    m.ellipse([CX - r1, CY - r1, CX + r1, CY + r1], fill=255)
    if r0 > 0: m.ellipse([CX - r0, CY - r0, CX + r0, CY + r0], fill=0)
    d.paste(Image.new("RGB", (320, 170), col), (0, 0), im)

def type_line(dr, s, row, t, start, col):
    if t < start: return
    n = min(len(s), (t - start) // 30 + 1)
    dr.text((COL_X, HUD_Y + row * HUD_DY + 2), s[:n], font=FONT, fill=col)

def frame(t):
    img = Image.new("RGB", (320, 170), BG)
    img.paste(scene, (CX - 76, CY - 76), scene)
    if t < T_IRIS:
        annulus(img, int(SCENE_R * ease(t / T_IRIS)), SCENE_R + 4, BG)
    d = ImageDraw.Draw(img)
    locked = t >= T_LOCK
    if locked: ox = oy = 0
    else:
        k = 1 - ease(0 if t < T_IRIS else (t - T_IRIS) / (T_LOCK - T_IRIS))
        ox = int(40 * k * math.sin(t * 0.009)); oy = int(28 * k * math.sin(t * 0.013 + 1))
    col = RED if locked else RET; R = RING_IN - 1
    if abs(oy) < R:
        hw = int(math.sqrt(R * R - oy * oy)); y = CY + oy
        d.line([CX - hw, y, CX + hw, y], fill=col)
        d.rectangle([CX - hw, y - 1, CX - hw + 17, y + 1], fill=RET)
        d.rectangle([CX + hw - 18, y - 1, CX + hw - 1, y + 1], fill=RET)
    if abs(ox) < R:
        hh = int(math.sqrt(R * R - ox * ox)); x = CX + ox
        d.line([x, CY - hh, x, CY + hh], fill=col)
        d.rectangle([x - 1, CY - hh, x + 1, CY - hh + 17], fill=RET)
        d.rectangle([x - 1, CY + hh - 18, x + 1, CY + hh - 1], fill=RET)
    d.ellipse([CX + ox - 7, CY + oy - 7, CX + ox + 7, CY + oy + 7], outline=col)
    if locked: d.ellipse([CX - 2, CY - 2, CX + 2, CY + 2], fill=RED)
    flash = locked and (t - T_LOCK) < 240 and ((t - T_LOCK) // 60) % 2 == 0
    ring = RED if flash else RING
    annulus(img, RING_IN, RING_OUT, ring); d = ImageDraw.Draw(img)
    d.rectangle([CX - 10, 0, CX + 9, CY - RING_OUT + 1], fill=ring)
    d.rectangle([CX + RING_OUT - 2, CY - 10, CX + RING_OUT + 5, CY + 9], fill=ring)
    if locked:
        W, H = wm.size; w = int(W * ease((t - T_LOCK) / (T_WIPE - T_LOCK)))
        img.paste(wm, (COL_X, WM_Y), wm); d = ImageDraw.Draw(img)
        if w < W:
            d.rectangle([COL_X + w, WM_Y, COL_X + W, WM_Y + H - 1], fill=BG)
            d.line([COL_X + w, WM_Y - 3, COL_X + w, WM_Y + H + 2], fill=GREEN)
    type_line(d, "SCAN WIFI+BLE", 0, t, T_IRIS, GREEN)
    type_line(d, "> FLOCK FALCON", 1, t, T_LOCK, AMB)
    if t >= DUR or (t >= T_WIPE and ((t - T_WIPE) // 160) % 2 == 0):
        type_line(d, "LOCKED  98%", 2, t, T_WIPE, RED)
    return img

frames = [frame(t) for t in range(0, DUR, 33)] + [frame(DUR)] * 25
big = [f.resize((640, 340), Image.NEAREST) for f in frames]
big[0].save(os.path.join(OUT, "bw_boot_preview.gif"), save_all=True,
            append_images=big[1:], duration=33, loop=0)
frame(DUR).save(os.path.join(OUT, "bw_boot_final_320x170.png"))
frame(DUR).resize((960, 510), Image.NEAREST).save(os.path.join(OUT, "bw_boot_final_3x.png"))
print(len(frames), "frames")
