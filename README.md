# Hellbot Magna 2 300 — entorno Marlin (Dragonburner + Sherpa Mini)

> 🔴 **Klipper with the stock TFT touchscreen**: see the
> **[Complete guide: Klipper + TFT + touch](klipper-hellbot/GUIDE-KLIPPER-TFT.md)**
> — firmware build, Pi setup, and touch calibration without recompiling.

Entorno completo para editar y compilar Marlin para la **Hellbot Magna 2 300**
con placa **MKS Robin Nano V1.2**, convertido a hotend **Dragonburner** con
extrusor **Sherpa Mini**.

## Uso diario

```bash
source env.sh          # activa PlatformIO local (no instala nada en el sistema)

pio-build              # variante TFT_COLOR_UI (por defecto)
pio run -e mks_robin_nano_v1v2_lvgl    # variante TFT_LVGL_UI (UI MKS, para módulo WiFi MKS)
```

**Dos variantes disponibles:**

| Env | UI | Salida |
|---|---|---|
| `mks_robin_nano_v1v2` (default) | TFT_COLOR_UI (Marlin) | `.pio/build/mks_robin_nano_v1v2/Robin_nano35.bin` |
| `mks_robin_nano_v1v2_lvgl` | TFT_LVGL_UI (estilo MKS/factory) | `.pio/build/mks_robin_nano_v1v2_lvgl/Robin_nano35.bin` **+ carpeta `assets/`** |

La variante LVGL necesita copiar a la raíz de la SD, junto al `Robin_nano35.bin`,
también la carpeta `assets/` que queda dentro de `.pio/build/mks_robin_nano_v1v2_lvgl/`.

El flag que elige la UI está en `Marlin/Configuration.h` (bloque `HELLBOT_LVGL_UI`).

Para flashear: copiá `Robin_nano35.bin` a la raíz de la microSD, apagá la
impresora, insertá la SD y encendé. La Robin Nano la cifra y flashea sola
(la pantalla queda en negro ~1 min). Verificá con `M115` (debe decir
"Hellbot Magna 2 300").

## Estructura

| Ruta | Qué es |
|---|---|
| `env.sh` | variables de entorno de PlatformIO (source antes de compilar) |
| `marlin/` | clone de MarlinFirmware/Marlin (bugfix-2.1.x) |
| `marlin/Marlin/Configuration.h` | configuración principal (editada) |
| `marlin/Marlin/Configuration_adv.h` | configuración avanzada (editada) |
| `marlin/hellbot.ini` | notas del env de compilación |
| `config/pristine/` | copia de los Configuration originales sin editar |
| `configurations/` | repo oficial de configuraciones de ejemplo (referencia) |
| `.tools/` | PlatformIO instalado localmente |
| `.pio-home/` | datos de PlatformIO (toolchains, paquetes) |

## Cambios aplicados a la configuración

### Placa / máquina
- `MOTHERBOARD BOARD_MKS_ROBIN_NANO` (Robin Nano V1.2, STM32F103VE)
- Nombre: "Hellbot Magna 2 300"
- Volumen: 300 × 300 × 400 mm
- Pantalla MKS TFT 3.5" táctil: `MKS_ROBIN_TFT35` + UI seleccionable (`TFT_COLOR_UI` o `TFT_LVGL_UI`)
- Serie: `SERIAL_PORT 1`, `SERIAL_PORT_2 3` (250000 baud)
- `SDSUPPORT` activado (impresión desde microSD)
- `EEPROM_SETTINGS` + `EEPROM_INIT_NOW` (persistencia en flash: touch, pasos, PID)
- Endstops NO: `*_MIN_ENDSTOP_HIT_STATE LOW`

### Hotend Dragonburner
- `TEMP_SENSOR_0 5` (Semitec 104GT-2, típico de E3D Revo/V6). Si mantenés la
  NTC genérica del hotend original, usar `1`.
- `HEATER_0_MAXTEMP 300`
- Fan del hotend en **FAN1 (PB0)** con `E0_AUTO_FAN_PIN FAN1_PIN`: gira solo
  por encima de 50 °C (`EXTRUDER_AUTO_FAN_TEMPERATURE`).
- Fan de parte fría en **FAN0 (PB1)** (el de siempre, controlado con M106).

### Extrusor Sherpa Mini
- Pasos E por mm: **282** (Sherpa Mini con motor pancake 0.9°, 16 µsteps).
  ⚠️ Calibrar igualmente: `M92 E…` + medir 100 mm extruidos + `M500`.
- Feedrate/accel moderados para toolhead directo (retracción accel 3000).

### Pendientes al instalar
1. Calibrar `M92 E` (esteps del Sherpa).
2. PID del hotend nuevo: `M303 E0 S220 C8` luego `M500`.
3. PID de cama si cambia: `M303 E-1 S60 C8` + `M500`.
4. Si el fan del hotend quedó en otro conector del PCB del toolhead,
   ajustar `E0_AUTO_FAN_PIN`.
5. Migrar settings viejos: tras flashear, correr `M502` + `M500` para
   arrancar limpio desde los defaults del firmware.

## Para actualizar Marlin más adelante

```bash
cd marlin
git stash            # guarda tu configuración
git pull
git stash pop        # reaplica (resolvé conflictos si los hay)
pio-build
```
