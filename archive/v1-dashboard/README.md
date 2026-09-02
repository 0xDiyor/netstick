# T-Dongle-C5 Dashboard

Firmware for the **LilyGO T-Dongle-C5** (ESP32-C5, dual-band Wi-Fi 6, 16MB flash,
8MB PSRAM, 0.96" ST7735 160x80, microSD, APA102 LED).

A dual-band Wi-Fi survey tool and an ambient status display in one stick, with a
built-in glitching fsociety mask screen. Written against ESP-IDF v5.5.

## Screens

Press the side button (GPIO28) to cycle. Screens that have nothing to show are
skipped automatically.

| Screen | What it shows |
|---|---|
| **SURVEY** | Dual-band scan. Channel-congestion histogram: 2.4GHz (ch 1-14, amber) on the left, 5GHz (ch 36-177, cyan) on the right; your own AP's channel turns green. Header counts APs per band plus how many are 802.11ax. Bottom lists the three strongest APs with RSSI, channel and PHY. Every scan is appended to the SD card as CSV. |
| **@github** | 53x7 contribution heatmap, plus stars, followers, current streak, open PRs, repo count and today's contribution count. |
| **fsociety** | The mask, rasterised from geometry at boot and glitched live — CRT scanlines, slice tearing, RGB channel split, static, and a wordmark that types itself in. Needs no SD card. |
| **clock** | NTP clock and date, link details (SSID, RSSI, band, channel, PHY), a 64-sample ping-latency sparkline with timeouts as red spikes, and drop/loss/uptime counters. |
| **anim** | Plays `.anim` files from `/sd/anim/`. Only appears in the cycle when the card actually has some. |

### Button

| Gesture | Action |
|---|---|
| Tap (< 0.7s) | Next screen |
| Hold (0.7 - 2.5s) | Backlight 100% / 15% |
| Long hold (> 2.5s) | Force a rescan / GitHub refresh on the current screen |

The APA102 mirrors link health: green online, amber high-latency or no IP, red offline.

## Setup

### 1. Toolchain

```sh
git clone --depth 1 -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32c5
python3 $IDF_PATH/tools/idf_tools.py install cmake ninja   # macOS ships neither
. ~/esp/esp-idf/export.sh
```

### 2. Credentials

```sh
cp main/secrets.example.h main/secrets.h
```

Fill in `WIFI_SSID`, `WIFI_PASS`, `GITHUB_USER`, and ideally `GITHUB_TOKEN`.
`secrets.h` is gitignored.

**About the GitHub token.** With one, the firmware makes a single GraphQL request
and gets the whole contribution calendar, star totals and open-PR count. Without
one it falls back to unauthenticated REST: followers, repo and star counts only —
no heatmap, no PR count, and a 60 requests/hour limit. A classic PAT with no
scopes at all is enough for public data.

### 3. Build and flash

```sh
idf.py set-target esp32c5
idf.py -p /dev/cu.usbmodem* flash monitor
```

## The SD card — must be FAT32

ESP-IDF compiles FatFs with `FF_FS_EXFAT 0` and exposes **no Kconfig option to
enable it**. Cards over 32GB ship exFAT from the factory, so a stock 64GB card
will not mount; the boot screen says `SD: no card / not FAT32`.

On macOS:

```sh
diskutil list                                    # find the card, e.g. disk4
diskutil eraseDisk FAT32 DONGLE MBRFormat /dev/disk4
```

Everything except the `anim` screen and CSV logging works fine without a card.

## Animations

`.anim` is a trivial container — a 16-byte header then raw big-endian RGB565
frames, so playback is a DMA straight from the card to the panel with no decoding
on the dongle. See `main/anim.h` for the layout.

```sh
pip3 install --user Pillow
python3 tools/mkanim.py cat.gif                  # -> cat.anim
python3 tools/mkanim.py frames/ --fps 20 --fill
python3 tools/fsociety.py                        # the mask as a file, if you want it on SD
```

Copy the result to `/anim/` on the card. A 160x80 frame is 25.6KB, so budget
about 1.2MB for a 48-frame loop — trivial on a 64GB card.

## Debugging the display without looking at it

`FB_DUMP=1` makes the firmware base64 each rendered frame to the console; the
host turns them back into PNGs. This is how every screen in this repo was checked.

Enable `FB_DUMP` under *T-Dongle-C5 dashboard* in menuconfig (it is a Kconfig
option so that toggling it actually rebuilds — an env var would stick in the
CMake cache):

```sh
idf.py menuconfig            # T-Dongle-C5 dashboard -> Dump rendered frames
idf.py build flash
python3 tools/fbview.py --seconds 90 --out /tmp/screens --save-raw /tmp/raw.txt
python3 tools/fbview.py --file /tmp/raw.txt --out /tmp/screens   # re-parse offline
```

## Hardware notes

- **No USB-OTG.** The C5 has only a USB Serial/JTAG controller, so despite the
  USB-A shell this cannot act as a keyboard, mass-storage device or Ethernet
  gadget. To a host it is a CDC serial port.
- **One radio.** The C5 cannot use 2.4GHz and 5GHz at the same time. In AP+STA
  mode the SoftAP is pinned to whatever band the station is on, so "join 5GHz,
  rebroadcast 2.4GHz" is not possible.
- **LCD and SD share SPI2** (MOSI 2 / MISO 7 / SCK 6) with separate CS lines
  (LCD 10, SD 23). The bus is initialised once in `board_spi_init()`.
- **Backlight is GPIO0 and active low**, driven by LEDC so it can be dimmed.
- **Button is GPIO28**, active low with an internal pull-up.
- The APA102 has dedicated pins (CI 4, DI 5) and shares nothing.

### Scanning interacts with the link

A full sweep visits 42 channels and takes roughly 5 seconds, during which an
associated link is off-channel — expect a latency spike on the clock screen's
sparkline. Scans therefore only run while the SURVEY screen is showing. The
driver also refuses to scan while an association attempt is in flight, so
`netmgr_scan_hold()` pauses the reconnect loop around each sweep.

## Restoring the stock firmware

LilyGO's factory image is kept in `stock-firmware/`:

```sh
python -m esptool --chip esp32c5 -p /dev/cu.usbmodem* write_flash 0x0 \
    stock-firmware/T-Dongle-C5-Factory_V1.4_0616.bin
```

## Layout

```
main/
  app_main.c    boot sequence and the on-panel boot log
  board.h       pin map
  display.c     ST7735 init + framebuffer blit over esp_lcd
  gfx.c         drawing primitives and 5x8 text
  fsociety.c    the mask: geometry rasteriser + live glitch effects
  scanner.c     dual-band survey, channel histograms, CSV logging
  netmgr.c      Wi-Fi station, reconnect backoff, SNTP, ping
  github.c      GraphQL / REST client and contribution parsing
  anim.c        .anim playback from SD
  storage.c     SD over SPI
  led.c         APA102
  ui.c          screen manager, button, render and worker tasks
tools/
  mkanim.py     GIF/images -> .anim
  fsociety.py   generates the mask animation as a file
  fbview.py     framebuffer dumps -> PNG
  animfmt.py    shared .anim writer
```
