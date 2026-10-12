# Klipper + TFT + Touch on MKS Robin Nano V1.2 (Hellbot Magna 2 300)

Run **Klipper** on a printer with an **MKS Robin Nano V1.2** board while
keeping the stock display: the **MKS TFT35** (ILI9488 480×320 over FSMC)
shows 6 configurable buttons with a **working touchscreen** — no display
replacement, no extra hardware.

📕 **Start here: [Complete installation guide](klipper-hellbot/GUIDE-KLIPPER-TFT.md)**
— firmware build, Raspberry Pi setup, touch calibration and menu pages,
all without ever recompiling after the first flash.

## What you get

| Feature | How |
|---|---|
| Klipper motion/thermal control | fork of upstream Klipper, signed `Robin_nano35.bin` flashed from SD |
| Stock TFT keeps working | `hbtft.c` FSMC/ILI9488 driver, progressive drawing that never trips the watchdog |
| 6 configurable buttons | labels/gcodes/colors from `printer.cfg` (`[hbtft_buttons]`) |
| Touchscreen | XPT2046 driver; raw coordinates reported to the host |
| Touch calibration **without recompiling** | mapping done in the host module, parameters in `printer.cfg` |
| Menu pages | a button with gcode `PAGE:N` switches the whole button grid (host-only, instant) |

## Repository layout

| Path | What it is |
|---|---|
| [`klipper-hellbot/`](klipper-hellbot/) | the Klipper fork (only changes: `src/stm32/hbtft.c`, `robin` make target, `.config.hellbot`) |
| [`klipper-hellbot/GUIDE-KLIPPER-TFT.md`](klipper-hellbot/GUIDE-KLIPPER-TFT.md) | **the complete installation guide** |
| [`klipper-hellbot/klippy-extras/hbtft_buttons.py`](klipper-hellbot/klippy-extras/hbtft_buttons.py) | host module: buttons, menus, touch calibration (copy to `~/klipper/klippy/extras/`) |
| [`klipper/printer.cfg`](klipper/printer.cfg) | real-world `printer.cfg` for the Hellbot Magna 2 300, usable as a template |

## Quick start

```bash
# 1. Build the firmware (Linux PC)
git clone https://github.com/adrianeb2015/klipper.on.hellbot.magna.2.300.git
cd klipper.on.hellbot.magna.2.300/klipper-hellbot
cp .config.hellbot .config
make -j4 robin
# → out/Robin_nano35.bin

# 2. Flash: copy that file to a clean FAT32 SD card, printer off,
#    insert SD, power on, wait ~1 min, power off, REMOVE the SD.

# 3. On the Pi: install Klipper with KIAUH, copy the host module,
#    add the [hbtft_buttons] section to printer.cfg.
```

Full details (connection on USART3/CH340, calibration workflow, menu
pages, troubleshooting, porting notes): **[the guide](klipper-hellbot/GUIDE-KLIPPER-TFT.md)**.

## Hardware

- Board: MKS Robin Nano V1.2 (STM32F103VET6, 8 MHz crystal, MKS 28 KiB bootloader)
- Display: MKS TFT35, ILI9488 480×320 over FSMC, resistive touch XPT2046 (SPI2)
- Host: Raspberry Pi (or any Linux SBC) over the board's USB CH340 (USART3, 250000 baud)

## License

The `klipper-hellbot/` tree keeps upstream Klipper's license (GPLv3, see
`klipper-hellbot/COPYING`).
