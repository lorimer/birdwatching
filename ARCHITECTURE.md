# Architecture

A map of the firmware for anyone (human or coding agent) making changes. Pair this
with [`AGENTS.md`](AGENTS.md) for the working rules and commands.

## Two build targets

`platformio.ini` defines two environments. Both share `main.cpp`; the display file
differs.

| Env | Board | Source filter | Display |
|---|---|---|---|
| `xiao_esp32s3` | Seeed XIAO ESP32-S3 | `main.cpp` only | headless (buzzer + LED) |
| `lilygo_t_display_s3` | LilyGO T-Display-S3 / Touch | `main.cpp` + `display_tdisplay_s3.cpp` | 320x170 colour UI |

The T-Display-S3 code is gated behind `#ifdef BOARD_LILYGO_T_DISPLAY_S3`, set by a
build flag in that env. The XIAO build never compiles the display code, so any new
`tds*` call from `main.cpp` must sit inside that `#ifdef`, and anything the XIAO
path still needs a symbol for gets a stub in the `#else` of `display_tdisplay_s3.h`.

## File map

| Path | Role |
|---|---|
| `main.cpp` | Detection core, data model, persistence, serial protocol. Hardware-agnostic. |
| `display_tdisplay_s3.cpp` | The entire T-Display-S3 UI: pages, rendering, touch/button input, boot animation, alerts. |
| `display_tdisplay_s3.h` | The firmware↔UI boundary: view structs and the `fyUi*` accessor API. **This is the contract.** |
| `oui_vendors.h` | Generated IEEE OUI vendor DB (~40k prefixes) in PROGMEM. Rebuilt by `tools/gen_oui_vendors.py` from `oui.txt`. |
| `bw_boot_anim.inc`, `bw_scene_png.h`, `bw_wordmark_png.h` | Boot animation (`tdsBootAnim()`) and its embedded art. Source + regen script in `boot/`. |
| `display_dongle.cpp/.h` | T-Dongle-S3 UI, present but not wired to an env yet. |
| `api/` | Flask dashboard that ingests the JSON detection stream over USB. |
| `datasets/` | Signature provenance docs. |
| `dist/` | Pre-built binaries for flashing without a toolchain (see `FLASHING.md`). |

## Runtime model

`setup()` brings up the display, loads persisted state, then starts the radios.
`loop()` drains the capture rings, runs the BLE scan, and ticks the UI. There are
no FreeRTOS tasks of our own; everything runs in `loop()` plus the two radio
callbacks.

**The capture pattern (do not break this):**

1. `wifiSniffer()` runs in the WiFi driver's context and is `IRAM_ATTR`. It must
   stay lock-free and tiny: it parses the frame and calls `ambPush()` (ordinary
   WiFi) or hands Flock hits onward. `ambPush()` only writes into a ring buffer.
2. `loop()` calls `drainAmbient()` / `drainAmbientBle()`, which move ring entries
   into the real tables (`AmbEntry`, `BleEntry`, `SsidEntry`) where allocation,
   dedup and sorting are allowed.
3. BLE advertisements arrive on a NimBLE scan callback and `blePush()` into their
   own ring, drained the same way.

Flock detections go through `fyAddDetection()`, which is the single chokepoint that
bumps counters, updates the unique MAC set, persists, alerts the UI, and emits the
JSON line. New detection logic should funnel through it, not around it.

WiFi RX and the BLE scan share the 2.4 GHz radio; the code deliberately
time-slices them. Channel hopping is 11/6/1 descending at a 250 ms dwell.

## Data model and the UI boundary

`main.cpp` owns all state. The UI never reaches into it directly: it reads through
the `fyUi*` functions declared in `display_tdisplay_s3.h`. Examples:

- `fyUiDetCount()` / `fyUiGetDet()` — Flock detections
- `fyUiAmbCount()` / `fyUiGetAmb()` — ordinary WiFi (APs and clients)
- `fyUiBleCount()` / `fyUiGetBle()`, `fyUiSsidCount()` / `fyUiGetSsid()`
- `fyUiSess(cat)` / `fyUiLife(cat)` — the RADAR odometers, per `TdsCat`
- `fyUiResetSession()`, `fyUiResetLife()`, `fyUiFactoryReset()`, `fyUiLoadLife()`

Each getter fills a flat view struct (`TdsDetView`, `TdsAmbView`, `TdsBleView`,
`TdsSsidView`) so the UI sees a snapshot, not live internals. **To surface new data
on the screen, add a `fyUi*` accessor and a view field; don't export a global.**

Counters: `cntSess[]` (since reset) and `cntLife[]` (persisted lifetime) are
odometers of first-live sightings. Flock and skimmer all-time are different: they
are true unique tallies backed by MAC sets (`flockSeen`, `skimSeen`) stored in
`/seen.bin`, so `fyUiLife(TDSC_FLOCK)` returns the set size, not an odometer.

## Persistence

| Store | Holds |
|---|---|
| SPIFFS `/session.json` (+ `.tmp`, `/prev_session.json`) | Flock session, CRC-enveloped, atomically promoted |
| SPIFFS `/ambient.bin` | Ambient WiFi + BLE + SSID tables (raw structs), saved ≤ every 2 min |
| SPIFFS `/seen.bin` | Unique Flock + skimmer MAC sets |
| NVS (`Preferences`) | Brightness, orientation, USB-full flag, beep mask, lifetime counters |

`fyUiFactoryReset()` clears all of these.

## Serial protocol

One JSON object per line, in and out. Outbound: each Flock detection is a JSON line
in the schema `api/flockyou.py` expects. Inbound commands (`handleCommandLine()`):

| Command | Effect |
|---|---|
| `{"cmd":"get_config"}` | Dump current config |
| `{"cmd":"set_beep","tier":N,"on":0/1}`, `{"cmd":"set_beep_mask","mask":N}` | Audio mask |
| `{"cmd":"counts"}` | Print every counter (diagnostic) |
| `{"cmd":"seen"}` | Dump stored unique Flock + skimmer MACs |
| `{"cmd":"dump_session","source":"live"|"prev"}` | Stream a stored session for dashboard import |
| `{"cmd":"ui_demo","tier":N}` | Display-only test Flock alert *(T-Display only)* |
| `{"cmd":"ble_demo"}` | Inject sample BLE devices incl. a skimmer *(T-Display only)* |
| `{"cmd":"flock_demo"}` | Inject a real detection through `fyAddDetection()` |
| `{"cmd":"boot_demo"}` | Replay the boot animation *(T-Display only)* |

The `*_demo` commands are the way to exercise the UI with no RF present.

## Hard constraints

- **Passive receive only.** The radio never transmits. Do not add deauth, packet
  injection, an AP/SoftAP, or any active probing. This is a firm project rule, not
  a TODO.
- **Everything but Flock is display-only.** Ordinary WiFi, BLE, the SSID registry
  and skimmer flags must never enter the Flock table, the SPIFFS session, or the
  JSON stream, so the dashboard still sees only Flock hits.
- **Keep IRAM callbacks lock-free.** Push to a ring; do real work in the `loop()`
  drain.
