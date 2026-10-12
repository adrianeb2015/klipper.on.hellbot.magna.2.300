# Klipper-hellbot: TFT MKS TFT35 en Robin Nano V1.2 — Documento de avance

> Fecha: 2025-10-10 · Estado: **TFT funcionando, touch casi listo (falta calibrar)**
> Fork: `/workspace/Hellbot/klipper-hellbot` (basado en Klipper, MCU STM32F103VE)

## Objetivo

Usar el TFT de la placa MKS Robin Nano V1.2 (MKS TFT35, ILI9488 480×320 por
FSMC) desde Klipper, con 6 botones configurables desde `printer.cfg`, sin que
el display afecte el control de la impresora.

## Arquitectura resultante

```
printer.cfg ([hbtft_buttons])                ┌─ MCU (klipper-hellbot) ─────────┐
        │ lee labels/gcodes/colores          │ hbtft.c:                        │
        ▼                                     │  - FSMC → ILI9488 (480x320)     │
klippy-extras/hbtft_buttons.py (en la Pi) ──► │  - fuente 5x7 + botones         │
  - envía hb_label (idx,w0..w3,color)         │  - touch XPT2046 (SPI2 hw)      │
  - recibe hb_button (btn,x,y) ──► ejecuta    │  - todo por DECL_TASK sin       │
    el gcode del botón                          │    bloquear el loop principal   │
                                              └─────────────────────────────────┘
```

## Problemas resueltos (en orden histórico)

### 1. Ningún bin del fork conectaba — flash start equivocado
- **Causa**: el fork linkeaba en `0x8002000` (`FLASH_START_2000`); el bootloader
  de la Robin Nano V1.2 ocupa 28 KiB → el FW debía ir en **`0x8007000`**
  (`FLASH_START_7000`).
- **Fix**: `.config` con `CONFIG_STM32_FLASH_START_7000=y` y
  `CONFIG_FLASH_APPLICATION_ADDRESS=0x8007000`. Verificado con objdump
  (`.text` en `0x08007000`).
- Serie del host: **USART3 PB11/PB10 @250000** (el CH340 de esta placa va a
  USART3, no a USART1).

### 2. FW no flasheable — faltaba update_mks_robin.py
- **Fix**: target `make robin` en el Makefile: compila y ejecuta
  `scripts/update_mks_robin.py` automáticamente → `out/Robin_nano35.bin`
  listo para la SD. **Nunca flashear `out/klipper.bin` crudo.**

### 3. Bootloop permanente — watchdog IWDG de Klipper
- **Causa**: Klipper habilita el IWDG con timeout ~500 ms y lo alimenta desde
  una DECL_TASK. Cualquier busy-wait largo (init TFT con delays, dibujo
  completo en DECL_INIT) supera el timeout → reset en bucle.
- **Fix (diseño definitivo)**:
  - Init del panel por **máquina de estados con deadlines** (SWRESET → espera
    150 ms → SLPOUT → espera → config → dibujo), usando
    `timer_read_time()/timer_from_us()/timer_is_before()`.
  - El dibujo es **progresivo por tandas**: una banda de 32 filas o un botón
    por vuelta de DECL_TASK (~1 ms cada una). El watchdog se alimenta solo.

### 4. Pantalla blanca — secuencia de init incompleta
- **Causa**: mi secuencia ILI9488 simplificada no llevaba la alimentación del
  panel a un estado correcto.
- **Fix**: secuencia **exacta de Marlin** (`lcd/tft_io/ili9488.h` +
  `tft_io.cpp`): FRMCTR1 0xB1=0xA0, INVTR 0xB4=0x02, DISCTRL 0xB6=0x02,0x02,
  PWCTRL1 0xC0=0x15,0x17, PWCTRL2 0xC1=0x41, VMCTRL 0xC5=0x00,0x12,0x80,
  SETIMAGE 0xE9=0x00, ADJCTL3 0xF7=0xA9,0x51,0x2C,0x82, gammas E0/E1,
  INVOFF, NORON, DISON. Parámetros como escrituras individuales de 1 byte.
- Config de color correcta: **INVOFF** (la de Marlin), COLMOD 0x55 (RGB565),
  MADCTL 0xE8 (480×320 landscape, BGR).

### 5. Los píxeles no se dibujaban — DOS bugs de bus
- **Bug A — dirección de datos FSMC**: Marlin usa un struct con base en
  `0x6001FFFE`: `REG` en +0 (A16=0 → RS=0 → comando) y `RAM` en +2 =
  **`0x60020000`** (A16=1 → RS=1 → dato). Yo tenía `LCD_RAM=0x60000000`
  (también A16=0): cada píxel se interpretaba como comando basura.
- **Bug B — ventana (CASET/PASET)**: los parámetros deben ir como **4
  escrituras de 1 byte** (hi x0, lo x0, hi x1, lo x1); yo mandaba 2 palabras
  de 16 bits y el panel (que latchea el byte bajo) armaba una ventana
  inválida.
- Con ambos corregidos, la pantalla dibuja perfecta.

### 6. Touch XPT2046
- Pines (como Marlin): SCK=PB13, MISO=PB14, MOSI=PB15, **CS=PA7**, sin IRQ
  (polling).
- SPI2 por hardware (APB1/256 = 140 kHz, modo 0, MSB first).
- **Trampa 1**: sin el bit **SSI** (CR1 bit 8) el SPI en master con NSS por
  GPIO entra en mode fault → muerto silencioso.
- **Trampa 2**: el umbral invertido en el deadline del polling hacía que nunca
  se sondeara.
- **Trampa 3 — alineación de la conversión**: la trama real del XPT de esta
  placa es `[cmd][b1: junk][b2: D11..D4][b3: D3..D0+0000]`; el valor 12-bit es
  `(b2<<4 | b3>>4) & 0xFFF`. Se detectó mostrando los bytes crudos en pantalla.
- Anti-ruido: descartar la 1ra conversión de cada eje tras el toque, settle
  20 ms, promedio de 4 muestras, press cuando Z1 (cmd 0xB0) > 100.

### 7. Errores lado host (klippy)
- `%s` en comandos de protocolo → "Command parser error" en el MCU: los labels
  viajan empaquetados en **4×uint32** (`hb_label idx=%u w0=%u w1=%u w2=%u
  w3=%u color=%u`), 4 caracteres ASCII por palabra, byte bajo primero.
- `str` en vez de `bytes` al enviar → codificar con `.encode('ascii')`.
- `register_response` cambió de lugar entre versiones de klippy → el módulo
  prueba `mcu.register_response`, `mcu.serial.register_response` y
  `mcu._serial.register_response`; si no encuentra ninguna, loggea warning y
  sigue (labels funcionan, toques no ejecutan gcode). **PENDIENTE** resolver.

## Estado actual

| Componente | Estado |
|---|---|
| FW base (USART3, stepper, todo Klipper) | ✅ funcionando |
| TFT ILI9488 por FSMC (botones + texto) | ✅ funcionando |
| Labels desde printer.cfg | ✅ funcionando |
| Colores por botón (`btnN_color`, formato `0xRRGGBB`) | ✅ funcionando |
| Touch: detección de presión | ⚠️ la última trama (3er byte) aún sin validar en placa |
| Touch: calibración (rango/inversión de ejes) | ❌ pendiente |
| Ejecución de gcode al tocar (register_response) | ❌ pendiente |

## Pendientes inmediatos

1. **Validar trama de 3 bytes del XPT** (último build): presionar y ver si
   `Z` sube a cientos/miles en la franja de diag. Si no, mostrar/analizar
   `B` (b2,b3) presionando.
2. **Calibración**: tocar 4 esquinas, anotar XR/YR, ajustar `XPT_MIN_RAW`
   (300), `XPT_MAX_RAW` (3800) y los flips en `xpt_map_x/xpt_map_y`
   (hoy: X invertido, Y directo).
3. **register_response**: pedir `git -C ~/klipper describe --tags` y
   `grep -i hbtft printer_data/logs/klippy.log`; ubicar la API exacta de esa
   versión (serialhdl tiene `register_response(callback, name, oid=None)` en
   el fork). Alternativa: subscribir vía `mcu.serial.register_response`.
4. **Opcional**: quitar la franja de diagnóstico del touch (queda pegada al
   dibujar labels) cuando todo funcione.

## Archivos clave

| Archivo | Qué es |
|---|---|
| `klipper-hellbot/src/stm32/hbtft.c` | Todo el driver TFT/touch/botones (únicos cambios al FW) |
| `klipper-hellbot/klippy-extras/hbtft_buttons.py` | Módulo klippy (copiar a `~/klipper/klippy/extras/` en la Pi) |
| `klipper-hellbot/Makefile` | Target `make robin` (build + firmado MKS) |
| `klipper-hellbot/.config` / `.config.pure` | FW con TFT / FW puro sin TFT (referencia) |
| `klipper-hellbot/out/Robin_nano35.bin` | Último FW firmado listo para SD |
| `klipper-hellbot/HB_TFT_README.md` | README corto (algo desactualizado) |

## Flujo de trabajo

```bash
# Compilar y firmar (siempre genera out/Robin_nano35.bin listo para SD)
cd klipper-hellbot
make -j4 robin CROSS_PREFIX=/workspace/Hellbot/.pio-home/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-

# Flashear
# 1. copiar out/Robin_nano35.bin a la SD (renombrado como Robin_nano35.bin)
# 2. placa apagada → SD puesta → encender → beep → esperar ~1 min
# 3. apagar → SACAR SD (el bootloader la renombra .CUR) → encender

# Host (Pi "blue")
sudo systemctl stop klipper          # para diagnosticar por serie
miniterm.py /dev/serial/by-id/XXX 250000 --raw
# marcadores en serie: 'H' boot, 'G' escena completa, '0'-'6' botones tocados
```

## Config printer.cfg (actual)

```ini
[hbtft_buttons]
btn1_label: HOME
btn1_gcode: G28
btn1_color: 0xFF0000
btn2_label: UNLOAD
btn2_gcode: M702
btn2_color: 0x00AA00
# ... btn3..btn6 iguales
```

`btnN_color` acepta SOLO `0xRRGGBB` (la detección automática de RGB565 se
quitó por ambigua: `0x00AA00` < 0xFFFF se tomaba como 565 y daba marrón).

## Notas de hardware

- Robin Nano V1.2 = STM32F103VET6, cristal 8 MHz, bootloader MKS 28 KiB.
- USB de la placa → CH340 → **USART3** (PB10/PB11). USART1 queda libre.
- FSMC: banco 1 (NE1=PD7 @ 0x60000000), RS=A16 (PD11), REG=0x6001FFFE,
  RAM=0x60020000, timings ADDSET=15/DATAST=15, remap NADV (MAPR bit 10).
- Backlight PD13. Pines FSMC: PD0/1/4/5/7-11/14/15 + PE7-15 (sin conflicto con
  steppers PE0-PE6, PD3/PD6, ni SD_DETECT PD12).
- Touch: SPI2 (PB13/14/15) + CS PA7.
