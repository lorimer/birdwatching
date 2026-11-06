// ============================================================================
// Birdwatcher boot animation for the T-Display-S3 (320x170 landscape).
//
// Include this file inside display_tdisplay_s3.cpp, ABOVE tdsInit(), after
// `lcd`, `fb`, `fbOk` and the C_* colour tokens are declared:
//
//     #include "bw_boot_anim.inc"
//
// Timeline (2.5 s):
//   0.00-0.25 s  scope iris opens on the pigeon-cam scene
//   0.25-1.30 s  reticle hunts around, HUD types "SCAN WIFI+BLE"
//   1.30 s       lock on the camera lens: ring flashes red, "> FLOCK FALCON"
//   1.30-2.00 s  BIRD/WATCHER wordmark wipes in behind a green scan line
//   2.00-2.50 s  hold final frame ("LOCKED 98%" blinks)
//
// Assets: bw_scene_png.h / bw_wordmark_png.h (tools/gen_birdwatcher_assets.py).
// Each PNG is decoded ONCE into a PSRAM sprite, then blitted per frame.
// ============================================================================
#include "bw_scene_png.h"
#include "bw_wordmark_png.h"

namespace bw {
constexpr int CX = 84, CY = 85;                 // scope centre on panel
constexpr int SCENE = BW_SCENE_PNG_W;           // 152
constexpr int SCENE_R = 74;                     // visible scene radius
constexpr int RING_IN = 73, RING_OUT = 81;      // scope tube
constexpr int COL_X = 176, WM_Y = 16;           // right column / wordmark
constexpr int HUD_Y = 100, HUD_DY = 20;         // HUD text rows (Font2)
constexpr uint32_t T_IRIS = 250, T_LOCK = 1300, T_WIPE = 2000, DUR = 2500;

// Local palette. Move into initColours() if you want them as global tokens.
static uint16_t cRing, cRingLock, cReticle, cReticleLock;

inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline float easeOut(float x) { x = clamp01(x); float k = 1 - x; return 1 - k * k * k; }

// Reticle offset from the lens (0,0 once locked).
inline void reticleOffset(uint32_t t, int& ox, int& oy) {
  if (t >= T_LOCK) { ox = oy = 0; return; }
  float k = 1.0f - easeOut(t < T_IRIS ? 0.0f : (float)(t - T_IRIS) / (T_LOCK - T_IRIS));
  ox = (int)(40.0f * k * sinf(t * 0.009f));
  oy = (int)(28.0f * k * sinf(t * 0.013f + 1.0f));
}

void drawReticle(LovyanGFX* d, int ox, int oy, uint16_t col, uint16_t post) {
  const int R = RING_IN - 1, P = 18;
  int y = CY + oy, x = CX + ox;
  if (abs(oy) < R) {
    int hw = (int)sqrtf((float)(R * R - oy * oy));
    d->drawFastHLine(CX - hw, y, 2 * hw, col);
    d->fillRect(CX - hw, y - 1, P, 3, post);
    d->fillRect(CX + hw - P, y - 1, P, 3, post);
  }
  if (abs(ox) < R) {
    int hh = (int)sqrtf((float)(R * R - ox * ox));
    d->drawFastVLine(x, CY - hh, 2 * hh, col);
    d->fillRect(x - 1, CY - hh, 3, P, post);
    d->fillRect(x - 1, CY + hh - P, 3, P, post);
  }
  d->drawCircle(x, y, 7, col);
}

void typeLine(LovyanGFX* d, const char* s, int row, uint32_t t, uint32_t start, uint16_t col) {
  if (t < start) return;
  size_t n = (t - start) / 30 + 1, len = strlen(s);
  if (n > len) n = len;
  char buf[24];
  memcpy(buf, s, n); buf[n] = 0;
  d->setTextColor(col);
  d->drawString(buf, COL_X, HUD_Y + row * HUD_DY);
}

// Draw one frame at time t (ms). scene/wm may be null -> decode PNG directly
// (only used for the single static frame when there is no framebuffer).
void drawFrame(LovyanGFX* d, uint32_t t, LGFX_Sprite* scene, LGFX_Sprite* wm) {
  d->fillScreen(C_BG);

  // Scene + iris
  const int sx = CX - SCENE / 2, sy = CY - SCENE / 2;
  if (scene) scene->pushSprite(d, sx, sy);
  else d->drawPng(BW_SCENE_PNG, BW_SCENE_PNG_len, sx, sy);
  if (t < T_IRIS) {
    int r = (int)(SCENE_R * easeOut((float)t / T_IRIS));
    d->fillArc(CX, CY, r, SCENE_R + 4, 0, 360, C_BG);
  }

  // Reticle
  bool locked = t >= T_LOCK;
  int ox, oy; reticleOffset(t, ox, oy);
  drawReticle(d, ox, oy, locked ? cReticleLock : cReticle, cReticle);
  if (locked) d->fillCircle(CX, CY, 2, cReticleLock);

  // Scope tube (flashes red on lock)
  bool flash = locked && (t - T_LOCK) < 240 && ((t - T_LOCK) / 60) % 2 == 0;
  uint16_t ring = flash ? cRingLock : cRing;
  d->fillArc(CX, CY, RING_IN, RING_OUT, 0, 360, ring);
  d->fillRect(CX - 10, 0, 20, CY - RING_OUT + 2, ring);           // top turret
  d->fillRect(CX + RING_OUT - 2, CY - 10, 8, 20, ring);           // side turret

  // Wordmark wipe
  if (locked) {
    const int W = BW_WORDMARK_PNG_W, H = BW_WORDMARK_PNG_H;
    int w = (int)(W * easeOut((float)(t - T_LOCK) / (T_WIPE - T_LOCK)));
    if (wm) wm->pushSprite(d, COL_X, WM_Y);
    else d->drawPng(BW_WORDMARK_PNG, BW_WORDMARK_PNG_len, COL_X, WM_Y);
    if (w < W) {
      d->fillRect(COL_X + w, WM_Y, W - w + 1, H, C_BG);
      d->drawFastVLine(COL_X + w, WM_Y - 3, H + 6, C_GREEN);
    }
  }

  // HUD
  d->setFont(&lgfx::fonts::Font2);
  d->setTextDatum(lgfx::textdatum_t::top_left);
  typeLine(d, "SCAN WIFI+BLE", 0, t, T_IRIS, C_GREEN);
  typeLine(d, "> FLOCK FALCON", 1, t, T_LOCK, C_AMB);
  bool blinkOn = ((t - T_WIPE) / 160) % 2 == 0;
  if (t >= DUR || (t >= T_WIPE && blinkOn))
    typeLine(d, "LOCKED  98%", 2, t, T_WIPE, cRingLock);
}
}  // namespace bw

void tdsBootAnim() {
  bw::cRing        = lcd.color565(75, 85, 99);
  bw::cRingLock    = lcd.color565(239, 68, 68);
  bw::cReticle     = lcd.color565(17, 24, 39);
  bw::cReticleLock = lcd.color565(239, 68, 68);

  if (!fbOk) {                       // no framebuffer: one static final frame
    bw::drawFrame(&lcd, bw::DUR, nullptr, nullptr);
    return;
  }

  LGFX_Sprite scene(&lcd), wm(&lcd);
  bool ok = true;
  for (LGFX_Sprite* s : {&scene, &wm}) { s->setPsram(true); s->setColorDepth(16); }
  ok &= scene.createSprite(BW_SCENE_PNG_W, BW_SCENE_PNG_H) != nullptr;
  ok &= wm.createSprite(BW_WORDMARK_PNG_W, BW_WORDMARK_PNG_H) != nullptr;
  if (ok) {
    scene.fillScreen(C_BG); scene.drawPng(BW_SCENE_PNG, BW_SCENE_PNG_len, 0, 0);
    wm.fillScreen(C_BG);    wm.drawPng(BW_WORDMARK_PNG, BW_WORDMARK_PNG_len, 0, 0);
  }

  uint32_t t0 = millis(), t;
  while ((t = millis() - t0) < bw::DUR) {
    bw::drawFrame(&fb, t, ok ? &scene : nullptr, ok ? &wm : nullptr);
    fb.pushSprite(0, 0);
    delay(16);
  }
  bw::drawFrame(&fb, bw::DUR, ok ? &scene : nullptr, ok ? &wm : nullptr);
  fb.pushSprite(0, 0);
  delay(400);                         // hold the brand frame

  scene.deleteSprite(); wm.deleteSprite();
  // Leave the final brand frame on screen; the live UI repaints on the first tick.
}
