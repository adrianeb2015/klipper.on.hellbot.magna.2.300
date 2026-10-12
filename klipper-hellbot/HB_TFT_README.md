# Klipper-hellbot: soporte TFT (MKS TFT35 / FSMC) en el core Klipper

Fork de Klipper con un módulo `src/stm32/hbtft.c` que agrega al firmware del MCU:

- **Driver FSMC** para la MKS TFT35 (ILI9488, 320×480) de la Robin Nano V1.2
  - Portado desde Marlin (`HAL/STM32/tft/tft_fsmc.cpp` + `lcd/tft_io/ili9488.h`)
  - CS=PD7 (NE1, banco1 @ 0x60000000), RS=PD11 (A16), bus 16-bit
  - Timings idénticos a Marlin: lectura 15-15-24, escritura 8-15-8
- **Tarea de baja prioridad**: `DECL_TASK` dibuja 8 filas por vuelta del loop
  principal — el control de motores/sensores nunca se bloquea
- **Fase 1**: dibuja franjas de colores animadas (prueba de vida del panel)
- Comunicación serie sin cambios: USART1 (CH340 / cable USB)

## Build

```bash
cd klipper-hellbot
# la config está en .config.hellbot (F103 / 8MHz / USART1 / bootloader 28KiB / HB_TFT)
cp .config.hellbot .config
make -j4 CROSS_PREFIX=/workspace/Hellbot/.pio-home/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-
# → out/klipper.bin
```

## Flashear

1. Correr `make robin` → copiar `out/Robin_nano35.bin` (ya firmado con `update_mks_robin.py`) a la SD (SD limpia, solo ese archivo)
2. Impresora apagada → insertar SD → encender → esperar ~1 min → apagar → sacar SD
3. Encender: **la pantalla debe mostrar franjas de colores animadas** (Klipper no usa
   la pantalla para nada más; negro con basura gráfica = flash falló)
4. Cable USB a la Pi, `ls /dev/serial/by-id/` → CH340, usar en printer.cfg como siempre

## Próximas fases

- **Fase 2**: texto + 6 botones estáticos + lectura XPT2046 (SPI2: PB13/14/15, CS PA7)
- **Fase 3**: extra klippy Python que empuja estado (IDLE/PAUSE/PRINT) y macros de botones
- **Fase 4**: pulido visual, temperaturas en pantalla

## Estado actual (fase TFT viva)

- `src/stm32/hbtft.c` = driver FSMC ILI9488 + escena de 6 botones estáticos.
- Linker: 0x8007000 (bootloader Robin Nano 28 KiB) — antes estaba en 0x8002000 y por eso ningún bin conectaba.
- El host se conecta por USART3 PB10/PB11 (el conector USB de la placa es USART1/CH340 y queda sin usar).
- Build único: `make robin` → `out/Robin_nano35.bin` ya firmado.
- Esperado al bootear: pantalla encendida con 6 rectángulos de colores (3x2), y Klipper operativo por serie.
