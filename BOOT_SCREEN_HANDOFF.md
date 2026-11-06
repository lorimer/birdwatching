# Boot screen handoff — build a new boot logo/animation

Written for: a separate coding session (or developer) working in this repo to turn
supplied iconography into the T-Display-S3 boot screen. You have the iconography;
these are the instructions to combine it into the firmware. Nothing here changes
detection logic — do not touch WiFi/BLE/skimmer/Flock code or persistence.

## 1. Short answer on animation

Yes, the board animates easily. It is an ESP32-S3 (dual core, 240 MHz) with 8 MB
PSRAM driving an ST7789 over an 8-bit parallel bus. The firmware already keeps a
full-screen 16-bit sprite in PSRAM and blits it each frame, so a 2 to 4 second
boot animation at 30 to 60 fps for vector/shape art is comfortable. A decoded PNG
frame sequence also works if you pre-decode the frames once (PNG decode is tens of
ms each, too slow to do live every frame). Budget: the panel is 320x170, one
16-bit full-screen frame is 320*170*2 = 108,800 bytes, trivial against 8 MB PSRAM.

## 2. The display, in facts you need

File: `display_tdisplay_s3.cpp` (the whole UI). Library: LovyanGFX 1.2.7, pinned in
`platformio.ini` under `[env:lilygo_t_display_s3]`.

- Panel is used landscape: `#define TDS_W 320`, `#define TDS_H 170`.
- Two draw targets exist already:
  - `lcd` — the live panel (`LGFX_TDisplayS3`).
  - `fb` — an `LGFX_Sprite` full-screen framebuffer in PSRAM; `g` points at `fb`
    when `fbOk` is true, else at `lcd`. Normal UI draws into `g` then calls
    `fb.pushSprite(0, 0)`.
- 16-bit color. Build colors with `lcd.color565(r,g,b)`. Theme tokens already
  defined in `initColours()` (e.g. `C_BG`, `C_GREEN`, `C_AMB`, `C_SUB`, `C_TXT`,
  `C_TIER[0..4]`). Reuse these so the logo matches the chosen concept's palette.
- Fonts available without extra assets: `lgfx::fonts::Font0/Font2/Font4`,
  `FreeSansBold18pt7b`. For the concept wordmarks a bold sans is fine; a custom
  display font can be added as an `lgfx::GFXfont` header if wanted.

## 3. Where the boot screen lives

Function `tdsInit()` in `display_tdisplay_s3.cpp` (around line 1442). Right after
the framebuffer is created it draws the current text splash straight to `lcd`:

```
    lcd.drawString("FLOCK-YOU", 10, 8);
    ...
    lcd.drawString("passive ALPR detector  -  T-Display-S3", 10, 48);
    bootY = 72;
```

`tdsBootLine()` then appends setup progress lines under it, and `bootDone` flips
true on the first `tdsTick()`. Replace the static splash block with a call to a new
`tdsBootAnim()`. Boot runs linearly in `setup()`, so a self-contained blocking
animation loop here is acceptable — keep it to ~2.5 s max so the device is usable
quickly. After the animation either clear and resume `tdsBootLine()` progress, or
skip the text lines entirely and go straight to the live UI.

## 4. Two ways to bring the iconography in

Pick per asset. Vector for the primitive-drawn concepts (CROW'S NEST radar rings,
crosshair, sweep); raster for anything hand-drawn.

### A. Runtime PNG decode (simplest for arbitrary art)
Embed the PNG bytes as a `const uint8_t[] PROGMEM` and draw with LovyanGFX:

```
g->drawPng(LOGO_PNG, sizeof(LOGO_PNG), x, y);          // whole panel or an icon
```

`drawPng` honors alpha, so export the icon as a PNG with a transparent background.
For animation, decode each frame once into its own small `LGFX_Sprite` in PSRAM at
boot, then `frame.pushSprite(...)` per tick. Do not `drawPng` every frame.

Convert a file to a header:
```
xxd -i crow.png > crow_png.h   # yields crow_png[] and crow_png_len
```

### B. Pre-converted RGB565 array (fastest blit, no decoder)
Convert PNG to a raw 16-bit array plus a transparent color key, embed as PROGMEM,
and blit with `g->pushImage(x, y, w, h, DATA, TRANSPARENT_KEY)`. A tiny Python
converter belongs in `tools/` (mirror the style of `tools/gen_oui_vendors.py`):
read with Pillow, pack `((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)` big-endian, emit a
`static const uint16_t NAME[w*h] PROGMEM`. Use this when A is too slow or the art
is small and reused many times.

### C. Vector / primitives (no asset file)
Redraw with `fillCircle/drawArc/fillTriangle/drawLine/fillRect` straight into `g`,
exactly as the concept panels were mocked. Best for radar rings, sweeps,
crosshairs, and the sweep-angle animation.

## 5. Animation pattern (drop-in skeleton)

```c
void tdsBootAnim() {
  if (!fbOk) { /* draw one static frame to lcd and return */ return; }
  const uint32_t DUR = 2500;                 // ms
  uint32_t t0 = millis(), t;
  while ((t = millis() - t0) < DUR) {
    float p = (float)t / DUR;                 // 0..1 progress
    fb.fillScreen(C_BG);
    // --- draw frame at progress p into `fb`/`g` here ---
    // e.g. radar sweep angle = p*TWO_PI; fade wordmark in with alpha;
    //      pushImage/drawPng/pushSprite your prepared icon frames.
    fb.pushSprite(0, 0);
    delay(16);                                // ~60 fps cap
  }
  fb.fillScreen(C_BG); fb.pushSprite(0, 0);
}
```

Timing cues that read well: fade/scale the mark in over the first ~60%, let the
radar sweep or crows resolve, hold the final frame ~400 ms, then hand off. Keep the
final held frame as the "brand" state in case you also want it as an idle header.

## 6. Add a serial replay command (so you don't reboot to test)

In `main.cpp`, `handleCommandLine()` (around line 1400) follows a
`if (strstr(line, "\"ui_demo\"")) { ... }` pattern. Add:

```
if (strstr(line, "\"boot_demo\"")) { tdsBootAnim(); return; }
```

Declare `tdsBootAnim()` in `display_tdisplay_s3.h` next to `tdsInit`. Then over the
USB serial monitor send `{"cmd":"boot_demo"}` to replay the animation on demand.

## 7. Build and flash

```
~/.venvs/pio/bin/pio run -e lilygo_t_display_s3 -t upload
~/.venvs/pio/bin/pio device monitor -b 115200
```

USB-JTAG quirks seen on this board: on reset it drops off USB for ~30-45 s — wait
for re-enumeration before uploading again; if upload reports "port is busy" or an
I/O error, retry after it re-enumerates. These do not indicate a code fault.

## 8. Asset handover checklist (what to supply alongside this file)

- Final concept choice: name + artwork + color (mixing across the four mockups is
  fine). The palette should map to existing `color565` tokens or add new ones in
  `initColours()`.
- Each icon as a transparent-background PNG, sized at or under 320x170 (full-panel
  art exactly 320x170). If it is an animated sequence, supply numbered frames.
- Any custom display font as a ready `lgfx::GFXfont` header, or name a Google/F
  free font to convert.

## 9. Acceptance

- Boot shows the new logo/animation within ~0.5 s of power-on, finishes in ~2.5 s,
  lands on the live RADAR UI.
- `{"cmd":"boot_demo"}` replays it without a reboot.
- No regression: detection, counters, persistence, pages, and brightness behave
  exactly as before. Do not add deauth/packet-injection or any other new RF
  transmit behavior — out of scope and previously declined.
