# Birdwatcher boot screen: asset handover

This bundle answers `BOOT_SCREEN_HANDOFF.md` §8 for the T-Display-S3 (320x170).

**Concept:** Birdwatcher. A rifle scope hunts across the scene and locks onto a
pigeon whose head is a Flock-style camera. A small flock of camera-birds sits on the
wire behind it. After the lock, the BIRD/WATCHER wordmark wipes in.
Runtime is 2.5 s, plus a 0.4 s hold.

## Files

| Path | Purpose |
|---|---|
| `firmware/bw_boot_anim.inc` | Defines `tdsBootAnim()`, using handoff options A (PNG decoded once into PSRAM sprites) and C (reticle, ring and HUD drawn with primitives) |
| `firmware/bw_scene_png.h` | 152x152 scope scene with a transparent outside, as a PROGMEM PNG with `_W`/`_H` constants |
| `firmware/bw_wordmark_png.h` | 140x71 wordmark with a transparent background, as a PROGMEM PNG |
| `art/scene.svg`, `assets/*.png` | Source art and the PNGs used to build the headers |
| `tools/gen_birdwatcher_assets.py` | Rebuilds the PNGs and headers from the SVG (needs Pillow, rsvg-convert and Poppins) |
| `tools/preview_boot.py` | Desktop preview of the same timeline. Font2 is approximated |
| `preview/` | Preview GIF (2x) and the final frame |

## Integration (about 5 lines)

1. Copy `firmware/*` next to `display_tdisplay_s3.cpp`.
2. In `display_tdisplay_s3.cpp`, add `#include "bw_boot_anim.inc"` **above `tdsInit()`**.
   It must come after `lcd`, `fb`, `fbOk`, `C_BG`, `C_GREEN` and `C_AMB` exist.
3. In `tdsInit()`, replace the static `FLOCK-YOU` splash block with `tdsBootAnim();`.
   Then either keep the `tdsBootLine()` progress (set `bootY` as before) or skip it.
4. Declare `void tdsBootAnim();` in `display_tdisplay_s3.h`.
5. In `main.cpp` `handleCommandLine()`, add
   `if (strstr(line, "\"boot_demo\"")) { tdsBootAnim(); return; }`.

## Palette

The animation uses the existing tokens `C_BG`, `C_GREEN` (scan line and SCAN text) and
`C_AMB` (target line). It adds four local colors in `bw::` that you can move into
`initColours()`: ring `#4B5563`, lock red `#EF4444`, reticle `#111827`, reticle-lock red.
The scene and wordmark colors are baked into the PNGs. The wordmark is white plus
amber `#FBBF24`, and both PNGs are alpha-blended onto `C_BG` at decode time.

## Notes and assumptions

- This has **not been compiled or run on hardware**; there was no access to the repo.
  It assumes the LovyanGFX 1.2.7 APIs `fillArc`, `drawPng(data,len,x,y)`,
  `LGFX_Sprite::pushSprite(dst,x,y)` and `lgfx::fonts::Font2`.
- If PSRAM sprite allocation fails, it falls back to `drawPng` every frame, which
  is slower but correct. With no framebuffer (`!fbOk`), it draws one static final frame to `lcd`.
- Detection, WiFi/BLE, persistence and RF behavior are untouched.
