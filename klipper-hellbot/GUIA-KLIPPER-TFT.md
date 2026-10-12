# Guía completa: Klipper + TFT MKS35 + touch en MKS Robin Nano V1.2

Esta guía explica cómo reproducir **desde cero** el sistema completo de este
fork: Klipper controlando la impresora, con la pantalla TFT de fábrica
(MKS TFT35, ILI9488 480×320 por FSMC) mostrando 6 botones configurables y
**touch funcional** — sin perder la pantalla original ni añadir hardware.

> **Hardware de referencia**: Hellbot Magna 2 300 (o cualquier impresora con
> MKS Robin Nano V1.2, MCU STM32F103VET6, cristal 8 MHz, bootloader MKS 28 KiB).
> El TFT es el que viene soldado/en el conector FSMC de la placa.
> El touch es el XPT2046 resistivo que trae el propio panel.

---

## 1. Cómo funciona (arquitectura)

```
printer.cfg ([hbtft_buttons])                 ┌─ MCU (este fork) ──────────────┐
        │ labels, gcodes, colores,            │ src/stm32/hbtft.c:             │
        │ calibración del touch               │  - FSMC → ILI9488 (480×320)    │
        ▼                                     │  - fuente 5x7 + 6 botones      │
klippy/extras/hbtft_buttons.py (en la Pi) ──► │  - touch XPT2046 (SPI2 h/w)    │
  - envía hb_label (texto/color de botones)   │  - todo en DECL_TASK, sin      │
  - recibe hb_button (raw x,y al tocar)       │    bloquear motores/sensores   │
  - mapea raw→botón (calibración en cfg)      └────────────────────────────────┘
  - ejecuta el gcode del botón                host ←→ MCU: USART3 (CH340 USB)
```

Puntos clave del diseño:

- **El touch se calibra en `printer.cfg`, no en el firmware**: el MCU reporta
  coordenadas ADC crudas y el módulo Python hace el mapeo. Recalibrar =
  editar config + `RESTART`, sin recompilar.
- El dibujo del TFT es **progresivo por tandas** dentro de `DECL_TASK` para
  no superar el timeout del watchdog IWDG de Klipper (causa clásica de
  bootloop si se dibuja todo de golpe en `DECL_INIT`).
- Klipper **no** usa la pantalla para nada más: la Pi es la interfaz
  principal; el TFT es un panel de acceso rápido autónomo.

---

## 2. Generar el firmware (tu PC con Linux)

### 2.1 Dependencias

```bash
sudo apt install build-essential git python3 python3-pip \
     gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
```

> Si tu distro trae un toolchain muy viejo, alternativa verificada: usar el
> toolchain de PlatformIO (`~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-`).

### 2.2 Compilar

```bash
git clone https://github.com/adrianeb2015/klipper.on.hellbot.magna.2.300.git
cd klipper.on.hellbot.magna.2.300/klipper-hellbot

# Config pre-armada: STM32F103, 8 MHz, USART3, bootloader 28 KiB, HB_TFT
cp .config.hellbot .config
make -j4 robin
```

El target `robin` compila **y firma** automáticamente con
`scripts/update_mks_robin.py`. El resultado es:

```
out/Robin_nano35.bin   ← este archivo va a la SD
```

> ⚠️ **Nunca flashees `out/klipper.bin` crudo**: el bootloader MKS lo
> rechaza. Siempre `Robin_nano35.bin` (firmado).

### 2.3 Flashear por SD

1. Copia `out/Robin_nano35.bin` a la raíz de una microSD **limpia**
   (FAT32, que solo contenga ese archivo).
2. Impresora **apagada** → inserta la SD → enciende.
3. Suena un beep y la pantalla queda negra ~1 minuto mientras flashea.
4. Apaga → **saca la SD** (el bootloader renombra el bin a `.CUR`; si la
   dejas puesta re-flashea en cada arranque) → enciende.

Al arrancar debes ver la escena del TFT: **6 rectángulos de colores (3×2)**
con los labels de `printer.cfg` (o `BTN1..BTN6` por defecto).

---

## 3. Instalar el host en la Raspberry Pi

### 3.1 Klipper base

Instala con KIAUH (lo más simple) o a mano:

```bash
git clone https://github.com/th3dstudio/kiauh.git   # o dw-0/kiauh
./kiauh/kiauh.sh   # → 1) Install → 1) Klipper (+ Moonraker + Mainsail/Fluidd)
```

**Importante**: en KIAUH, cuando pregunte por el microcontrolador, elige
*opción de compilar más tarde* — ya tienes el bin de este fork.

### 3.2 Conexión

El cable USB de la placa (CH340) llega a **USART3**:

```bash
ls /dev/serial/by-id/
# algo como: usb-1a86_USB_Serial-if00-port0  (CH340)
```

En `printer.cfg`:

```ini
[mcu]
serial: /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
baud: 250000
```

### 3.3 Módulo de botones

Copia el extra al klipper instalado y hazlo visible para config:

```bash
cp klipper-hellbot/klippy-extras/hbtft_buttons.py ~/klipper/klippy/extras/
```

### 3.4 printer.cfg

Parte de la config genérica de la placa
(`klipper-hellbot/config/generic-mks-robin-nano-v1.cfg` o `-v2` según tu
revisión) y añade tus ejes/heater/extrusor. Al final añade:

```ini
[hbtft_buttons]
btn1_label: HOME
btn1_gcode: G28
btn1_color: 0xFF0000

btn2_label: UNLOAD
btn2_gcode: M702
btn2_color: 0x00AA00

btn3_label: LOAD
btn3_gcode: M701
btn3_color: 0x00AA00

btn4_label: PURGE
btn4_gcode: PURGE
btn4_color: 0x0000FF

btn5_label: MENU
btn5_gcode: {'; nada por ahora'}
btn5_color: 0xAAAA00

btn6_label: OFF
btn6_gcode: TURN_OFF_HEATERS
btn6_color: 0x666666

# ── Calibración del touch (TODO aquí, sin recompilar el FW) ──
# Valores por defecto razonables para esta placa; afina con el
# procedimiento de la sección 4.
xpt_x_min: 150
xpt_x_max: 1850
xpt_y_min: 220
xpt_y_max: 1830
flip_x: False
flip_y: True     # en este panel Y viene invertida de fábrica
```

`btnN_color` acepta **solo** `0xRRGGBB` (RGB565 directo no: es ambiguo).
Labels: máximo 16 caracteres, se muestran en mayúsculas.

```bash
sudo systemctl restart klipper
```

---

## 4. Calibrar el touch (sin recompilar)

1. Ejecuta en la consola de Mainsail/Fluidd:

   ```
   HBTFT_TOUCH_DEBUG
   ```

2. Toca dentro de cada **botón** y mira el log
   (`~/printer_data/logs/klippy.log`):

   ```
   hbtft_buttons: TOUCH raw=(823,1702) -> pixel=(185,32) boton=1
   ```

   Si `boton=N` no corresponde con el que tocaste, hay que calibrar.

3. Toca las **4 esquinas de la pantalla** y anota los `raw` mínimo/máximo
   de cada eje. Ponlos en `printer.cfg`:

   | Toque | Qué anotar |
   |---|---|
   | esquina izquierda | el raw X más chico → `xpt_x_min` |
   | esquina derecha   | el raw X más grande → `xpt_x_max` |
   | esquina superior  | raw Y extremo según `flip_y` |
   | esquina inferior  | el otro extremo de Y |

4. Si el cursor se mueve en espejo en algún eje, invierte
   `flip_x` / `flip_y`.

5. `RESTART` y repite la prueba de botones. Listo — cualquier recalibración
   futura es solo editar estos 6 valores.

**Diagnóstico**: si el MCU y la calibración del host no concuerdan, el log
dice `MCU dijo btn=N, calibración host dice btn=M` y **se usa la del host**.

---

## 5. Compilación desde cero del fork (para desarrolladores)

Si quieres partir del Klipper oficial y aplicar los cambios tú mismo, la
diferencia completa del fork respecto a `Klipper3d/klipper` se reduce a:

| Archivo | Cambio |
|---|---|
| `src/stm32/hbtft.c` | **nuevo**: driver FSMC ILI9488 + fuente + XPT2046 + protocolo `hb_label`/`hb_button` |
| `src/stm32/Makefile` | añadir `hbtft.c` al build (`src-$(CONFIG_BOARD_MKS_ROBIN_NANO...)` o genérico) |
| `Makefile` | target `robin`: `make` + `python scripts/update_mks_robin.py out/klipper.bin out/Robin_nano35.bin` |
| `.config.hellbot` | config Kconfig lista para usar |
| `klippy-extras/hbtft_buttons.py` | **nuevo**: módulo host (copiar a `klippy/extras/`) |
| `config/generic-mks-robin-nano-v1.cfg` | sección `[hbtft_buttons]` documentada |

Config Kconfig imprescindible (la típica trampa de esta placa):

```
CONFIG_MCU="stm32"
CONFIG_MCU_STM32F103=y
CONFIG_BOARD_MKS_ROBIN_NANO=y        # o generic, con estos 2 manuales:
CONFIG_MCU_STM32F103VE=y
CONFIG_CLOCK_FREQ=72000000
CONFIG_SERIAL=y
# ¡CLAVE! el bootloader MKS ocupa 28 KiB:
CONFIG_STM32_FLASH_START_7000=y
# CH340 de esta placa entra por USART3 (PB10/PB11):
CONFIG_STM32_SERIAL_USART3=y
CONFIG_HB_TFT=y                      # el driver de este fork
```

Sin `FLASH_START_7000` el bin linkea en `0x08002000` y **nada conecta**
(es el fallo #1 al portar a Robin Nano).

### Lecciones aprendidas (por si portas a otra placa/TFT)

1. **Watchdog**: init del panel por máquina de estados con deadlines
   (`timer_from_us()`), dibujo progresivo por `DECL_TASK`. Un busy-wait
   largo en init = bootloop infinito por IWDG.
2. **FSMC direcciones**: `REG=0x6001FFFE` (A16=0, comando) y
   `RAM=0x60020000` (A16=1, dato). Si el dato va a `0x60000000` cada píxel
   se interpreta como comando basura.
3. **Ventana ILI9488** (CASET/PASET): parámetros como **4 escrituras de
   1 byte**, no 2 palabras de 16 bits.
4. **Init del panel**: usar la secuencia exacta de Marlin
   (`lcd/tft_io/ili9488.h`), incluidos PWCTRL/VMCTRL/gammas e `INVOFF`.
5. **XPT2046 por SPI hw**: sin el bit `SSI` (CR1 bit 8) con NSS por GPIO el
   SPI entra en mode fault silencioso. Trama real: `[cmd][junk][D11..D4]
   [D3..D0+0000]` → valor 12-bit `(b2<<4 | b3>>4) & 0xFFF`. Descartar la
   1ª conversión de cada eje y promediar 4.

---

## 6. Diagnóstico rápido

| Síntoma | Causa probable |
|---|---|
| Pantalla blanca tras flashear | init del panel incompleto o flash corrupto → reflashea con SD limpia |
| Bootloop (beeps cada pocos seg) | busy-wait supera el IWDG, o flash start equivocado (ver §5) |
| Pantalla OK pero la Pi no ve el puerto | revisar que el FW es **USART3** y baud 250000 |
| Labels no cambian | el extra no está en `klippy/extras/`, o `klippy.log` dice que no encontró la API de registro |
| Toques no hacen nada | revisar `hbtft_buttons: mensaje recibido` en el log; si no aparece, el registro de respuestas falló |
| Toque detecta el botón equivocado | calibración (§4) — casi siempre `flip_y` o los min/max |

Marcadores por serie (`miniterm.py /dev/serial/by-id/... 250000 --raw`):
`H` = boot del driver, `G` = escena completa, `0`-`6` = botón tocado.

## 7. Estado

| Componente | Estado |
|---|---|
| FW base (steppers, serie, todo Klipper) | ✅ |
| TFT ILI9488 por FSMC (botones + labels + colores) | ✅ |
| Touch: detección de presión y raw→botón en host | ✅ |
| Calibración sin recompilar (`printer.cfg`) | ✅ |
| Macros de botones ejecutando gcode | ✅ (módulo host) |
| Widgets avanzados (temperaturas en pantalla) | 🔜 |
