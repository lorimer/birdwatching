// LilyGO T-Display-S3 (touch) front end for flock-you.
//
//   Pages (swipe left/right, tap the header, or short-press KEY):
//     LIVE   radar of devices heard in the last 20 s + strongest target
//     LIST   every unique detection this session, newest first (drag to
//            scroll, tap a row for details)
//     STATS  uptime, frame rate, per-tier counts, battery, settings buttons
//
//   A new MAC (or a confidence upgrade) pops a full-screen alert in the
//   tier's colour for 6 s. Tap anywhere to dismiss it early.
//
//   Buttons:  KEY  (GPIO14)  short = next page     long = brightness
//             BOOT (GPIO0)   short = prev page /   long = screen off/on
//                                    scroll list
//   The screen wakes on its own for any tier 2+ detection.

#ifdef BOARD_LILYGO_T_DISPLAY_S3

#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "display_tdisplay_s3.h"
#include "oui_vendors.h"

// ---------------------------------------------------------------- pins
#define TDS_PIN_LCD_POWER 15   // must be HIGH for the panel on battery
#define TDS_PIN_BL        38
#define TDS_PIN_BTN_BOOT   0
#define TDS_PIN_BTN_KEY   14
#define TDS_PIN_BAT_ADC    4   // 1:2 divider
#define TDS_PIN_TP_SDA    18
#define TDS_PIN_TP_SCL    17
#define TDS_PIN_TP_INT    16
#define TDS_PIN_TP_RST    21

// ---------------------------------------------------------------- tuning
#define TDS_W              320
#define TDS_H              170
#define TDS_HDR_H           20
#define TDS_FRAME_MS        80     // ~12 fps redraw
#define TDS_ACTIVE_MS    20000     // "in range" window for the radar
#define TDS_ALERT_MS      6000     // full-screen alert duration
#define TDS_FLASH_MS      1800     // alert background flashes this long
#define TDS_LONGPRESS_MS   700
#define TDS_ROW_H           18
#define TDS_LIST_Y0         38

// ---------------------------------------------------------------- driver
class LGFX_TDisplayS3 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_CST816S _touch;
public:
  LGFX_TDisplayS3() {
    { auto c = _bus.config();
      c.freq_write = 20000000;
      c.pin_wr = 8;  c.pin_rd = 9;  c.pin_rs = 7;
      c.pin_d0 = 39; c.pin_d1 = 40; c.pin_d2 = 41; c.pin_d3 = 42;
      c.pin_d4 = 45; c.pin_d5 = 46; c.pin_d6 = 47; c.pin_d7 = 48;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = 6; c.pin_rst = 5; c.pin_busy = -1;
      c.panel_width = 170; c.panel_height = 320;
      c.offset_x = 35; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = TDS_PIN_BL; c.invert = false; c.freq = 22000; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    { auto c = _touch.config();
      c.x_min = 0; c.x_max = 169; c.y_min = 0; c.y_max = 319;
      c.pin_int = TDS_PIN_TP_INT; c.pin_rst = TDS_PIN_TP_RST;
      c.bus_shared = false; c.offset_rotation = 0;
      c.i2c_port = 0; c.i2c_addr = 0x15;
      c.pin_sda = TDS_PIN_TP_SDA; c.pin_scl = TDS_PIN_TP_SCL; c.freq = 400000;
      _touch.config(c); _panel.setTouch(&_touch); }
    setPanel(&_panel);
  }
};

static LGFX_TDisplayS3 lcd;
static LGFX_Sprite     fb(&lcd);
static lgfx::LovyanGFX* g = &lcd;   // draw target: sprite if it allocated
static bool            fbOk = false;

// ---------------------------------------------------------------- colours
static uint16_t C_BG, C_HDR, C_DIM, C_GRID, C_TXT, C_SUB, C_GREEN, C_GREEN_D;
static uint16_t C_TIER[5];
static uint16_t C_AMB, C_AMB_HI;       // non-Flock WiFi
static uint16_t C_CAT[16];             // device-type category tints
static uint16_t C_BLE, C_BLE_HI;       // Bluetooth accents

static const char* const TIER_BIG[5]   = { "SSID MATCH", "OUI ECHO", "OUI MATCH",
                                           "LIKELY FLOCK", "FLOCK CONFIRMED" };
static const char* const TIER_WHY[5]   = { "SSID keyword", "addr1/addr3 echo - low confidence",
                                           "transmitter OUI match", "wildcard probe + Flock OUI",
                                           "IE fingerprint + OUI + probe" };
static const char* const TIER_SHORT[5] = { "SSID", "ECHO", "OUI", "PROBE", "CONF" };

static void initColours() {
  C_BG      = lcd.color565(6, 10, 14);
  C_HDR     = lcd.color565(18, 28, 40);
  C_DIM     = lcd.color565(70, 80, 90);
  C_GRID    = lcd.color565(0, 70, 40);
  C_TXT     = lcd.color565(230, 235, 240);
  C_SUB     = lcd.color565(140, 150, 160);
  C_GREEN   = lcd.color565(0, 235, 120);
  C_GREEN_D = lcd.color565(0, 110, 60);
  C_AMB     = lcd.color565(95, 100, 105);
  C_AMB_HI  = lcd.color565(185, 190, 195);
  // by category index (OuiCat order): net,apple,samsung,google,amazon,iot,
  // cam,printer,tv,audio,pc,phone,auto,other
  C_CAT[OUICAT_NET]     = lcd.color565(120, 180, 255);
  C_CAT[OUICAT_APPLE]   = lcd.color565(230, 230, 235);
  C_CAT[OUICAT_SAMSUNG] = lcd.color565(90, 160, 255);
  C_CAT[OUICAT_GOOGLE]  = lcd.color565(240, 180, 70);
  C_CAT[OUICAT_AMAZON]  = lcd.color565(255, 170, 60);
  C_CAT[OUICAT_IOT]     = lcd.color565(180, 140, 255);
  C_CAT[OUICAT_CAM]     = lcd.color565(255, 120, 120);
  C_CAT[OUICAT_PRINTER] = lcd.color565(150, 200, 170);
  C_CAT[OUICAT_TV]      = lcd.color565(120, 220, 220);
  C_CAT[OUICAT_AUDIO]   = lcd.color565(200, 160, 220);
  C_CAT[OUICAT_PC]      = lcd.color565(160, 200, 255);
  C_CAT[OUICAT_PHONE]   = lcd.color565(120, 230, 160);
  C_CAT[OUICAT_AUTO]    = lcd.color565(230, 200, 120);
  C_CAT[OUICAT_OTHER]   = lcd.color565(170, 175, 180);
  C_BLE     = lcd.color565(80, 130, 210);
  C_BLE_HI  = lcd.color565(130, 185, 255);
  C_TIER[0] = lcd.color565(150, 150, 150);
  C_TIER[1] = lcd.color565(0, 200, 255);
  C_TIER[2] = lcd.color565(255, 215, 0);
  C_TIER[3] = lcd.color565(255, 130, 0);
  C_TIER[4] = lcd.color565(255, 35, 35);
}
static inline uint16_t tierCol(uint8_t t) { return C_TIER[t < 5 ? t : 0]; }

static uint16_t dimCol(uint16_t c, uint8_t num, uint8_t den) {
  uint8_t r = ((c >> 11) & 0x1F) * num / den;
  uint8_t gg = ((c >> 5) & 0x3F) * num / den;
  uint8_t b = (c & 0x1F) * num / den;
  return (r << 11) | (gg << 5) | b;
}

// ---------------------------------------------------------------- OUI lookup
// Binary search over the flash-resident IEEE registry (oui_vendors.h).
// Returns the vendor short-name (points into flash) and sets *cat to the
// inferred device-type category. A randomized/locally-administered MAC can't
// be attributed, so those return nullptr.
static char ouiNameBuf[24];
static const char* ouiLookup(const uint8_t* mac, uint8_t* cat) {
  if (cat) *cat = OUICAT_OTHER;
  if (mac[0] & 0x02) return nullptr;                 // locally-administered (random)
  uint32_t key = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
  int lo = 0, hi = OUI_DB_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint32_t k = pgm_read_dword(&OUI_DB_KEY[mid]);
    if (k < key) lo = mid + 1;
    else if (k > key) hi = mid - 1;
    else {
      uint32_t v = pgm_read_dword(&OUI_DB_VAL[mid]);
      if (cat) *cat = (uint8_t)(v >> 24);
      const char* src = &OUI_DB_NAMES[v & 0x00FFFFFF];
      for (int i = 0; i < 23; i++) {
        char c = pgm_read_byte(src + i);
        ouiNameBuf[i] = c;
        if (!c) break;
      }
      ouiNameBuf[23] = '\0';
      return ouiNameBuf;
    }
  }
  return nullptr;
}
// Short category label for the inferred device type (flash string).
static void ouiCatLabel(uint8_t cat, char* buf, size_t n) {
  if (cat >= (uint8_t)(sizeof(OUI_CAT_LABEL)/sizeof(OUI_CAT_LABEL[0]))) cat = OUICAT_OTHER;
  const char* p = (const char*)pgm_read_ptr(&OUI_CAT_LABEL[cat]);
  strncpy_P(buf, p, n - 1);
  buf[n - 1] = '\0';
}

// ---------------------------------------------------------------- BLE id
// Manufacturer company ID -> vendor, for the common consumer assignments.
// Unknown IDs fall back to a hex label, which is honest about the gap.
static const char* bleVendor(uint16_t cid) {
  switch (cid) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x00E0: return "Google";
    case 0x0075: return "Samsung";
    case 0x0059: return "Nordic";
    case 0x02E5: return "Espressif";
    case 0x0171: return "Amazon";
    case 0x038F: return "Xiaomi";
    case 0x0157: return "Huami";
    case 0x0087: return "Garmin";
    case 0x009E: return "Bose";
    case 0x012D: return "Sony";
    case 0x0499: return "Ruuvi";
    case 0x0310: return "SGL Italia";
    case 0x0A12: return "Pebblebee";
    case 0xFFFF: return nullptr;
    default:     return nullptr;
  }
}

// GAP appearance -> device type. Category is the top 10 bits; a few HID
// subcategories are split out. This is the device's own self-description.
static const char* bleAppearance(uint16_t app) {
  if (!app) return nullptr;
  uint16_t cat = app >> 6;
  switch (cat) {
    case 1:  return "Phone";
    case 2:  return "Computer";
    case 3:  return "Watch";
    case 4:  return "Clock";
    case 5:  return "Display";
    case 6:  return "Remote";
    case 7:  return "Glasses";
    case 8:  return "Tag";
    case 9:  return "Keyring";
    case 10: return "Media Player";
    case 11: return "Barcode";
    case 12: return "Thermometer";
    case 13: return "Heart Rate";
    case 14: return "Blood Press";
    case 15:
      switch (app) {
        case 0x03C1: return "Keyboard";
        case 0x03C2: return "Mouse";
        case 0x03C3: return "Joystick";
        case 0x03C4: return "Gamepad";
        case 0x03C5: return "Tablet";
        default:     return "HID";
      }
    case 16: return "Glucose";
    case 17: return "Run Sensor";
    case 18: return "Cycling";
    case 20: return "Network";
    case 21: return "Sensor";
    case 37: return "Audio";
    case 49: return "Pulse Ox";
    default: return nullptr;
  }
}

// Best-effort type label from appearance, then name keywords. Writes buf.
static void bleType(const TdsBleView& b, char* buf, size_t n) {
  const char* t = bleAppearance(b.appearance);
  if (t) { strlcpy(buf, t, n); return; }
  // name keyword fallback
  struct { const char* kw; const char* lbl; } K[] = {
    {"watch","Watch"}, {"band","Band"}, {"buds","Earbuds"}, {"pods","Earbuds"},
    {"headphone","Headphones"}, {"headset","Headset"}, {"speaker","Speaker"},
    {"\x5btv\x5d","TV"}, {"tv","TV"}, {"mouse","Mouse"}, {"keyboard","Keyboard"},
    {"iphone","Phone"}, {"galaxy","Phone"}, {"pixel","Phone"}, {"macbook","Computer"},
    {"beacon","Beacon"}, {"tile","Tracker"}, {"airtag","Tracker"}, {"tracker","Tracker"},
    {"sensor","Sensor"}, {"light","Light"}, {"bulb","Light"}, {"lock","Lock"},
    {"scale","Scale"}, {"printer","Printer"}, {"camera","Camera"},
  };
  char low[24]; int i = 0;
  for (; b.name[i] && i < 23; i++) low[i] = tolower((unsigned char)b.name[i]);
  low[i] = '\0';
  for (auto& k : K) if (strstr(low, k.kw)) { strlcpy(buf, k.lbl, n); return; }
  strlcpy(buf, "BLE device", n);
}

// ---------------------------------------------------------------- state
enum { PAGE_RADAR = 0, PAGE_FLOCK, PAGE_AP, PAGE_STA, PAGE_BLE, PAGE_SSID, PAGE_STATS, PAGE_COUNT };
static const char* const PAGE_NAME[PAGE_COUNT] =
  { "RADAR", "FLOCK", "AP", "STA", "BLE", "SSID", "STATS" };
static inline bool isListPage(uint8_t p) {
  return p == PAGE_FLOCK || p == PAGE_AP || p == PAGE_STA || p == PAGE_BLE || p == PAGE_SSID;
}

static const uint8_t BRIGHT_LVL[] = { 255, 150, 70, 25 };
#define BRIGHT_N (sizeof(BRIGHT_LVL) / sizeof(BRIGHT_LVL[0]))

static Preferences prefs;
static uint8_t  page = PAGE_RADAR;
static uint8_t  brightIdx = 0;
static bool     usbFullBright = true;   // force full brightness while on USB power
static bool     onUsb = false;          // best-effort "externally powered" guess
static uint8_t  radarMode = 0;          // RADAR summary: 0 now, 1 since-reset, 2 all-time
static uint8_t  pendingReset = 0;       // confirm dialog: 0 none, 1 session, 2 factory, 3 reboot
static uint8_t  rotation = 1;
static bool     screenOn = true;
static bool     bootDone = false;
static int      bootY = 0;
static uint8_t  curCh = 1;
static unsigned long lastFrame = 0;

static struct {
  bool active;
  unsigned long start;
  char mac[18];
  char method[40];
  char name[33];
  uint8_t tier;
  int8_t rssi;
  uint8_t ch;
  int count;
  bool skimmer;       // this alert is a BLE skimmer suspect, not a Flock hit
  uint8_t skimScore;  // 1 raw serial service, 2 known module name
} alert;

static int  pageScroll[PAGE_COUNT] = {0};   // vertical scroll, per list page
static int  detailIdx = -1;                 // fyDet index on FLOCK detail, -1 none
static bool ambDetail = false;              // AP/STA/BLE detail card open
static uint8_t ambDetailMac[6];             // which device (survives re-sort)

static float    batMv = 0;
static unsigned long batAt = 0;
static uint32_t fpsLastFrames = 0;
static unsigned long fpsAt = 0;
static uint32_t framesPerSec = 0;

// ---------------------------------------------------------------- helpers
static void fmtAge(uint32_t ms, char* buf, size_t n) {
  uint32_t s = ms / 1000;
  if (s < 60)        snprintf(buf, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(buf, n, "%lum", (unsigned long)(s / 60));
  else               snprintf(buf, n, "%luh", (unsigned long)(s / 3600));
}

static uint32_t macHash(const char* m) {
  uint32_t h = 2166136261u;
  while (*m) { h ^= (uint8_t)*m++; h *= 16777619u; }
  return h;
}

static uint32_t macHash6(const uint8_t* m) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; i++) { h ^= m[i]; h *= 16777619u; }
  return h;
}

static void macStr6(const uint8_t* m, char* buf, size_t n) {
  snprintf(buf, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

#define TDS_AMB_LIST_MS 60000   // grey rows on LIST: heard in the last minute

static bool isBleMethod(const char* m) { return m && strncmp(m, "ble", 3) == 0; }

// 0..4 bars from RSSI
static int rssiBars(int r) {
  if (r >= -55) return 4;
  if (r >= -67) return 3;
  if (r >= -78) return 2;
  if (r >= -88) return 1;
  return 0;
}

static void drawBars(int x, int y, int rssi, uint16_t col) {
  int n = rssiBars(rssi);
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    g->fillRect(x + i * 5, y + 13 - h, 4, h, i < n ? col : C_DIM);
  }
}

static void applyBrightness() {
  uint8_t lvl = BRIGHT_LVL[brightIdx];
  if (usbFullBright && onUsb) lvl = 255;     // pop to full when plugged in
  lcd.setBrightness(screenOn ? lvl : 0);
}

static void setScreen(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  applyBrightness();
  lastFrame = 0;
}

static void readBattery(unsigned long now) {
  if (batAt && now - batAt < 2000) return;
  batAt = now;
  float mv = analogReadMilliVolts(TDS_PIN_BAT_ADC) * 2.0f;
  batMv = (batMv == 0) ? mv : batMv * 0.7f + mv * 0.3f;
  // Best-effort external-power guess from the battery rail: charging/USB sits
  // above a resting cell, and a board with no battery reads low. Not exact.
  bool usbNow = (batMv > 4300.0f) || (batMv < 2600.0f);
  if (usbNow != onUsb) { onUsb = usbNow; applyBrightness(); }
}

// Ambient WiFi for the AP / STA pages. apMode=true keeps access points,
// false keeps client stations. Sorted strongest-first.
static int ambSorted[128];
static int buildAmb(bool apMode, unsigned long now, uint32_t windowMs) {
  int n = 0, total = fyUiAmbCount();
  TdsAmbView a, b;
  for (int i = 0; i < total && n < 128; i++) {
    if (!fyUiGetAmb(i, &a)) continue;
    if (!a.prev && now - a.lastSeen > windowMs) continue;   // prev kept until heard live
    if ((bool)a.ap != apMode) continue;
    int j = n++;
    while (j > 0) {
      fyUiGetAmb(ambSorted[j - 1], &b);
      bool bBetter = (b.prev < a.prev) || (b.prev == a.prev && b.rssi >= a.rssi);
      if (bBetter) break;
      ambSorted[j] = ambSorted[j - 1];
      j--;
    }
    ambSorted[j] = i;
  }
  return n;
}

// Ambient BLE for the BLE page, strongest-first, live before previous-session.
static int bleSorted[128];
static int buildBle(unsigned long now, uint32_t windowMs) {
  int n = 0, total = fyUiBleCount();
  TdsBleView a, b;
  for (int i = 0; i < total && n < 128; i++) {
    if (!fyUiGetBle(i, &a)) continue;
    if (!a.prev && now - a.lastSeen > windowMs) continue;
    int j = n++;
    while (j > 0) {
      fyUiGetBle(bleSorted[j - 1], &b);
      bool bBetter = (b.prev < a.prev) ||
                     (b.prev == a.prev && b.skimmer > a.skimmer) ||
                     (b.prev == a.prev && b.skimmer == a.skimmer && b.rssi >= a.rssi);
      if (bBetter) break;
      bleSorted[j] = bleSorted[j - 1];
      j--;
    }
    bleSorted[j] = i;
  }
  return n;
}

// Sorted (newest first) view of the detection table.
static int sortedIdx[256];
static int buildSorted() {
  int n = fyUiDetCount();
  if (n > 256) n = 256;
  TdsDetView a, b;
  for (int i = 0; i < n; i++) {
    int j = i;
    fyUiGetDet(i, &a);
    while (j > 0) {
      fyUiGetDet(sortedIdx[j - 1], &b);
      if (b.lastSeen >= a.lastSeen) break;
      sortedIdx[j] = sortedIdx[j - 1];
      j--;
    }
    sortedIdx[j] = i;
  }
  return n;
}

// ---------------------------------------------------------------- header
static inline uint16_t catCol(uint8_t cat) {
  return C_CAT[cat < 16 ? cat : OUICAT_OTHER];
}

// Tab geometry: tabs fill a fixed strip left of the channel/battery cluster,
// sized to however many pages exist.
#define TDS_TAB_X0   2
#define TDS_TAB_AREA 238
static inline int tabW()          { return TDS_TAB_AREA / PAGE_COUNT; }
static inline int tabCenter(int i) { return TDS_TAB_X0 + tabW() * i + tabW() / 2; }

static void drawHeader(unsigned long now) {
  g->fillRect(0, 0, TDS_W, TDS_HDR_H, C_HDR);

  // tabs
  g->setTextDatum(lgfx::textdatum_t::middle_center);
  g->setFont(&lgfx::fonts::Font0);
  int tw = tabW();
  for (int i = 0; i < PAGE_COUNT; i++) {
    int cx = tabCenter(i);
    if (i == page) {
      g->fillRoundRect(cx - tw / 2 + 1, 2, tw - 2, TDS_HDR_H - 4, 3, C_GREEN_D);
      g->setTextColor(C_TXT);
    } else {
      g->setTextColor(C_SUB);
    }
    g->drawString(PAGE_NAME[i], cx, TDS_HDR_H / 2);
  }

  // channel
  g->setTextDatum(lgfx::textdatum_t::middle_right);
  g->setTextColor(C_SUB);
  char buf[12];
  snprintf(buf, sizeof(buf), "CH%u", (unsigned)curCh);
  g->drawString(buf, 268, TDS_HDR_H / 2);

  // battery
  int bx = 272, by = 5;
  bool usb = batMv > 4350 || batMv < 2500;
  int pct = constrain((int)((batMv - 3300) / (4150 - 3300) * 100), 0, 100);
  g->drawRect(bx, by, 30, 11, C_SUB);
  g->fillRect(bx + 30, by + 3, 2, 5, C_SUB);
  if (usb) {
    g->setTextDatum(lgfx::textdatum_t::middle_center);
    g->setTextColor(C_GREEN);
    g->drawString("USB", bx + 15, by + 6);
  } else {
    uint16_t c = pct > 40 ? C_GREEN : (pct > 15 ? C_TIER[2] : C_TIER[4]);
    g->fillRect(bx + 2, by + 2, 26 * pct / 100, 7, c);
  }
}

// ---------------------------------------------------------------- LIVE page
// One count cell for the RADAR 2x2 summary grid.
static void drawCountCell(int x, int y, int w, int h, const char* label,
                          int val, uint16_t col, bool alarm) {
  g->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 6, alarm ? col : C_DIM);
  if (alarm) g->drawRoundRect(x + 3, y + 3, w - 6, h - 6, 6, col);
  g->setFont(&lgfx::fonts::Font0);
  g->setTextDatum(lgfx::textdatum_t::top_center);
  g->setTextColor(C_SUB);
  g->drawString(label, x + w / 2, y + 4);
  g->setFont(&lgfx::fonts::Font4);
  g->setTextDatum(lgfx::textdatum_t::middle_center);
  g->setTextColor(col);
  g->drawNumber(val, x + w / 2, y + h / 2 + 6);
}

static void drawLive(unsigned long now) {
  const int cx = 82, cy = TDS_HDR_H + 1 + 74, R = 72;

  // radar grid
  g->drawCircle(cx, cy, R, C_GRID);
  g->drawCircle(cx, cy, R * 2 / 3, C_GRID);
  g->drawCircle(cx, cy, R / 3, C_GRID);
  g->drawFastHLine(cx - R, cy, 2 * R, C_GRID);
  g->drawFastVLine(cx, cy - R, 2 * R, C_GRID);

  // sweep with fading trail (one revolution / 2.4 s)
  float a0 = (now % 2400) / 2400.0f * 2 * PI;
  for (int i = 0; i < 18; i++) {
    float a = a0 - i * 0.045f;
    uint16_t c = dimCol(C_GREEN, 18 - i, 18 * (i == 0 ? 1 : 2));
    g->drawLine(cx, cy, cx + cosf(a) * R, cy + sinf(a) * R, c);
  }

  // ambient WiFi (grey). Blips glow as the sweep passes, like a phosphor CRT.
  int ambActive = 0, ambAps = 0, ambBest = -1, ambBestRssi = -127;
  {
    TdsAmbView a;
    int na = fyUiAmbCount();
    for (int i = 0; i < na; i++) {
      if (!fyUiGetAmb(i, &a)) continue;
      if (a.prev) continue;
      uint32_t age = now - a.lastSeen;
      if (age > TDS_ACTIVE_MS) continue;
      ambActive++;
      if (a.ap) ambAps++;
      if (a.rssi > ambBestRssi) { ambBestRssi = a.rssi; ambBest = i; }
      float ang = (macHash6(a.mac) % 3600) / 3600.0f * 2 * PI;
      float frac = constrain((-(int)a.rssi - 30) / 65.0f, 0.08f, 1.0f);
      int bx = cx + cosf(ang) * frac * (R - 4);
      int by = cy + sinf(ang) * frac * (R - 4);
      float behind = fmodf(a0 - ang + 4 * PI, 2 * PI);       // radians since sweep passed
      int lvl = 70 + (behind < 1.6f ? (int)(150 * (1.0f - behind / 1.6f)) : 0);
      if (age < 1000) lvl += 30;
      if (age > 8000) lvl = lvl * 2 / 3;
      if (lvl > 230) lvl = 230;
      uint16_t c = lcd.color565(lvl, lvl, lvl + 8 > 255 ? 255 : lvl + 8);
      if (a.ap) g->fillRect(bx - 2, by - 2, 4, 4, c);
      else      g->fillCircle(bx, by, 1, c);
    }
  }

  // ambient BLE (blue). Same phosphor glow, placed by signal strength.
  int bleActive = 0, bleBest = -1, bleBestRssi = -127, skimLive = 0;
  {
    TdsBleView b;
    int nb = fyUiBleCount();
    for (int i = 0; i < nb; i++) {
      if (!fyUiGetBle(i, &b) || b.prev) continue;
      uint32_t age = now - b.lastSeen;
      if (age > TDS_ACTIVE_MS) continue;
      bleActive++;
      if (b.skimmer >= 2) skimLive++;
      if (b.rssi > bleBestRssi) { bleBestRssi = b.rssi; bleBest = i; }
      float ang = (macHash6(b.mac) % 3600) / 3600.0f * 2 * PI;
      float frac = constrain((-(int)b.rssi - 30) / 65.0f, 0.08f, 1.0f);
      int bx = cx + cosf(ang) * frac * (R - 4);
      int by = cy + sinf(ang) * frac * (R - 4);
      float behind = fmodf(a0 - ang + 4 * PI, 2 * PI);
      int lvl = 60 + (behind < 1.6f ? (int)(150 * (1.0f - behind / 1.6f)) : 0);
      if (age < 1000) lvl += 30;
      if (lvl > 230) lvl = 230;
      uint16_t c = b.skimmer ? lcd.color565(lvl > 200 ? 255 : lvl + 40, lvl / 4, lvl / 4)
                             : lcd.color565(lvl / 3, lvl * 2 / 3, lvl > 240 ? 255 : lvl + 15);
      g->fillCircle(bx, by, b.skimmer ? 3 : 2, c);
    }
  }

  // Flock blips (coloured, drawn on top)
  int active = 0, bestTier = -1, bestRssi = -127, bestIdx = -1;
  int n = fyUiDetCount();
  TdsDetView d;
  for (int i = 0; i < n; i++) {
    if (!fyUiGetDet(i, &d)) continue;
    uint32_t age = now - d.lastSeen;
    if (age > TDS_ACTIVE_MS) continue;
    active++;
    if (d.tier > bestTier || (d.tier == bestTier && d.rssi > bestRssi)) {
      bestTier = d.tier; bestRssi = d.rssi; bestIdx = i;
    }
    float ang = (macHash(d.mac) % 3600) / 3600.0f * 2 * PI;
    float frac = constrain((-(int)d.rssi - 30) / 65.0f, 0.08f, 1.0f);
    int bx = cx + cosf(ang) * frac * (R - 6);
    int by = cy + sinf(ang) * frac * (R - 6);
    uint16_t col = tierCol(d.tier);
    if (age > 5000) col = dimCol(col, 1, 2);
    int rad = d.tier >= 3 ? 5 : 3;
    g->fillCircle(bx, by, rad, col);
    if (age < 1500 && (now / 150) % 2) g->drawCircle(bx, by, rad + 4, col);
  }

  // right panel — 2x3 count grid in one of three modes (tap radar to cycle):
  //   0 IN RANGE NOW   1 SINCE RESET (session)   2 ALL TIME (persisted)
  const int px = 166;
  int apNow = ambAps, staNow = ambActive - ambAps, ssidNow = 0;
  { TdsSsidView e;
    for (int i = 0; i < fyUiSsidCount(); i++)
      if (fyUiGetSsid(i, &e) && !e.prev && now - e.lastSeen < TDS_ACTIVE_MS) ssidNow++; }

  int vFlk, vSkim, vAp, vSta, vBle, vSsid;
  if (radarMode == 0) {
    vFlk = active; vSkim = skimLive; vAp = apNow; vSta = staNow; vBle = bleActive; vSsid = ssidNow;
  } else if (radarMode == 1) {
    vFlk = fyUiSess(TDSC_FLOCK); vSkim = fyUiSess(TDSC_SKIM); vAp = fyUiSess(TDSC_AP);
    vSta = fyUiSess(TDSC_CLIENT); vBle = fyUiSess(TDSC_BLE); vSsid = fyUiSess(TDSC_SSID);
  } else {
    vFlk = fyUiLife(TDSC_FLOCK); vSkim = fyUiLife(TDSC_SKIM); vAp = fyUiLife(TDSC_AP);
    vSta = fyUiLife(TDSC_CLIENT); vBle = fyUiLife(TDSC_BLE); vSsid = fyUiLife(TDSC_SSID);
  }

  static const char* const MODE_LBL[3] = { "IN RANGE NOW", "SINCE RESET", "ALL TIME" };
  uint16_t modeCol = (radarMode == 0) ? C_GREEN : (radarMode == 1 ? C_TIER[2] : C_BLE_HI);
  g->setFont(&lgfx::fonts::Font0);
  g->setTextDatum(lgfx::textdatum_t::top_left);
  g->setTextColor(modeCol);
  g->drawString(MODE_LBL[radarMode], px, TDS_HDR_H + 3);
  g->setTextDatum(lgfx::textdatum_t::top_right);
  g->setTextColor(C_SUB);
  g->drawString("tap", TDS_W - 4, TDS_HDR_H + 3);

  int gy = TDS_HDR_H + 16;
  int cw = (TDS_W - px) / 2;
  int chh = (TDS_H - gy) / 3;
  bool live = (radarMode == 0);
  uint16_t flkCol = (vFlk > 0) ? ((live && active) ? tierCol(bestTier) : C_GREEN) : C_GREEN_D;
  drawCountCell(px,       gy,            cw, chh, "FLOCK",  vFlk,  flkCol, (live && active > 0));
  drawCountCell(px + cw,  gy,            cw, chh, "SKIM",   vSkim, vSkim > 0 ? C_TIER[4] : C_DIM, vSkim > 0);
  drawCountCell(px,       gy + chh,      cw, chh, "AP",     vAp,  C_AMB_HI, false);
  drawCountCell(px + cw,  gy + chh,      cw, chh, "CLIENT", vSta, C_AMB,    false);
  drawCountCell(px,       gy + 2 * chh,  cw, chh, "BLE",    vBle, C_BLE_HI, false);
  drawCountCell(px + cw,  gy + 2 * chh,  cw, chh, "SSID",   vSsid, C_GREEN, false);

}

// ---------------------------------------------------------------- LIST page
static int listRows() { return (TDS_H - TDS_LIST_Y0) / TDS_ROW_H; }

static void clampScroll(int n) {
  int rows = listRows();
  int maxS = n > rows ? n - rows : 0;
  if (pageScroll[page] > maxS) pageScroll[page] = maxS;
  if (pageScroll[page] < 0) pageScroll[page] = 0;
}

static void drawListHeader(const char* c0, const char* c1, const char* c2, const char* c3) {
  g->setFont(&lgfx::fonts::Font0);
  g->setTextDatum(lgfx::textdatum_t::top_left);
  g->setTextColor(C_SUB);
  int hy = TDS_HDR_H + 6;
  g->drawString(c0, 10, hy);
  g->drawString(c1, 150, hy);
  g->drawString(c2, 236, hy);
  g->drawString(c3, 280, hy);
  g->drawFastHLine(0, TDS_LIST_Y0 - 3, TDS_W, C_DIM);
}

static void drawScrollbar(int n) {
  int rows = listRows();
  int maxS = n > rows ? n - rows : 0;
  if (maxS <= 0) return;
  int trackH = TDS_H - TDS_LIST_Y0;
  int barH = max(10, trackH * rows / n);
  int barY = TDS_LIST_Y0 + (trackH - barH) * pageScroll[page] / maxS;
  g->fillRect(TDS_W - 3, barY, 3, barH, C_DIM);
}

static void emptyMsg(const char* m) {
  g->setFont(&lgfx::fonts::Font2);
  g->setTextDatum(lgfx::textdatum_t::middle_center);
  g->setTextColor(C_SUB);
  g->drawString(m, TDS_W / 2, (TDS_LIST_Y0 + TDS_H) / 2);
}

// FLOCK page — confirmed/ suspected Flock devices, newest first.
static void drawFlock(unsigned long now) {
  int n = buildSorted();
  clampScroll(n);
  drawListHeader("FLOCK DEVICE (MAC)", "TYPE", "RSSI", "AGE");
  if (n == 0) { emptyMsg("No Flock devices yet"); return; }
  int rows = listRows(), sc = pageScroll[page];
  TdsDetView d;
  char age[8];
  g->setFont(&lgfx::fonts::Font2);
  for (int r = 0; r < rows && sc + r < n; r++) {
    if (!fyUiGetDet(sortedIdx[sc + r], &d)) continue;
    int y = TDS_LIST_Y0 + r * TDS_ROW_H;
    uint16_t col = tierCol(d.tier);
    bool fresh = now - d.lastSeen < TDS_ACTIVE_MS;
    if (r % 2) g->fillRect(0, y - 1, TDS_W, TDS_ROW_H, lcd.color565(12, 18, 24));
    g->fillRect(2, y + 1, 4, TDS_ROW_H - 4, col);
    g->setTextColor(fresh ? C_TXT : C_SUB);
    g->drawString(d.mac, 10, y);
    g->setTextColor(col);
    g->drawString(isBleMethod(d.method) ? "BLE" : TIER_SHORT[d.tier], 150, y);
    g->setTextColor(fresh ? C_TXT : C_SUB);
    g->drawNumber(d.rssi, 236, y);
    fmtAge(now - d.lastSeen, age, sizeof(age));
    g->drawString(age, 280, y);
  }
  drawScrollbar(n);
}

// AP / STA pages — ordinary WiFi in grey, with vendor + inferred device type.
static void drawAmbientList(unsigned long now, bool apMode) {
  int n = buildAmb(apMode, now, TDS_AMB_LIST_MS);
  clampScroll(n);
  if (apMode) drawListHeader("NETWORK / AP", "VENDOR", "RSSI", "AGE");
  else        drawListHeader("CLIENT (MAC)", "VENDOR", "RSSI", "AGE");
  if (n == 0) { emptyMsg(apMode ? "No access points yet" : "No clients yet"); return; }
  int rows = listRows(), sc = pageScroll[page];
  TdsAmbView a;
  char mac[18], age[8], vend[24];
  g->setFont(&lgfx::fonts::Font2);
  for (int r = 0; r < rows && sc + r < n; r++) {
    if (!fyUiGetAmb(ambSorted[sc + r], &a)) continue;
    int y = TDS_LIST_Y0 + r * TDS_ROW_H;
    bool fresh = now - a.lastSeen < 5000;
    if (r % 2) g->fillRect(0, y - 1, TDS_W, TDS_ROW_H, lcd.color565(12, 18, 24));
    uint8_t cat; const char* vn = ouiLookup(a.mac, &cat);
    g->fillRect(2, y + 1, 4, TDS_ROW_H - 4, vn ? dimCol(catCol(cat), 2, 3) : C_DIM);
    // left column
    g->setTextColor(fresh ? C_AMB_HI : C_AMB);
    if (apMode) {
      if (a.ssid[0]) { g->drawString(a.ssid, 10, y); }
      else { g->setTextColor(C_DIM); g->drawString("(hidden)", 10, y); }
    } else {
      macStr6(a.mac, mac, sizeof(mac));
      g->drawString(mac, 10, y);
    }
    // vendor / randomized
    if (vn) { g->setTextColor(catCol(cat)); strlcpy(vend, vn, sizeof(vend)); vend[12] = '\0'; g->drawString(vend, 150, y); }
    else    { g->setTextColor(C_DIM); g->drawString(apMode ? "?" : "random", 150, y); }
    g->setTextColor(fresh ? C_AMB_HI : C_AMB);
    g->drawNumber(a.rssi, 236, y);
    if (a.prev) { g->setTextColor(C_DIM); g->drawString("prev", 280, y); }
    else { fmtAge(now - a.lastSeen, age, sizeof(age)); g->drawString(age, 280, y); }
  }
  drawScrollbar(n);
}

// BLE page — ordinary Bluetooth advertisers in grey/blue, name + type.
static void drawBleList(unsigned long now) {
  int nb = buildBle(now, TDS_AMB_LIST_MS);
  clampScroll(nb);
  drawListHeader("BLE NAME / MAC", "TYPE", "RSSI", "AGE");
  if (nb == 0) { emptyMsg("No BLE devices yet"); return; }
  int rows = listRows(), sc = pageScroll[page];
  TdsBleView b;
  char mac[18], age[8], tbuf[24];
  g->setFont(&lgfx::fonts::Font2);
  for (int r = 0; r < rows && sc + r < nb; r++) {
    if (!fyUiGetBle(bleSorted[sc + r], &b)) continue;
    int y = TDS_LIST_Y0 + r * TDS_ROW_H;
    bool fresh = !b.prev && now - b.lastSeen < 5000;
    if (r % 2) g->fillRect(0, y - 1, TDS_W, TDS_ROW_H, lcd.color565(12, 18, 24));
    bool suspect = (b.skimmer >= 2);
    uint16_t bar = suspect ? C_TIER[4] : (b.prev ? C_DIM : dimCol(C_BLE, 2, 3));
    g->fillRect(2, y + 1, 4, TDS_ROW_H - 4, bar);
    g->setTextColor(suspect ? C_TIER[4] : (fresh ? C_BLE_HI : C_AMB));
    if (b.name[0]) g->drawString(b.name, 10, y);
    else { macStr6(b.mac, mac, sizeof(mac)); g->drawString(mac, 10, y); }
    if (suspect)             { strlcpy(tbuf, "SKIM?",  sizeof(tbuf)); g->setTextColor(C_TIER[4]); }
    else if (b.skimmer == 1) { strlcpy(tbuf, "serial", sizeof(tbuf)); g->setTextColor(b.prev ? C_DIM : C_TIER[2]); }
    else                     { bleType(b, tbuf, sizeof(tbuf));        g->setTextColor(b.prev ? C_DIM : C_BLE_HI); }
    tbuf[12] = '\0';
    g->drawString(tbuf, 150, y);
    g->setTextColor(fresh ? C_BLE_HI : C_AMB);
    g->drawNumber(b.rssi, 236, y);
    if (b.prev) { g->setTextColor(C_DIM); g->drawString("prev", 280, y); }
    else { fmtAge(now - b.lastSeen, age, sizeof(age)); g->drawString(age, 280, y); }
  }
  drawScrollbar(nb);
}

// FLOCK detail card.
static void drawDetail(unsigned long now) {
  TdsDetView d;
  if (!fyUiGetDet(detailIdx, &d)) { detailIdx = -1; return; }
  uint16_t col = tierCol(d.tier);
  g->fillRect(0, TDS_HDR_H, TDS_W, TDS_H - TDS_HDR_H, C_BG);
  g->drawRoundRect(2, TDS_HDR_H + 2, TDS_W - 4, TDS_H - TDS_HDR_H - 4, 6, col);
  g->setTextDatum(lgfx::textdatum_t::top_left);
  g->setFont(&lgfx::fonts::Font2);
  g->setTextColor(col);
  g->drawString((d.tier == 2 && isBleMethod(d.method)) ? "BLE MATCH" : TIER_BIG[d.tier], 10, TDS_HDR_H + 6);
  g->setFont(&lgfx::fonts::Font4);
  g->setTextColor(C_TXT);
  g->drawString(d.mac, 10, TDS_HDR_H + 24);

  char buf[64], a1[8], a2[8];
  g->setFont(&lgfx::fonts::Font2);
  g->setTextColor(C_SUB);
  int y = TDS_HDR_H + 56;
  snprintf(buf, sizeof(buf), "method  %s", d.method);                         g->drawString(buf, 10, y); y += 17;
  if (d.channel) snprintf(buf, sizeof(buf), "rssi  %d dBm   ch %u   hits %u", (int)d.rssi, (unsigned)d.channel, (unsigned)d.count);
  else           snprintf(buf, sizeof(buf), "rssi  %d dBm   BLE   hits %u", (int)d.rssi, (unsigned)d.count);
  g->drawString(buf, 10, y); y += 17;
  fmtAge(now - d.firstSeen, a1, sizeof(a1));
  fmtAge(now - d.lastSeen, a2, sizeof(a2));
  snprintf(buf, sizeof(buf), "first %s ago   last %s ago", a1, a2);             g->drawString(buf, 10, y); y += 17;
  if (d.name[0]) { snprintf(buf, sizeof(buf), "name  %s", d.name);          g->drawString(buf, 10, y); }
  g->setFont(&lgfx::fonts::Font0);
  g->setTextDatum(lgfx::textdatum_t::bottom_right);
  g->drawString("tap to close", TDS_W - 8, TDS_H - 6);
}

// AP / STA detail card — vendor, inferred type, BSSID and network names.
static void drawAmbDetail(unsigned long now) {
  TdsAmbView a; bool found = false;
  int total = fyUiAmbCount();
  for (int i = 0; i < total; i++) {
    if (fyUiGetAmb(i, &a) && memcmp(a.mac, ambDetailMac, 6) == 0) { found = true; break; }
  }
  if (!found) { ambDetail = false; return; }
  uint8_t cat; const char* vn = ouiLookup(a.mac, &cat);
  uint16_t col = vn ? catCol(cat) : C_AMB_HI;
  g->fillRect(0, TDS_HDR_H, TDS_W, TDS_H - TDS_HDR_H, C_BG);
  g->drawRoundRect(2, TDS_HDR_H + 2, TDS_W - 4, TDS_H - TDS_HDR_H - 4, 6, col);

  char buf[72], mac[18], catlbl[12];
  ouiCatLabel(cat, catlbl, sizeof(catlbl));
  macStr6(a.mac, mac, sizeof(mac));
  g->setTextDatum(lgfx::textdatum_t::top_left);
  g->setFont(&lgfx::fonts::Font4);
  g->setTextColor(col);
  g->drawString(a.ap ? "ACCESS POINT" : "WIFI CLIENT", 10, TDS_HDR_H + 5);

  g->setFont(&lgfx::fonts::Font2);
  int y = TDS_HDR_H + 28;
  if (a.ap) {
    g->setTextColor(C_TXT);
    if (a.ssid[0]) { snprintf(buf, sizeof(buf), "SSID  %s", a.ssid); g->drawString(buf, 10, y); }
    else           { g->setTextColor(C_DIM); g->drawString("SSID  (hidden)", 10, y); }
    y += 18;
    g->setTextColor(C_SUB);
    snprintf(buf, sizeof(buf), "BSSID %s", mac);                                g->drawString(buf, 10, y); y += 17;
  } else {
    g->setTextColor(C_TXT);
    snprintf(buf, sizeof(buf), "MAC   %s", mac);                                g->drawString(buf, 10, y); y += 18;
    g->setTextColor(C_SUB);
    if (a.hasBssid) { char b2[18]; macStr6(a.bssid, b2, sizeof(b2));
      snprintf(buf, sizeof(buf), "BSSID %s", b2); g->drawString(buf, 10, y); y += 17; }
  }
  g->setTextColor(C_SUB);
  if (a.mac[0] & 0x02) { g->drawString("vendor  randomized MAC (private)", 10, y); y += 17; }
  else {
    snprintf(buf, sizeof(buf), "vendor  %s%s%s", vn ? vn : "unknown",
             catlbl[0] ? "  -  " : "", catlbl);
    g->drawString(buf, 10, y); y += 17;
  }
  snprintf(buf, sizeof(buf), "%d dBm   ch %u   %u frames", (int)a.rssi, (unsigned)a.ch, (unsigned)a.count);
  g->drawString(buf, 10, y); y += 17;
  if (!a.ap && a.probedCount) {
    g->setTextColor(C_AMB_HI);
    snprintf(buf, sizeof(buf), "probing %u network%s:", a.probedCount, a.probedCount == 1 ? "" : "s");
    g->drawString(buf, 10, y); y += 15;
    g->setFont(&lgfx::fonts::Font0);
    g->setTextColor(C_SUB);
    for (int k = 0; k < a.probedCount && y < TDS_H - 10; k++) {
      snprintf(buf, sizeof(buf), "  %s", a.probed[k]);
      g->drawString(buf, 12, y); y += 11;
    }
  } else if (a.prev) {
    g->drawString("seen last session", 10, y);
  }
  g->setFont(&lgfx::fonts::Font0);
  g->setTextDatum(lgfx::textdatum_t::bottom_right);
  g->setTextColor(C_SUB);
  g->drawString("tap to close", TDS_W - 8, TDS_H - 6);
}

// BLE detail card — vendor (company ID), self-reported type, name, services.
static void drawBleDetail(unsigned long now) {
  TdsBleView b; bool found = false;
  int total = fyUiBleCount();
  for (int i = 0; i < total; i++)
    if (fyUiGetBle(i, &b) && memcmp(b.mac, ambDetailMac, 6) == 0) { found = true; break; }
  if (!found) { ambDetail = false; return; }
  g->fillRect(0, TDS_HDR_H, TDS_W, TDS_H - TDS_HDR_H, C_BG);
  g->drawRoundRect(2, TDS_HDR_H + 2, TDS_W - 4, TDS_H - TDS_HDR_H - 4, 6, C_BLE);

  char buf[64], mac[18], tbuf[24];
  g->setTextDatum(lgfx::textdatum_t::top_left);
  g->setFont(&lgfx::fonts::Font4);
  g->setTextColor(C_BLE_HI);
  g->drawString("BLUETOOTH LE", 10, TDS_HDR_H + 6);

  g->setFont(&lgfx::fonts::Font2);
  int y = TDS_HDR_H + 32;
  g->setTextColor(C_TXT);
  if (b.name[0]) { g->drawString(b.name, 10, y); y += 18; }
  macStr6(b.mac, mac, sizeof(mac));
  g->drawString(mac, 10, y); y += 18;
  g->setTextColor(C_SUB);
  const char* at = (b.addrType == 0 || b.addrType == 2) ? "public" : "random/private";
  snprintf(buf, sizeof(buf), "address  %s", at);                               g->drawString(buf, 10, y); y += 17;
  const char* vn = bleVendor(b.company);
  if (vn)                      snprintf(buf, sizeof(buf), "vendor  %s", vn);
  else if (b.company != 0xFFFF) snprintf(buf, sizeof(buf), "vendor  company 0x%04X", b.company);
  else                         snprintf(buf, sizeof(buf), "vendor  (none advertised)");
  g->drawString(buf, 10, y); y += 17;
  bleType(b, tbuf, sizeof(tbuf));
  if (b.appearance) snprintf(buf, sizeof(buf), "type    %s (appear 0x%04X)", tbuf, b.appearance);
  else              snprintf(buf, sizeof(buf), "type    %s", tbuf);
  g->drawString(buf, 10, y); y += 17;
  if (b.skimmer >= 2) {
    g->setTextColor(C_TIER[4]);
    g->drawString("! SUSPECT SKIMMER MODULE", 10, y); y += 15;
    g->setFont(&lgfx::fonts::Font0);
    g->setTextColor(C_SUB);
    g->drawString("known serial-module name - often harmless, verify", 10, y); y += 13;
    g->setFont(&lgfx::fonts::Font2);
    g->setTextColor(C_SUB);
  } else if (b.skimmer == 1) {
    g->setFont(&lgfx::fonts::Font0);
    g->setTextColor(C_SUB);
    g->drawString("raw serial service - common in IoT/hobby gear", 10, y); y += 13;
    g->setFont(&lgfx::fonts::Font2);
    g->setTextColor(C_SUB);
  }
  char a1[8], a2[8];
  fmtAge(now - b.firstSeen, a1, sizeof(a1)); fmtAge(now - b.lastSeen, a2, sizeof(a2));
  snprintf(buf, sizeof(buf), "%d dBm   %u services   %u ads", (int)b.rssi, (unsigned)b.svc, (unsigned)b.count);
  g->drawString(buf, 10, y); y += 17;
  if (b.prev) g->drawString("seen last session", 10, y);
  else { snprintf(buf, sizeof(buf), "first %s ago   last %s ago", a1, a2); g->drawString(buf, 10, y); }
  g->setFont(&lgfx::fonts::Font0);
  g->setTextDatum(lgfx::textdatum_t::bottom_right);
  g->setTextColor(C_SUB);
  g->drawString("tap to close", TDS_W - 8, TDS_H - 6);
}

// SSID page — every network name detected, from beacons and probe requests.
static int ssidSorted[96];
static int buildSsidList(unsigned long now) {
  int n = 0, total = fyUiSsidCount();
  TdsSsidView a, b;
  for (int i = 0; i < total && n < 96; i++) {
    if (!fyUiGetSsid(i, &a)) continue;
    int j = n++;
    while (j > 0) {
      fyUiGetSsid(ssidSorted[j - 1], &b);
      bool bBetter = (b.prev < a.prev) || (b.prev == a.prev && b.rssi >= a.rssi);
      if (bBetter) break;
      ssidSorted[j] = ssidSorted[j - 1];
      j--;
    }
    ssidSorted[j] = i;
  }
  return n;
}

static void drawSsidList(unsigned long now) {
  int n = buildSsidList(now);
  clampScroll(n);
  drawListHeader("NETWORK NAME (SSID)", "CH", "SRC", "AGE");
  if (n == 0) { emptyMsg("No SSIDs detected yet"); return; }
  int rows = listRows(), sc = pageScroll[page];
  TdsSsidView e;
  char age[8];
  g->setFont(&lgfx::fonts::Font2);
  for (int r = 0; r < rows && sc + r < n; r++) {
    if (!fyUiGetSsid(ssidSorted[sc + r], &e)) continue;
    int y = TDS_LIST_Y0 + r * TDS_ROW_H;
    if (r % 2) g->fillRect(0, y - 1, TDS_W, TDS_ROW_H, lcd.color565(12, 18, 24));
    uint16_t col = e.fromAp ? C_GREEN : C_TIER[2];   // AP-advertised vs probe-only
    g->fillRect(2, y + 1, 4, TDS_ROW_H - 4, e.prev ? C_DIM : col);
    g->setTextColor(e.prev ? C_DIM : C_TXT);
    g->drawString(e.ssid[0] ? e.ssid : "(hidden)", 10, y);
    g->setTextColor(e.prev ? C_DIM : C_SUB);
    if (e.ch) g->drawNumber(e.ch, 236, y);
    g->setTextColor(e.prev ? C_DIM : col);
    g->drawString(e.fromAp ? "AP" : "probe", 262, y);
    g->setTextColor(C_SUB);
    if (e.prev) g->drawString("prev", 290, y);
    else { fmtAge(now - e.lastSeen, age, sizeof(age)); g->drawString(age, 290, y); }
  }
  drawScrollbar(n);
}

// ---------------------------------------------------------------- STATS page
// Touch buttons on the stats page
struct Btn { int x, y, w, h; const char* label; };
static const Btn BTN_BRIGHT  = { 212, TDS_HDR_H + 2,   104, 19, "Brightness" };
static const Btn BTN_USB     = { 212, TDS_HDR_H + 23,  104, 19, "USB full" };
static const Btn BTN_FLIP    = { 212, TDS_HDR_H + 44,  104, 19, "Flip screen" };
static const Btn BTN_OFF     = { 212, TDS_HDR_H + 65,  104, 19, "Screen off" };
static const Btn BTN_REBOOT  = { 212, TDS_HDR_H + 86,  104, 19, "Reboot" };
static const Btn BTN_RSTSESS = { 212, TDS_HDR_H + 107, 104, 19, "Reset session" };
static const Btn BTN_FACTORY = { 212, TDS_HDR_H + 128, 104, 19, "Factory reset" };
// confirmation dialog buttons
static const Btn BTN_CFM_NO  = { 36,  TDS_HDR_H + 86, 110, 34, "Cancel" };
static const Btn BTN_CFM_YES = { 174, TDS_HDR_H + 86, 110, 34, "Confirm" };
static void drawConfirm();

static void drawBtn(const Btn& b) {
  g->fillRoundRect(b.x, b.y, b.w, b.h, 6, C_HDR);
  g->drawRoundRect(b.x, b.y, b.w, b.h, 6, C_GREEN_D);
  g->setFont(&lgfx::fonts::Font2);
  g->setTextDatum(lgfx::textdatum_t::middle_center);
  g->setTextColor(C_TXT);
  g->drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
}
static bool inBtn(const Btn& b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static void drawStats(unsigned long now) {
  char buf[48];
  g->setFont(&lgfx::fonts::Font2);
  g->setTextDatum(lgfx::textdatum_t::top_left);
  int x = 8, y = TDS_HDR_H + 6;
  unsigned long s = now / 1000;
  snprintf(buf, sizeof(buf), "Uptime  %02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  g->setTextColor(C_TXT); g->drawString(buf, x, y); y += 17;
  int n = fyUiDetCount();
  snprintf(buf, sizeof(buf), "Unique  %d / 200", n);
  g->drawString(buf, x, y); y += 17;
  {
    TdsAmbView a; int seen = 0, aps = 0;
    for (int i = 0; i < fyUiAmbCount(); i++)
      if (fyUiGetAmb(i, &a)) { seen++; if (a.ap) aps++; }
    snprintf(buf, sizeof(buf), "%lu fr/s  WiFi %d (%d AP)", (unsigned long)framesPerSec, seen, aps);
  }
  g->drawString(buf, x, y); y += 17;
  {
    TdsBleView b; int bn = 0, named = 0;
    for (int i = 0; i < fyUiBleCount(); i++)
      if (fyUiGetBle(i, &b)) { bn++; if (b.name[0]) named++; }
    snprintf(buf, sizeof(buf), "BLE  %d  (%d named)", bn, named);
  }
  g->setTextColor(C_BLE_HI); g->drawString(buf, x, y); y += 17;
  snprintf(buf, sizeof(buf), "SSIDs detected  %d", fyUiSsidCount());
  g->setTextColor(C_GREEN); g->drawString(buf, x, y); y += 17;
  {
    TdsBleView b; int sk = 0;
    for (int i = 0; i < fyUiBleCount(); i++) if (fyUiGetBle(i, &b) && b.skimmer >= 2) sk++;
    if (sk) {
      snprintf(buf, sizeof(buf), "! %d BLE skimmer suspect%s", sk, sk == 1 ? "" : "s");
      g->setTextColor(C_TIER[4]); g->drawString(buf, x, y); y += 17;
    }
  }
  g->setTextColor(C_TXT);

  // per-tier counts
  int cnt[5] = {0};
  TdsDetView d;
  for (int i = 0; i < n; i++) if (fyUiGetDet(i, &d) && d.tier < 5) cnt[d.tier]++;
  int tx = x;
  for (int t = 4; t >= 0; t--) {
    snprintf(buf, sizeof(buf), "%s %d", TIER_SHORT[t], cnt[t]);
    g->setTextColor(tierCol(t));
    g->drawString(buf, tx, y);
    tx += g->textWidth(buf) + 8;
    if (tx > 190 && t > 0) { tx = x; y += 17; }
  }
  y += 10;

  g->setTextColor(C_SUB);
  bool usb = batMv > 4350 || batMv < 2500;
  if (usb) snprintf(buf, sizeof(buf), "Battery  USB (%.2f V)", batMv / 1000.0f);
  else     snprintf(buf, sizeof(buf), "Battery  %.2f V", batMv / 1000.0f);
  g->drawString(buf, x, y); y += 17;
  snprintf(buf, sizeof(buf), "SPIFFS %s  beep 0x%02X", fyUiSpiffsReady() ? "ok" : "FAIL", fyUiBeepMask());
  g->drawString(buf, x, y); y += 17;
  snprintf(buf, sizeof(buf), "Heap %uk  PSRAM %uk",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  g->drawString(buf, x, y);

  drawBtn(BTN_BRIGHT);
  // default-brightness level, right-aligned inside the Brightness button
  g->setFont(&lgfx::fonts::Font0);
  g->setTextDatum(lgfx::textdatum_t::middle_right);
  g->setTextColor(C_GREEN);
  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(brightIdx + 1), (unsigned)BRIGHT_N);
  g->drawString(buf, BTN_BRIGHT.x + BTN_BRIGHT.w - 6, BTN_BRIGHT.y + BTN_BRIGHT.h / 2);

  // USB-full checkbox button
  g->fillRoundRect(BTN_USB.x, BTN_USB.y, BTN_USB.w, BTN_USB.h, 6, C_HDR);
  g->drawRoundRect(BTN_USB.x, BTN_USB.y, BTN_USB.w, BTN_USB.h, 6,
                   usbFullBright ? C_GREEN : C_GREEN_D);
  {
    int cbx = BTN_USB.x + 8, cby = BTN_USB.y + BTN_USB.h / 2 - 6;
    g->drawRect(cbx, cby, 12, 12, C_SUB);
    if (usbFullBright) g->fillRect(cbx + 3, cby + 3, 6, 6, C_GREEN);
    g->setFont(&lgfx::fonts::Font2);
    g->setTextDatum(lgfx::textdatum_t::middle_left);
    g->setTextColor(C_TXT);
    g->drawString("USB=full", cbx + 18, BTN_USB.y + BTN_USB.h / 2);
  }

  drawBtn(BTN_FLIP);
  drawBtn(BTN_OFF);

  // Reboot — distinct cyan border to mark it as a power action.
  g->fillRoundRect(BTN_REBOOT.x, BTN_REBOOT.y, BTN_REBOOT.w, BTN_REBOOT.h, 6, C_HDR);
  g->drawRoundRect(BTN_REBOOT.x, BTN_REBOOT.y, BTN_REBOOT.w, BTN_REBOOT.h, 6, C_TIER[1]);
  g->setFont(&lgfx::fonts::Font2);
  g->setTextDatum(lgfx::textdatum_t::middle_center);
  g->setTextColor(C_TXT);
  g->drawString("Reboot", BTN_REBOOT.x + BTN_REBOOT.w / 2, BTN_REBOOT.y + BTN_REBOOT.h / 2);

  // reset buttons (coloured to signal weight)
  g->fillRoundRect(BTN_RSTSESS.x, BTN_RSTSESS.y, BTN_RSTSESS.w, BTN_RSTSESS.h, 6, C_HDR);
  g->drawRoundRect(BTN_RSTSESS.x, BTN_RSTSESS.y, BTN_RSTSESS.w, BTN_RSTSESS.h, 6, C_TIER[2]);
  g->setFont(&lgfx::fonts::Font2);
  g->setTextDatum(lgfx::textdatum_t::middle_center);
  g->setTextColor(C_TIER[2]);
  g->drawString("Reset session", BTN_RSTSESS.x + BTN_RSTSESS.w / 2, BTN_RSTSESS.y + BTN_RSTSESS.h / 2);
  g->fillRoundRect(BTN_FACTORY.x, BTN_FACTORY.y, BTN_FACTORY.w, BTN_FACTORY.h, 6, C_HDR);
  g->drawRoundRect(BTN_FACTORY.x, BTN_FACTORY.y, BTN_FACTORY.w, BTN_FACTORY.h, 6, C_TIER[4]);
  g->setTextColor(C_TIER[4]);
  g->drawString("Factory reset", BTN_FACTORY.x + BTN_FACTORY.w / 2, BTN_FACTORY.y + BTN_FACTORY.h / 2);

  if (pendingReset) drawConfirm();
}

// Modal confirmation for the two resets.
static void drawConfirm() {
  g->fillRect(0, TDS_HDR_H, TDS_W, TDS_H - TDS_HDR_H, C_BG);
  uint16_t accent = (pendingReset == 2) ? C_TIER[4]
                  : (pendingReset == 3) ? C_TIER[1]
                                        : C_TIER[2];
  const char* title = (pendingReset == 2) ? "FACTORY RESET?"
                    : (pendingReset == 3) ? "Reboot now?"
                                          : "Reset session count?";
  const char* sub   = (pendingReset == 2) ? "Erases all counts, lists and saved data."
                    : (pendingReset == 3) ? "Restarts the detector. Saved data is kept."
                                          : "Zeroes the since-reset counters only.";
  g->drawRoundRect(14, TDS_HDR_H + 8, TDS_W - 28, TDS_H - TDS_HDR_H - 16, 8, accent);
  g->setTextDatum(lgfx::textdatum_t::top_center);
  g->setFont(&lgfx::fonts::Font4);
  g->setTextColor(accent);
  g->drawString(title, TDS_W / 2, TDS_HDR_H + 18);
  g->setFont(&lgfx::fonts::Font2);
  g->setTextColor(C_SUB);
  g->drawString(sub, TDS_W / 2, TDS_HDR_H + 46);
  // buttons
  g->fillRoundRect(BTN_CFM_NO.x, BTN_CFM_NO.y, BTN_CFM_NO.w, BTN_CFM_NO.h, 6, C_HDR);
  g->drawRoundRect(BTN_CFM_NO.x, BTN_CFM_NO.y, BTN_CFM_NO.w, BTN_CFM_NO.h, 6, C_GREEN_D);
  g->fillRoundRect(BTN_CFM_YES.x, BTN_CFM_YES.y, BTN_CFM_YES.w, BTN_CFM_YES.h, 6, accent);
  g->setTextDatum(lgfx::textdatum_t::middle_center);
  g->setTextColor(C_TXT);
  g->drawString("Cancel", BTN_CFM_NO.x + BTN_CFM_NO.w / 2, BTN_CFM_NO.y + BTN_CFM_NO.h / 2);
  g->setTextColor(TFT_BLACK);
  g->drawString(pendingReset == 2 ? "ERASE" : (pendingReset == 3 ? "REBOOT" : "Confirm"),
                BTN_CFM_YES.x + BTN_CFM_YES.w / 2, BTN_CFM_YES.y + BTN_CFM_YES.h / 2);
}

// ---------------------------------------------------------------- ALERT
static void drawSkimmerAlert(unsigned long now);
static void drawAlert(unsigned long now) {
  if (alert.skimmer) { drawSkimmerAlert(now); return; }
  unsigned long el = now - alert.start;
  uint16_t col = tierCol(alert.tier);
  bool flashOn = el < TDS_FLASH_MS && ((el / 150) % 2 == 0);
  bool loud = alert.tier >= 3;
  uint16_t bg = (loud && flashOn) ? col : C_BG;
  uint16_t fg = (loud && flashOn) ? TFT_BLACK : C_TXT;
  g->fillRect(0, TDS_HDR_H, TDS_W, TDS_H - TDS_HDR_H, bg);
  g->fillRect(0, TDS_HDR_H, TDS_W, 4, col);
  g->fillRect(0, TDS_H - 4, TDS_W, 4, col);

  bool ble = isBleMethod(alert.method);
  const char* big = (alert.tier == 2 && ble) ? "BLE MATCH" : TIER_BIG[alert.tier];
  g->setTextDatum(lgfx::textdatum_t::top_center);
  g->setFont(&lgfx::fonts::FreeSansBold18pt7b);
  if (g->textWidth(big) > TDS_W - 10) g->setFont(&lgfx::fonts::FreeSansBold12pt7b);
  g->setTextColor((loud && flashOn) ? TFT_BLACK : col);
  g->drawString(big, TDS_W / 2, TDS_HDR_H + 10);

  g->setFont(&lgfx::fonts::Font4);
  g->setTextColor(fg);
  g->drawString(alert.mac, TDS_W / 2, TDS_HDR_H + 50);

  char buf[64];
  g->setFont(&lgfx::fonts::Font2);
  if (alert.ch) snprintf(buf, sizeof(buf), "%d dBm    ch %u    hits %d", (int)alert.rssi, (unsigned)alert.ch, alert.count);
  else          snprintf(buf, sizeof(buf), "%d dBm    BLE    hits %d", (int)alert.rssi, alert.count);
  g->drawString(buf, TDS_W / 2, TDS_HDR_H + 82);
  g->setTextColor((loud && flashOn) ? TFT_BLACK : C_SUB);
  if (alert.name[0]) snprintf(buf, sizeof(buf), "\"%s\"", alert.name);
  else               snprintf(buf, sizeof(buf), "%s", TIER_WHY[alert.tier < 5 ? alert.tier : 0]);
  g->drawString(buf, TDS_W / 2, TDS_HDR_H + 102);
  g->setFont(&lgfx::fonts::Font0);
  g->drawString(alert.method, TDS_W / 2, TDS_HDR_H + 124);

  // countdown bar
  int w = TDS_W * (TDS_ALERT_MS - min<unsigned long>(el, TDS_ALERT_MS)) / TDS_ALERT_MS;
  g->fillRect(0, TDS_H - 4, w, 4, TFT_WHITE);
}

// ---------------------------------------------------------------- input
struct Button { uint8_t pin; bool down; unsigned long downAt; bool longFired; };
static Button btnKey  = { TDS_PIN_BTN_KEY,  false, 0, false };
static Button btnBoot = { TDS_PIN_BTN_BOOT, false, 0, false };

// returns 0 none, 1 short, 2 long (long fires while still held)
static int pollButton(Button& b, unsigned long now) {
  bool pressed = digitalRead(b.pin) == LOW;
  if (pressed && !b.down) { b.down = true; b.downAt = now; b.longFired = false; return 0; }
  if (pressed && b.down && !b.longFired && now - b.downAt >= TDS_LONGPRESS_MS) {
    b.longFired = true; return 2;
  }
  if (!pressed && b.down) {
    b.down = false;
    if (!b.longFired && now - b.downAt >= 30) return 1;
  }
  return 0;
}

static void savePrefs() {
  prefs.putUChar("bright", brightIdx);
  prefs.putUChar("rot", rotation);
  prefs.putBool("usbbright", usbFullBright);
}

static void changePage(int target) {
  target = (target % PAGE_COUNT + PAGE_COUNT) % PAGE_COUNT;
  if (target == page) return;
  page = target;
  detailIdx = -1;
  ambDetail = false;
  lastFrame = 0;
}
static void nextPage(int dir) { changePage(page + dir); }

static void cycleBrightness() {
  brightIdx = (brightIdx + 1) % BRIGHT_N;
  applyBrightness();
  savePrefs();
}

static void flipScreen() {
  rotation = (rotation == 1) ? 3 : 1;
  lcd.setRotation(rotation);
  savePrefs();
}

static void doFactoryReset() {
  fyUiFactoryReset();                 // counters, tables, saved files, beep mask
  brightIdx = 0; rotation = 1; usbFullBright = true;
  prefs.clear();
  savePrefs();
  lcd.setRotation(rotation);
  applyBrightness();
  radarMode = 0;
}

static bool closeOverlay() {       // dismiss alert / detail / confirm; true if something closed
  if (pendingReset)   { pendingReset = 0; return true; }
  if (alert.active)   { alert.active = false; return true; }
  if (detailIdx >= 0) { detailIdx = -1; return true; }
  if (ambDetail)      { ambDetail = false; return true; }
  return false;
}

static void handleTap(int x, int y) {
  if (pendingReset) {                        // modal confirm owns all input
    if (inBtn(BTN_CFM_YES, x, y)) {
      if (pendingReset == 2)      doFactoryReset();
      else if (pendingReset == 3) { delay(80); ESP.restart(); }
      else                        fyUiResetSession();
    }
    pendingReset = 0;                        // Cancel, or anywhere else, dismisses
    lastFrame = 0;
    return;
  }
  if (closeOverlay()) return;
  if (y < TDS_HDR_H) {
    int tw = tabW();
    for (int i = 0; i < PAGE_COUNT; i++) {
      int cx = tabCenter(i);
      if (x >= cx - tw / 2 && x < cx + tw / 2) { changePage(i); return; }
    }
    return;
  }
  if (page == PAGE_RADAR) { radarMode = (radarMode + 1) % 3; return; }   // tap cycles mode
  int r = (y - TDS_LIST_Y0) / TDS_ROW_H;
  if (page == PAGE_FLOCK) {
    int n = buildSorted();
    if (y >= TDS_LIST_Y0 && pageScroll[page] + r < n) detailIdx = sortedIdx[pageScroll[page] + r];
  } else if (page == PAGE_AP || page == PAGE_STA) {
    int n = buildAmb(page == PAGE_AP, millis(), TDS_AMB_LIST_MS);
    if (y >= TDS_LIST_Y0 && pageScroll[page] + r < n) {
      TdsAmbView a;
      if (fyUiGetAmb(ambSorted[pageScroll[page] + r], &a)) {
        memcpy(ambDetailMac, a.mac, 6); ambDetail = true;
      }
    }
  } else if (page == PAGE_BLE) {
    int n = buildBle(millis(), TDS_AMB_LIST_MS);
    if (y >= TDS_LIST_Y0 && pageScroll[page] + r < n) {
      TdsBleView b;
      if (fyUiGetBle(bleSorted[pageScroll[page] + r], &b)) {
        memcpy(ambDetailMac, b.mac, 6); ambDetail = true;
      }
    }
  } else if (page == PAGE_STATS) {
    if (inBtn(BTN_BRIGHT, x, y)) cycleBrightness();
    else if (inBtn(BTN_USB, x, y)) {
      usbFullBright = !usbFullBright;
      prefs.putBool("usbbright", usbFullBright);
      applyBrightness();
    }
    else if (inBtn(BTN_FLIP, x, y)) flipScreen();
    else if (inBtn(BTN_OFF, x, y)) setScreen(false);
    else if (inBtn(BTN_REBOOT, x, y)) pendingReset = 3;
    else if (inBtn(BTN_RSTSESS, x, y)) pendingReset = 1;
    else if (inBtn(BTN_FACTORY, x, y)) pendingReset = 2;
  }
}

// Touch gesture state, with axis locking so a vertical drag scrolls and a
// horizontal drag flips pages without the two fighting.
static struct {
  bool down;
  int x0, y0, x, y;
  int scroll0;
  unsigned long t0;
  uint8_t axis;     // 0 undecided, 1 horizontal, 2 vertical
  bool swallow;     // touch woke the screen; ignore this gesture
} touch;
static unsigned long lastTouchPoll = 0;
static bool touchSeen = false;

#define TDS_AXIS_LOCK    10   // px of travel before an axis is chosen
#define TDS_SWIPE_MIN    32   // px of horizontal travel = a page flip
#define TDS_FLICK_MIN    18   // px, if thrown fast
#define TDS_FLICK_MS    260   // ...within this time

static bool detailOpen() { return alert.active || detailIdx >= 0 || ambDetail; }

static void pollTouch(unsigned long now) {
  if (now - lastTouchPoll < 16) return;
  lastTouchPoll = now;
  lgfx::touch_point_t tp;
  int n = lcd.getTouch(&tp, 1);
  if (n > 0) {
    if (!touchSeen) {
      touchSeen = true;
      Serial.printf("[tds3] touch controller responding (x=%d y=%d)\n", tp.x, tp.y);
    }
    if (!touch.down) {
      touch.down = true;
      touch.x0 = touch.x = tp.x; touch.y0 = touch.y = tp.y;
      touch.t0 = now;
      touch.axis = 0;
      touch.scroll0 = pageScroll[page];
      touch.swallow = !screenOn;
      if (!screenOn) setScreen(true);
    } else {
      touch.x = tp.x; touch.y = tp.y;
      int dx = touch.x - touch.x0, dy = touch.y - touch.y0;
      if (touch.axis == 0 && (abs(dx) > TDS_AXIS_LOCK || abs(dy) > TDS_AXIS_LOCK))
        touch.axis = (abs(dx) > abs(dy)) ? 1 : 2;
      // live vertical scroll on the list pages
      if (!touch.swallow && touch.axis == 2 && isListPage(page) && !detailOpen())
        pageScroll[page] = touch.scroll0 - dy / TDS_ROW_H;
    }
    lastFrame = 0;
    return;
  }
  if (!touch.down) return;
  touch.down = false;
  if (touch.swallow) return;
  int dx = touch.x - touch.x0, dy = touch.y - touch.y0;
  unsigned long dt = now - touch.t0;
  bool horiz = touch.axis == 1 ||
               (touch.axis == 0 && abs(dx) > abs(dy) && abs(dx) > TDS_AXIS_LOCK);
  bool flip = horiz && (abs(dx) > TDS_SWIPE_MIN ||
                        (abs(dx) > TDS_FLICK_MIN && dt < TDS_FLICK_MS));
  if (flip) {
    if (closeOverlay()) { lastFrame = 0; return; }   // first swipe just clears overlay
    nextPage(dx < 0 ? 1 : -1);
  } else if (abs(dx) < TDS_AXIS_LOCK && abs(dy) < TDS_AXIS_LOCK && dt < 500) {
    handleTap(touch.x0, touch.y0);
  }
  lastFrame = 0;
}

static void pollInput(unsigned long now) {
  int k = pollButton(btnKey, now);
  int b = pollButton(btnBoot, now);
  if ((k || b) && !screenOn) {           // any button wakes the screen
    if (b == 2) return;                  // ...but don't immediately re-sleep
    setScreen(true);
    return;
  }
  if (k == 1) { if (!closeOverlay()) nextPage(1); lastFrame = 0; }
  if (k == 2) cycleBrightness();
  if (b == 1) {
    if (closeOverlay()) { lastFrame = 0; }
    else if (isListPage(page)) {         // page down through the current list
      int rows = listRows();
      pageScroll[page] += rows;
      // wrap when past the end (recomputed next draw via clampScroll)
      if (pageScroll[page] > 400) pageScroll[page] = 0;
      lastFrame = 0;
    } else { nextPage(-1); lastFrame = 0; }
  }
  if (b == 2) setScreen(!screenOn);
  pollTouch(now);
}

// ---------------------------------------------------------------- public
// Birdwatcher boot animation (defines tdsBootAnim()). Must appear after lcd,
// fb, fbOk and the C_* colour tokens are declared, which they are above.
#include "bw_boot_anim.inc"

void tdsInit() {
  pinMode(TDS_PIN_LCD_POWER, OUTPUT);
  digitalWrite(TDS_PIN_LCD_POWER, HIGH);
  pinMode(TDS_PIN_BTN_KEY, INPUT_PULLUP);
  pinMode(TDS_PIN_BTN_BOOT, INPUT_PULLUP);
  analogReadResolution(12);

  prefs.begin("tds3", false);
  brightIdx = prefs.getUChar("bright", 0) % BRIGHT_N;
  rotation  = prefs.getUChar("rot", 1);
  usbFullBright = prefs.getBool("usbbright", true);
  if (rotation != 1 && rotation != 3) rotation = 1;

  lcd.init();
  lcd.setRotation(rotation);
  initColours();
  lcd.fillScreen(C_BG);
  applyBrightness();

  fb.setColorDepth(16);
  fb.setPsram(true);
  fbOk = fb.createSprite(TDS_W, TDS_H) != nullptr;
  g = fbOk ? (lgfx::LovyanGFX*)&fb : (lgfx::LovyanGFX*)&lcd;

  // birdwatcher boot animation only (no boot-log text). It leaves its final
  // brand frame on screen until the live UI takes over on the first tick.
  tdsBootAnim();
  Serial.printf("[tds3] display init, framebuffer=%d rotation=%u\n", fbOk, rotation);
}

void tdsBootLine(const char* msg) {
  // Boot-log text is intentionally not drawn: the boot animation is the only
  // boot-time visual. Kept as a no-op so main.cpp's progress calls still link.
  (void)msg;
}

void tdsAlert(const char* mac, const char* method, uint8_t tier, int8_t rssi,
              uint8_t ch, const char* bleName, int count, bool fresh) {
  bool same = alert.active && strcmp(alert.mac, mac) == 0;
  if (!fresh && !same) return;      // only new MACs / upgrades take the screen
  if (!same || fresh) alert.start = millis();
  alert.active = true;
  strlcpy(alert.mac, mac, sizeof(alert.mac));
  strlcpy(alert.method, method ? method : "", sizeof(alert.method));
  strlcpy(alert.name, bleName ? bleName : "", sizeof(alert.name));
  alert.tier  = tier < 5 ? tier : 0;
  alert.rssi  = rssi;
  alert.ch    = ch;
  alert.count = count;
  alert.skimmer = false;
  detailIdx = -1;
  if (fresh && tier >= 2 && !screenOn) setScreen(true);
  lastFrame = 0;
}

// Full-screen skimmer alert — same prominence as a Flock hit, red, with
// wording that makes clear it is a suspect module, not a confirmed skimmer.
void tdsSkimmerAlert(const char* mac, const char* name, uint8_t score, int8_t rssi) {
  alert.active = true;
  alert.skimmer = true;
  alert.skimScore = score;
  alert.start = millis();
  strlcpy(alert.mac, mac, sizeof(alert.mac));
  strlcpy(alert.name, name ? name : "", sizeof(alert.name));
  alert.method[0] = '\0';
  alert.rssi = rssi; alert.ch = 0; alert.count = 0;
  detailIdx = -1; ambDetail = false;
  if (!screenOn) setScreen(true);
  lastFrame = 0;
}

static void drawSkimmerAlert(unsigned long now) {
  unsigned long el = now - alert.start;
  uint16_t red = C_TIER[4];
  bool flashOn = el < TDS_FLASH_MS && ((el / 150) % 2 == 0);
  uint16_t bg = flashOn ? red : C_BG;
  uint16_t fg = flashOn ? TFT_BLACK : C_TXT;
  g->fillRect(0, TDS_HDR_H, TDS_W, TDS_H - TDS_HDR_H, bg);
  g->fillRect(0, TDS_HDR_H, TDS_W, 4, red);
  g->fillRect(0, TDS_H - 4, TDS_W, 4, red);

  const char* big = (alert.skimScore >= 2) ? "SUSPECT SKIMMER" : "SERIAL BT MODULE";
  g->setTextDatum(lgfx::textdatum_t::top_center);
  g->setFont(&lgfx::fonts::FreeSansBold18pt7b);
  if (g->textWidth(big) > TDS_W - 10) g->setFont(&lgfx::fonts::FreeSansBold12pt7b);
  g->setTextColor(flashOn ? TFT_BLACK : red);
  g->drawString(big, TDS_W / 2, TDS_HDR_H + 8);

  g->setFont(&lgfx::fonts::Font4);
  g->setTextColor(fg);
  g->drawString(alert.name[0] ? alert.name : alert.mac, TDS_W / 2, TDS_HDR_H + 46);

  char buf[64];
  g->setFont(&lgfx::fonts::Font2);
  if (alert.name[0]) { g->drawString(alert.mac, TDS_W / 2, TDS_HDR_H + 72); }
  snprintf(buf, sizeof(buf), "%d dBm   BLE serial module", (int)alert.rssi);
  g->drawString(buf, TDS_W / 2, TDS_HDR_H + 90);
  g->setTextColor(flashOn ? TFT_BLACK : C_SUB);
  g->drawString(alert.skimScore >= 2 ? "known skimmer-module name"
                                     : "raw serial service (0xFFE0)", TDS_W / 2, TDS_HDR_H + 108);
  g->setFont(&lgfx::fonts::Font0);
  g->drawString("possible card skimmer - verify in person", TDS_W / 2, TDS_HDR_H + 126);

  int w = TDS_W * (TDS_ALERT_MS - min<unsigned long>(el, TDS_ALERT_MS)) / TDS_ALERT_MS;
  g->fillRect(0, TDS_H - 4, w, 4, TFT_WHITE);
}

void tdsTick(unsigned long now, uint8_t ch) {
  curCh = ch;
  if (!bootDone) {
    // hold the boot log on screen briefly, then hand over to the UI
    if (now < 2500) { pollInput(now); return; }
    bootDone = true;
  }
  pollInput(now);
  readBattery(now);

  if (now - fpsAt >= 1000) {
    uint32_t f = fyUiFrameCount();
    framesPerSec = (f - fpsLastFrames) * 1000 / (now - fpsAt ? now - fpsAt : 1);
    fpsLastFrames = f;
    fpsAt = now;
  }

  if (alert.active && now - alert.start > TDS_ALERT_MS) { alert.active = false; lastFrame = 0; }
  if (!screenOn) return;
  if (lastFrame && now - lastFrame < TDS_FRAME_MS) return;
  lastFrame = now ? now : 1;

  g->fillScreen(C_BG);
  drawHeader(now);
  if (alert.active)            drawAlert(now);
  else if (page == PAGE_RADAR) drawLive(now);
  else if (page == PAGE_FLOCK) { if (detailIdx >= 0) drawDetail(now); else drawFlock(now); }
  else if (page == PAGE_AP)    { if (ambDetail) drawAmbDetail(now); else drawAmbientList(now, true); }
  else if (page == PAGE_STA)   { if (ambDetail) drawAmbDetail(now); else drawAmbientList(now, false); }
  else if (page == PAGE_BLE)   { if (ambDetail) drawBleDetail(now); else drawBleList(now); }
  else if (page == PAGE_SSID)  drawSsidList(now);
  else                         drawStats(now);

  if (fbOk) fb.pushSprite(0, 0);
}

#endif  // BOARD_LILYGO_T_DISPLAY_S3
