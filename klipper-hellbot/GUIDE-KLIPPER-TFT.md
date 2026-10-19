# Complete guide: Klipper + MKS TFT35 + touch on MKS Robin Nano V1.2

This guide explains how to reproduce **from scratch** the complete system in
this fork: Klipper running the printer, with the stock display (MKS TFT35,
ILI9488 480×320 over FSMC) showing 6 configurable buttons and a **working
touchscreen** — no display replacement, no extra hardware.

> **Reference hardware**: Hellbot Magna 2 300 (or any printer with an
> MKS Robin Nano V1.2 board, STM32F103VET6 MCU, 8 MHz crystal, MKS 28 KiB
> bootloader). The TFT is the panel wired to the board's FSMC connector.
> The touch layer is the resistive XPT2046 that comes with the panel itself.

---

## 1. How it works (architecture)

```
printer.cfg ([hbtft_buttons])                 ┌─ MCU (this fork) ──────────────┐
        │ labels, gcodes, colors,             │ src/stm32/hbtft.c:             │
        │ touch calibration, menu pages       │  - FSMC → ILI9488 (480×320)    │
        ▼                                     │  - 5x7 font + 6 buttons        │
klippy/extras/hbtft_buttons.py (on the Pi) ─► │  - XPT2046 touch (SPI2 h/w)    │
  - sends hb_label (button text/color)        │  - everything in DECL_TASK,    │
  - receives hb_button (raw x,y on touch)     │    never blocks steppers       │
  - maps raw→button (calibration in cfg)      └────────────────────────────────┘
  - runs the button's gcode                   host ←→ MCU: USART3 (CH340 USB)
```

Key design points:

- **Touch calibration lives in `printer.cfg`, not in the firmware**: the MCU
  reports raw ADC coordinates and the Python module does the mapping.
  Recalibrating = edit config + `RESTART`, no recompiling.
- **Menu pages are host-only too**: a button whose gcode is `PAGE:N` makes
  the host re-send 6 labels; the firmware redraws the buttons instantly.
- The TFT is drawn **progressively in batches** inside `DECL_TASK` so the
  IWDG watchdog of Klipper never times out (drawing everything at once in
  `DECL_INIT` is the classic cause of an infinite bootloop).
- Klipper **does not** use the display for anything else: the Pi is the main
  interface; the TFT is a standalone quick-access panel.

---

## 2. Building the firmware (Linux PC)

### 2.1 Dependencies

```bash
sudo apt install build-essential git python3 python3-pip \
     gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
```

> If your distro ships an old toolchain, a verified alternative is the
> PlatformIO one
> (`~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-`).

### 2.2 Compile

```bash
git clone https://github.com/adrianeb2015/klipper.on.hellbot.magna.2.300.git
cd klipper.on.hellbot.magna.2.300/klipper-hellbot

# Prebuilt config: STM32F103, 8 MHz, USART3, 28 KiB bootloader, HB_TFT
cp .config.hellbot .config
make -j4 robin
```

The `robin` target builds **and signs** automatically with
`scripts/update_mks_robin.py`. The output is:

```
out/Robin_nano35.bin   ← this file goes to the SD card
```

> ⚠️ **Never flash the raw `out/klipper.bin`**: the MKS bootloader rejects
> it. Always use `Robin_nano35.bin` (signed).

### 2.3 Flashing from SD

1. Copy `out/Robin_nano35.bin` to the root of a **clean** microSD
   (FAT32, only that file on it).
2. Printer **off** → insert the SD → power on.
3. A beep sounds and the screen stays black for ~1 minute while flashing.
4. Power off → **remove the SD** (the bootloader renames the bin to
   `.CUR`; leaving it in re-flashes on every boot) → power on.

On boot you should see the TFT scene: **6 colored rectangles (3×2)**
with the labels from `printer.cfg` (or `BTN1..BTN6` by default).

---

## 3. Host setup on the Raspberry Pi

### 3.1 Base Klipper

Install with KIAUH (simplest) or manually:

```bash
git clone https://github.com/dw-0/kiauh.git
./kiauh/kiauh.sh   # → 1) Install → 1) Klipper (+ Moonraker + Mainsail/Fluidd)
```

**Important**: in KIAUH, when asked about the microcontroller choose
*build later* — you already have the binary from this fork.

### 3.2 Connection

The board's USB port (CH340) maps to **USART3**:

```bash
ls /dev/serial/by-id/
# something like: usb-1a86_USB_Serial-if00-port0  (CH340)
```

In `printer.cfg`:

```ini
[mcu]
serial: /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
baud: 250000
```

### 3.3 Button module

Copy the extra into the installed Klipper:

```bash
cp klipper-hellbot/klippy-extras/hbtft_buttons.py ~/klipper/klippy/extras/
```

### 3.4 printer.cfg

Start from the board's generic config
(`klipper-hellbot/config/generic-mks-robin-nano-v1.cfg` or `-v2` depending
on your board revision) and add your axes/heater/extruder. Then append:

```ini
[hbtft_buttons]
# ── Page 1 (root) ──
btn1_label: MENU
btn1_gcode: PAGE:2          # special gcode: switch to page 2
btn1_color: 0xFF0000

btn2_label: HOME
btn2_gcode: G28
btn2_color: 0x00AA00

btn3_label: UNLOAD
btn3_gcode: M702
btn3_color: 0x00AA00

btn4_label: LOAD
btn4_gcode: M701
btn4_color: 0x0000FF

btn5_label: PURGE
btn5_gcode: PURGE
btn5_color: 0xAAAA00

btn6_label: OFF
btn6_gcode: TURN_OFF_HEATERS
btn6_color: 0x666666

# ── Page 2 (menu) ──
page2_btn1_label: BACK
page2_btn1_gcode: PAGE:1    # go back to page 1
page2_btn1_color: 0x666666
page2_btn2_label: HEAT
page2_btn2_gcode: SET_HEATER_TEMPERATURE HEATER=extruder TARGET=200
page2_btn2_color: 0xAA3300
page2_btn3_label: BED
page2_btn3_gcode: SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60
page2_btn3_color: 0xAA5500
# ... page2_btn4/5/6 the same way

# ── Touch calibration (ALL here, no firmware rebuild) ──
# Defaults are reasonable for this panel; fine-tune per section 4.
xpt_x_min: 150
xpt_x_max: 1850
xpt_y_min: 220
xpt_y_max: 1830
flip_x: False
flip_y: True     # this panel ships with Y inverted
```

Notes:

- `btnN_color` accepts **only** `0xRRGGBB` (raw RGB565 is not accepted: it
  is ambiguous).
- Labels: up to 16 characters per line, displayed uppercase.

### Dynamic labels: live values on a button

A label may contain templates like `{object.field}`, refreshed every
second by the host module (no firmware rebuild):

```ini
btn5_label: HEAT {extruder.temperature}
btn6_label: BED {heater_bed.temperature}
```

`object` is any klippy printer object with `get_status()` — e.g.
`extruder`, `heater_bed`, `temperature_sensor ...`. The value is shown as
an integer. Buttons with templates (and the pause button below) are
re-rendered once per second and redrawn on the TFT.

### Two-line labels and big font

Labels support a second line using `\n` in the config value, and a big
font (scale 3, ~15×21 px) for the first line:

```ini
btn1_label: HEAT\n{extruder.temperature}
btn2_label: TUNE
btn2_big_font: True        # first line rendered big
```

Or globally with `big_font: True` under `[hbtft_buttons]`. Requires the
`hb_label2` firmware from this fork (older firmware silently falls back to
single-line labels).

### Automatic PAUSE / RESUME button

A button whose gcode is the special value `PAUSE_BUTTON` toggles
automatically with the print state:

```ini
btn3_label: PAUSE          # initial label (overwritten at runtime)
btn3_gcode: PAUSE_BUTTON
```

- While printing normally the button reads **PAUSE** and sends `PAUSE`.
- While paused it reads **RESUME** and sends `RESUME`.
- The swap happens within ~1 second of the state change, and the button
  works on any menu page.
- **Menu pages**: page 1 is the classic `btnN_*` set. Extra pages are
  defined as `pageN_btnM_label/gcode/color` (as many pages as you want).
  A button with gcode `PAGE:N` switches to page N; the host re-sends the 6
  labels and the TFT redraws instantly. You can also switch from the
  console or a macro: `HBTFT_PAGE PAGE=2`.

```bash
sudo systemctl restart klipper
```

---

## 4. Calibrating the touch (no recompiling)

1. Run in the Mainsail/Fluidd console:

   ```
   HBTFT_TOUCH_DEBUG
   ```

2. Tap each **button** and check the log
   (`~/printer_data/logs/klippy.log`):

   ```
   hbtft_buttons: TOUCH raw=(823,1702) -> pixel=(185,32) boton=1
   ```

   If `boton=N` does not match the button you tapped, calibration is needed.

3. Tap the **4 corners of the screen** and note the min/max raw values per
   axis. Put them in `printer.cfg`:

   | Tap | What to note |
   |---|---|
   | left corner  | smallest raw X → `xpt_x_min` |
   | right corner | largest raw X → `xpt_x_max` |
   | top corner   | Y extreme depending on `flip_y` |
   | bottom corner| the other Y extreme |

4. If an axis moves mirrored, flip `flip_x` / `flip_y`.

5. `RESTART` and repeat the button test. Done — any future recalibration is
   just editing these 6 values.

**Diagnostics**: if the MCU's own mapping disagrees with the host
calibration, the log shows `MCU said btn=N, host calibration says btn=M`
and **the host wins**.

---

## 5. Rebuilding the fork from scratch (for developers)

If you want to start from upstream Klipper and apply the changes yourself,
this fork's full delta against `Klipper3d/klipper` is:

| File | Change |
|---|---|
| `src/stm32/hbtft.c` | **new**: FSMC ILI9488 driver + font + XPT2046 + `hb_label`/`hb_button` protocol |
| `src/stm32/Makefile` | add `hbtft.c` to the build |
| `Makefile` | `robin` target: `make` + `python scripts/update_mks_robin.py out/klipper.bin out/Robin_nano35.bin` |
| `.config.hellbot` | ready-to-use Kconfig |
| `klippy-extras/hbtft_buttons.py` | **new**: host module (copy to `klippy/extras/`) |
| `config/generic-mks-robin-nano-v1.cfg` | documented `[hbtft_buttons]` section |

The essential Kconfig (the classic trap of this board):

```
CONFIG_MCU="stm32"
CONFIG_MCU_STM32F103=y
CONFIG_BOARD_MKS_ROBIN_NANO=y        # or generic, plus these 2 manually:
CONFIG_MCU_STM32F103VE=y
CONFIG_CLOCK_FREQ=72000000
CONFIG_SERIAL=y
# KEY! the MKS bootloader occupies 28 KiB:
CONFIG_STM32_FLASH_START_7000=y
# This board's CH340 lands on USART3 (PB10/PB11):
CONFIG_STM32_SERIAL_USART3=y
CONFIG_HB_TFT=y                      # this fork's driver
```

Without `FLASH_START_7000` the binary links at `0x08002000` and **nothing
ever connects** (failure #1 when porting to the Robin Nano).

### Lessons learned (if you port to another board/panel)

1. **Watchdog**: panel init as a state machine with deadlines
   (`timer_from_us()`), progressive drawing per `DECL_TASK` call. A long
   busy-wait during init = infinite bootloop via IWDG.
2. **FSMC addresses**: `REG=0x6001FFFE` (A16=0, command) and
   `RAM=0x60020000` (A16=1, data). If data goes to `0x60000000` every pixel
   gets interpreted as garbage commands.
3. **ILI9488 windowing** (CASET/PASET): parameters as **4 single-byte
   writes**, not two 16-bit words.
4. **Panel init**: use Marlin's exact sequence
   (`lcd/tft_io/ili9488.h`), including PWCTRL/VMCTRL/gammas and `INVOFF`.
5. **XPT2046 on hardware SPI**: without the `SSI` bit (CR1 bit 8) with NSS
   on GPIO, the SPI enters silent mode fault. Real frame:
   `[cmd][junk][D11..D4][D3..D0+0000]` → 12-bit value
   `(b2<<4 | b3>>4) & 0xFFF`. Discard the first conversion of each axis
   and average 4 samples.

---

## 6. Quick troubleshooting

| Symptom | Likely cause |
|---|---|
| White screen after flashing | incomplete panel init or corrupt flash → reflash with a clean SD |
| Bootloop (beeps every few seconds) | busy-wait exceeds IWDG, or wrong flash start (see §5) |
| Screen OK but the Pi sees no port | FW must be **USART3**, baud 250000 |
| Labels don't change | extra not in `klippy/extras/`, or `klippy.log` says the response API wasn't found |
| Touch does nothing | check for `hbtft_buttons: message received` in the log; if absent, response registration failed |
| Touch picks the wrong button | calibration (§4) — almost always `flip_y` or the min/max values |

Serial markers (`miniterm.py /dev/serial/by-id/... 250000 --raw`):
`H` = driver boot, `G` = scene complete, `0`-`6` = button tapped.

Console commands provided by the host module:

| Command | Effect |
|---|---|
| `HBTFT_TOUCH_DEBUG` | toggles detailed touch logging (raw → pixel → button) |
| `HBTFT_PAGE PAGE=N` | switches the TFT to menu page N |

## 7. Status

| Component | Status |
|---|---|
| Base FW (steppers, serial, all of Klipper) | ✅ |
| ILI9488 TFT over FSMC (buttons + labels + colors) | ✅ |
| Touch: press detection and raw→button mapping in host | ✅ |
| Calibration without recompiling (`printer.cfg`) | ✅ |
| Menu pages (host-only, `PAGE:N` / `HBTFT_PAGE`) | ✅ |
| Button macros running gcode | ✅ (host module) |
| Advanced widgets (temperatures on screen) | 🔜 |
