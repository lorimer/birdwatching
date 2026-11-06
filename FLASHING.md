# Flashing the detector onto a LilyGO T-Display-S3

This guide takes you from a brand-new (or unknown) board to a working detector.
**No prior experience is assumed.** It works on **Windows, macOS, and Linux**, and
it does not matter what firmware (if any) is on the board now — the first step
wipes it clean.

If you only want to flash the board, you need **Part 1, Part 2, and one of Part 3
or Part 4**. The rest is reference.

---

## Part 1 — What you need

- **The board:** a *LilyGO T-Display-S3* or *T-Display-S3 Touch* (an ESP32-S3 with
  a small colour screen). This firmware is built for that exact board.
- **A USB-C cable that carries data.** Many cheap cables only charge. If your
  computer never shows the board when you plug it in, suspect the cable first.
- **A computer** running Windows, macOS, or Linux.
- **The firmware file:** `dist/flock-you-tdisplay-s3-merged.bin` from this
  repository. That one file contains everything (bootloader, partition table, and
  app). Its SHA-256 checksum is in `dist/SHA256SUMS.txt` if you want to verify the
  download is intact.

You do **not** need to install Arduino, PlatformIO, or any development tools to
flash a pre-built release. Those are only for building the firmware yourself
(see Part 8).

---

## Part 2 — Find the board's two buttons

The board has two tiny buttons on the long edge, near the USB-C connector. One is
usually marked **RST** (reset) and the other **BOOT** (sometimes **BOT**, or
**IO0**). You will use them to put the board into "download mode" if your computer
cannot flash it automatically.

**Download mode (memorize this move):**

1. Press and **hold BOOT**.
2. While still holding BOOT, briefly press and release **RST**.
3. Release **BOOT**.

The screen goes dark and the board waits silently to be flashed. This is normal.
After flashing, a single press of **RST** runs the new firmware.

> The T-Display-S3 talks to your computer over the ESP32-S3's built-in USB, so no
> special driver is needed on current versions of Windows, macOS, or Linux. If the
> computer does not react at all when you plug in, try a different cable and a
> different USB port before anything else.

---

## Part 3 — Easiest method: flash from your web browser (no install)

This uses Espressif's official web flasher. Nothing is installed, and it runs the
same tool as the command line underneath.

**You must use Google Chrome or Microsoft Edge on a desktop computer.** Firefox and
Safari cannot talk to USB devices, and phones/tablets will not work.

1. Download `dist/flock-you-tdisplay-s3-merged.bin` from this repo to your computer
   and remember where it saved (e.g. your Downloads folder).
2. Plug the board into your computer with the data cable.
3. Open **https://espressif.github.io/esptool-js/** in Chrome or Edge.
4. Leave the baud rate at its default and click **Connect**. A small window lists
   your serial ports. Pick the board's port and click **Connect**.
   - On **Windows** it looks like `COM5` (or `USB JTAG/serial debug unit`).
   - On **macOS** it looks like `/dev/cu.usbmodem…`.
   - On **Linux** it looks like `/dev/ttyACM0`.
   - If no port appears, or Connect fails, do the **download mode** move from
     Part 2 and click Connect again.
5. Click **Erase Flash** and wait for it to finish. This removes whatever was on
   the board before, no matter where it came from.
6. In the file area, set the **Flash Address** to `0x0` and click **Choose File**,
   then select `flock-you-tdisplay-s3-merged.bin`. Make sure there is exactly one
   file row and its address is `0x0`.
7. Click **Program** and wait. When it says it is done, press the **RST** button
   on the board once.

The board restarts and plays the boot animation (Part 6).

---

## Part 4 — Command-line method (works on every OS)

Use this if you prefer a terminal, or if the browser method is not available.

### 4a. Install the flashing tool (once)

You need **Python 3** and the `esptool` package.

- **Windows:** install Python from https://www.python.org/downloads/ and tick
  *"Add Python to PATH"* during setup. Then open **Command Prompt** and run:
  ```
  pip install esptool
  ```
- **macOS:** Python 3 is usually present. In **Terminal** run:
  ```
  python3 -m pip install esptool
  ```
- **Linux:** in a terminal run:
  ```
  python3 -m pip install esptool    # or: sudo apt install esptool
  ```

Check it installed:
```
esptool.py version
```
(If `esptool.py` is not found, try `python3 -m esptool version`.)

### 4b. Find the board's port

Plug the board in, then:

- **Windows:** open *Device Manager* → *Ports (COM & LPT)* and note the `COMx`
  number (for example `COM5`).
- **macOS:** run `ls /dev/cu.usbmodem*` — you want something like
  `/dev/cu.usbmodem101`.
- **Linux:** run `ls /dev/ttyACM*` — usually `/dev/ttyACM0`.

> **Linux permission note:** if you get a "permission denied" error on the port,
> add yourself to the serial group once with
> `sudo usermod -aG dialout $USER`, then log out and back in. A quick one-off
> alternative is to run the commands below with `sudo`.

### 4c. Erase, then flash

Replace `PORT` with your port from above (`COM5`, `/dev/cu.usbmodem101`,
`/dev/ttyACM0`, …). Run both commands from the folder that contains the `.bin`.

```
esptool.py --chip esp32s3 --port PORT erase_flash
esptool.py --chip esp32s3 --port PORT write_flash 0x0 flock-you-tdisplay-s3-merged.bin
```

If a command cannot connect, do the **download mode** move from Part 2 and run it
again. When flashing finishes, press **RST** on the board once.

> **Flashing the parts separately (advanced).** The merged file above is all you
> need. If you prefer the individual images, they are in `dist/components/`:
> ```
> esptool.py --chip esp32s3 --port PORT write_flash \
>   0x0 components/bootloader.bin \
>   0x8000 components/partitions.bin \
>   0xe000 components/boot_app0.bin \
>   0x10000 components/firmware.bin
> ```

---

## Part 5 — Did it work?

Immediately after flashing, press **RST**. Within a second the screen lights up and
plays a short **boot animation** (a rifle scope that hunts across a scene and locks
onto a camera-headed bird, then a "BIRD / WATCHER" logo wipes in). It lasts about
three seconds and then the live **RADAR** screen appears. If you see that, you are
done.

If the screen stays black, see Part 7.

---

## Part 6 — Using the detector

- **Screens:** swipe left/right to move between pages, or tap the tab name at the
  top. Pages are **RADAR**, **FLOCK**, **AP**, **STA** (client stations), **BLE**,
  **SSID**, and **STATS**.
- **RADAR:** shows everything heard in the last 20 seconds, placed by signal
  strength, with a count grid. **Tap the radar** to switch the grid between
  *in range now*, *since reset*, and *all time*.
- **Buttons:** the **KEY** button short-press moves to the next page; the **BOOT**
  button short-press moves back. A long press on BOOT turns the screen off and on.
- **Brightness and resets** live on the **STATS** page, including a "full
  brightness while on USB" option and a factory reset (behind a confirm dialog).
- Full feature details are in [`README.md`](README.md) under *LilyGO T-Display-S3*.

This device **only listens**. It never transmits, never joins a network, and never
attacks anything. It is a passive receiver.

---

## Part 7 — Troubleshooting

**Nothing happens when I plug in / no port appears.**
Try a different USB-C cable (many are charge-only) and a different port. Then do the
download-mode move from Part 2 and look again.

**The flasher cannot connect or times out.**
Put the board in download mode (Part 2) *before* clicking Connect or running the
command, then try again. Close any other program that might be holding the port
(Arduino Serial Monitor, PlatformIO monitor, another browser tab of the flasher).

**It flashed, but the screen is black.**
Press **RST** once. If still black, re-run the **erase** step and flash again — a
leftover from previous firmware can occasionally interfere, which the erase clears.

**Windows shows the board but flashing fails partway.**
Lower the speed: add `--baud 115200` to the command, or pick 115200 in the web
flasher before connecting.

**I want to check my download is genuine.**
Compare its checksum to `dist/SHA256SUMS.txt`:
`sha256sum flock-you-tdisplay-s3-merged.bin` (Linux),
`shasum -a 256 …` (macOS), or `certutil -hashfile … SHA256` (Windows).

---

## Part 8 — (Optional) Build the firmware yourself

Only needed if you want to change the code. Install
[PlatformIO](https://platformio.org/) (the VS Code extension is easiest), then from
the repo folder:

```
pio run -e lilygo_t_display_s3 -t upload   # build and flash
pio device monitor -b 115200               # watch its output
```

To rebuild the single-file image that this guide hands out:

```
BOOT_APP0=$(find ~/.platformio -name boot_app0.bin | head -1)
B=.pio/build/lilygo_t_display_s3
esptool.py --chip esp32s3 merge_bin -o dist/flock-you-tdisplay-s3-merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 $B/bootloader.bin 0x8000 $B/partitions.bin \
  0xe000 $BOOT_APP0 0x10000 $B/firmware.bin
```

The boot animation and its source art, plus the script that regenerates it, are in
[`boot/`](boot/); the firmware side is `bw_boot_anim.inc` with `bw_scene_png.h` and
`bw_wordmark_png.h`. See [`BOOT_SCREEN_HANDOFF.md`](BOOT_SCREEN_HANDOFF.md) for how
the boot screen is wired in.
