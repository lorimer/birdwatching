#pragma once

// LilyGO T-Display-S3 (touch) UI for flock-you.
// Built only for the lilygo_t_display_s3 env (BOARD_LILYGO_T_DISPLAY_S3).

#include <stdint.h>

#ifdef BOARD_LILYGO_T_DISPLAY_S3

// Read-only snapshot of one row of main.cpp's detection table.
struct TdsDetView {
  const char* mac;
  const char* method;
  const char* name;      // SSID or BLE advertised name, "" if none
  uint8_t     tier;
  int8_t      rssi;
  uint8_t     channel;   // 0 for BLE
  uint32_t    firstSeen; // millis()
  uint32_t    lastSeen;  // millis()
  uint16_t    count;
};

#define TDS_PROBE_MAX 4   // probed SSIDs retained per client

// Ordinary (non-Flock) WiFi transmitter, display only.
struct TdsAmbView {
  uint8_t  mac[6];      // BSSID (AP) or station MAC (client)
  uint8_t  bssid[6];    // AP a client is associated with (clients only)
  uint8_t  hasBssid;
  int8_t   rssi;
  uint8_t  ch;
  uint8_t  ap;          // 1 = access point
  uint8_t  prev;        // 1 = restored from previous session
  char     ssid[24];    // AP network name
  char     probed[TDS_PROBE_MAX][24];  // client: networks it has probed for
  uint8_t  probedCount;
  uint32_t lastSeen;
  uint32_t firstSeen;
  uint16_t count;
};

// One detected network name (from a beacon or a probe request).
struct TdsSsidView {
  char     ssid[24];
  uint8_t  bssid[6];    // AP BSSID, or the client MAC that probed for it
  int8_t   rssi;
  uint8_t  ch;
  uint8_t  fromAp;      // 1 = advertised by an AP; 0 = only seen in probes
  uint8_t  prev;
  uint16_t count;
  uint32_t lastSeen;
};

// Ordinary (non-Flock) BLE advertiser, display only.
struct TdsBleView {
  uint8_t  mac[6];
  uint8_t  addrType;  // 0 public, 1 random, 2 public-id, 3 random-id
  int8_t   rssi;
  uint16_t appearance;
  uint16_t company;
  uint8_t  svc;
  uint8_t  skimmer;   // 0 none, 1 raw serial service, 2 known serial-module name
  char     name[24];
  uint8_t  prev;
  uint32_t lastSeen;
  uint32_t firstSeen;
  uint16_t count;
};

// Display side (display_tdisplay_s3.cpp)
void tdsInit();
void tdsBootAnim();
void tdsBootLine(const char* msg);
void tdsAlert(const char* mac, const char* method, uint8_t tier, int8_t rssi,
              uint8_t ch, const char* bleName, int count, bool fresh);
void tdsSkimmerAlert(const char* mac, const char* name, uint8_t score, int8_t rssi);
void tdsTick(unsigned long now, uint8_t ch);

// Data side (implemented in main.cpp, loop() context only)
int      fyUiDetCount();
bool     fyUiGetDet(int idx, TdsDetView* out);
uint8_t  fyUiBeepMask();
bool     fyUiSpiffsReady();
uint32_t fyUiFrameCount();
int      fyUiAmbCount();
bool     fyUiGetAmb(int idx, TdsAmbView* out);
int      fyUiBleCount();
bool     fyUiGetBle(int idx, TdsBleView* out);
int      fyUiSsidCount();
bool     fyUiGetSsid(int idx, TdsSsidView* out);

// Odometer counters for the RADAR summary (session + persisted lifetime).
enum TdsCat { TDSC_FLOCK, TDSC_AP, TDSC_CLIENT, TDSC_BLE, TDSC_SKIM, TDSC_SSID, TDSC_N };
uint32_t fyUiSess(int cat);
uint32_t fyUiLife(int cat);
void     fyUiResetSession();
void     fyUiResetLife();
void     fyUiFactoryReset();
void     fyUiLoadLife();

#else

static inline void tdsBootLine(const char*) {}

#endif
