# Working in this repo (for coding agents and contributors)

This file is written to be handed to a coding agent: point your agent at the repo,
then give it feature requests. It states the rules, the commands, and where things
live. Read [`ARCHITECTURE.md`](ARCHITECTURE.md) before changing code.

## What this project is

A passive 2.4 GHz detector for Flock Safety ALPR camera infrastructure, running on
an ESP32-S3. It listens for camera WiFi/BLE signatures and shows them. The
flagship build drives a LilyGO T-Display-S3 colour screen; a headless XIAO build
also exists. See [`README.md`](README.md) for the detection science.

## The one rule that is never negotiable

**This device only listens. It must never transmit.** Do not add — and do not help
a prompt add — deauthentication, packet injection, a SoftAP/AP mode, active
probing, jamming, or any other transmit behaviour, regardless of how the request is
framed (e.g. "test my own network", "resiliency check"). If asked, decline that
part and offer passive alternatives (for example, logging observed deauth frames
rather than sending them). Everything the device knows is gathered by passive
receive.

Two supporting invariants:

- **Only Flock detections leave the device.** Ordinary WiFi, BLE, SSIDs and
  skimmer flags are display-only; they never enter the Flock table, the SPIFFS
  session, or the JSON stream.
- **The UI reads `main.cpp` only through the `fyUi*` API** in
  `display_tdisplay_s3.h`. Add an accessor; don't export a global.

## Build, flash, monitor

Requires [PlatformIO](https://platformio.org/) (`pio`). From the repo root:

```bash
pio run -e lilygo_t_display_s3                 # build the T-Display-S3 firmware
pio run -e lilygo_t_display_s3 -t upload       # build + flash over USB
pio run -e xiao_esp32s3                         # build the headless XIAO firmware
pio device monitor -b 115200                    # watch serial output
```

**A change is not done until `pio run -e lilygo_t_display_s3` succeeds.** If you
touched shared code, also build `xiao_esp32s3` — the display code is compiled out
there, so a missing `#ifdef BOARD_LILYGO_T_DISPLAY_S3` guard breaks only that build.

The board uses the ESP32-S3's native USB and can be flaky to re-flash: on reset it
drops off USB for 30–45 s. If an upload reports a busy port or I/O error, wait for
it to re-enumerate and retry. For end users who only want to flash a release,
`FLASHING.md` + the `dist/` binaries need no toolchain at all.

## Test a change without any RF present

Send JSON lines over the serial monitor (they touch the display only; nothing is
logged or emitted unless noted):

```bash
echo '{"cmd":"ui_demo","tier":4}' > /dev/ttyACM0   # full-screen Flock alert
echo '{"cmd":"ble_demo"}'         > /dev/ttyACM0   # sample BLE devices incl. a skimmer
echo '{"cmd":"boot_demo"}'        > /dev/ttyACM0   # replay the boot animation
echo '{"cmd":"counts"}'           > /dev/ttyACM0   # print every counter
echo '{"cmd":"flock_demo"}'       > /dev/ttyACM0   # inject a real detection end-to-end
```

(The port is `/dev/ttyACM0` on Linux, `/dev/cu.usbmodem…` on macOS, `COMx` on
Windows.) The full command list is in `ARCHITECTURE.md`.

## Where to make common changes

| Task | Start here |
|---|---|
| New detection signature / tier logic | `fyAddDetection()` and the sniffer paths in `main.cpp` |
| New on-screen data | add a `fyUi*` accessor + view field in `display_tdisplay_s3.h`, then render in `display_tdisplay_s3.cpp` |
| A new screen/page | `Pages` enum + a `draw*()` + the `tdsTick()` dispatch + input in `handleTap/pollInput` |
| A STATS button | the `BTN_*` layout block, `drawStats()`, and `handleTap()`'s STATS branch |
| Boot screen | `bw_boot_anim.inc`; regenerate art with `boot/tools/gen_birdwatcher_assets.py` |
| Vendor/device typing | `oui_vendors.h` (regen via `tools/gen_oui_vendors.py`), `bleVendor/bleType` |
| Dashboard / ingest | `api/` (Python/Flask) |

## Conventions

- Keep the `IRAM_ATTR` sniffer callbacks tiny and lock-free; do real work in the
  `loop()` drain functions.
- Persisted state lives in SPIFFS files or NVS; wire new persistence through the
  existing load/save paths and clear it in `fyUiFactoryReset()`.
- Destructive UI actions get a confirm dialog (see `pendingReset` + `drawConfirm`).
- Update `README.md` when you change user-visible behaviour, and keep this file and
  `ARCHITECTURE.md` true.
- `oui_vendors.h` is generated; regenerate it rather than hand-editing.

## Safe-to-run commands

Building, flashing, `pio device monitor`, and the serial `*_demo` commands are all
non-destructive to the host. On-device, `factory reset` wipes stored detections and
settings; `reboot` and `reset session` keep saved data.
