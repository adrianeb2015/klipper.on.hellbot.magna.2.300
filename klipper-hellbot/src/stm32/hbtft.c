// HB-TFT: MKS TFT35 (ILI9488, 480x320) por FSMC en Robin Nano V1.2 (STM32F103VE)
//
// Regla de oro: NUNCA bloquear el loop principal mas de unos pocos ms.
// Klipper habilita el IWDG (~500ms timeout) y lo alimenta desde una tarea;
// cualquier busy-wait largo reinicia la placa. Por eso:
//  - El init del panel es una maquina de estados con deadlines (sin delays).
//  - El dibujo se hace en tandas chicas por vuelta de DECL_TASK:
//    el display queda en minima prioridad, igual que cualquier otra tarea.
#include "autoconf.h"
#include "sched.h"
#include "command.h"
#include "generic/armcm_timer.h" // udelay
#include "generic/misc.h"        // timer_read_time, timer_from_us, timer_is_before

#include <stdint.h>

/****************************************************************
 * Registros F1 minimos
 ****************************************************************/
#define RCC_BASE    0x40021000U
#define RCC_APB1ENR (*(volatile uint32_t *)(RCC_BASE + 0x1C))
#define RCC_APB2ENR (*(volatile uint32_t *)(RCC_BASE + 0x18))
#define RCC_AHBENR  (*(volatile uint32_t *)(RCC_BASE + 0x14))

typedef struct { volatile uint32_t CRL, CRH, IDR, ODR, BSRR, BRR, LCKR; } GPIO_t;
#define GPIOA ((GPIO_t *)0x40010800U)
#define GPIOB ((GPIO_t *)0x40010C00U)
#define GPIOD ((GPIO_t *)0x40011400U)
#define GPIOE ((GPIO_t *)0x40011800U)
#define AFIO_MAPR (*(volatile uint32_t *)(0x40010000U + 0x04))

static inline void
f1_pin_afpp(GPIO_t *g, uint8_t pin)
{   // AF push-pull 50MHz (FSMC)
    volatile uint32_t *cr = (pin < 8) ? &g->CRL : &g->CRH;
    uint8_t pos = (pin & 7) * 4;
    uint32_t v = *cr;
    v &= ~(0xFU << pos);
    v |= (0xBU << pos);
    *cr = v;
}

static inline void
f1_pin_out(GPIO_t *g, uint8_t pin)
{   // GPIO push-pull 50MHz (backlight)
    volatile uint32_t *cr = (pin < 8) ? &g->CRL : &g->CRH;
    uint8_t pos = (pin & 7) * 4;
    uint32_t v = *cr;
    v &= ~(0xFU << pos);
    v |= (0x3U << pos);
    *cr = v;
}

/****************************************************************
 * FSMC -> ILI9488
 * Banco 1 (NE1=PD7): base 0x60000000. RS=A16 (PD11) =>
 *   REG (cmd): 0x6001FFFE ; RAM (data): 0x60000000
 ****************************************************************/
// REG en 0x6001FFFE (A16=0 -> RS=0, comando) y RAM en 0x60020000
// (A16=1 -> RS=1, dato). Es el truco del struct de Marlin: base+0 = REG,
// base+2 cae en el byte alto del banco con A16=1.
#define LCD_REG (*(volatile uint16_t *)0x6001FFFEU)
#define LCD_RAM (*(volatile uint16_t *)0x60020000U)

static inline void tft_cmd(uint8_t c)  { LCD_REG = c; }
static inline void tft_dat(uint16_t d) { LCD_RAM = d; }

#define TFT_W 480
#define TFT_H 320

static void
tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    // Como Marlin: 4 escrituras de un byte por comando (el panel latchea
    // el byte bajo de cada escritura del bus de 16).
    tft_cmd(0x2A);              // CASET
    tft_dat((x0 >> 8) & 0xFF);
    tft_dat(x0 & 0xFF);
    tft_dat((x1 >> 8) & 0xFF);
    tft_dat(x1 & 0xFF);
    tft_cmd(0x2B);              // PASET
    tft_dat((y0 >> 8) & 0xFF);
    tft_dat(y0 & 0xFF);
    tft_dat((y1 >> 8) & 0xFF);
    tft_dat(y1 & 0xFF);
    tft_cmd(0x2C);              // RAMWR
}

static void
tft_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    tft_set_window(x, y, x + w - 1, y + h - 1);
    uint32_t n = (uint32_t)w * h;
    while (n--)
        LCD_RAM = color;
}

static void
tft_frame_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h
               , uint16_t fill, uint16_t border, uint8_t th)
{
    tft_fill_rect(x, y, w, th, border);
    tft_fill_rect(x, y + h - th, w, th, border);
    tft_fill_rect(x, y, th, h, border);
    tft_fill_rect(x + w - th, y, th, h, border);
    tft_fill_rect(x + th, y + th, w - 2 * th, h - 2 * th, fill);
}

/****************************************************************
 * Comandos ILI9488
 ****************************************************************/
// Config "rapida" del panel (sin delays asociados)
static const uint8_t hb_variant = 0;   // INVOFF: colores correctos

// Secuencia de init ILI9488 identica a la de Marlin (tft_io.cpp/ili9488.h),
// parametros como escrituras individuales de un byte.
static void
ili9488_config(void)
{
    tft_cmd(0xB1); tft_dat(0xA0);           // FRMCTR1
    tft_cmd(0xB4); tft_dat(0x02);           // INVTR
    tft_cmd(0xB6); tft_dat(0x02); tft_dat(0x02);  // DISCTRL
    tft_cmd(0xC0); tft_dat(0x15); tft_dat(0x17);  // PWCTRL1
    tft_cmd(0xC1); tft_dat(0x41);           // PWCTRL2
    tft_cmd(0xC5); tft_dat(0x00); tft_dat(0x12); tft_dat(0x80);  // VMCTRL
    tft_cmd(0xE9); tft_dat(0x00);           // SETIMAGE
    tft_cmd(0xF7); tft_dat(0xA9); tft_dat(0x51); tft_dat(0x2C); tft_dat(0x82);  // ADJCTL3

    tft_cmd(0x36); tft_dat(0xE8);           // MADCTL: 480x320 landscape, BGR
    tft_cmd(0x3A); tft_dat(0x55);           // COLMOD: RGB565

    tft_cmd(0xE0);                          // PGAMCTRL
    { static const uint8_t g[15] = { 0x00,0x03,0x09,0x08,0x16,0x0A,0x3F
                                    ,0x78,0x4C,0x09,0x0A,0x08,0x16,0x1A,0x0F };
      for (int i = 0; i < 15; i++) tft_dat(g[i]); }
    tft_cmd(0xE1);                          // NGAMCTRL
    { static const uint8_t g[15] = { 0x00,0x16,0x19,0x03,0x0F,0x05,0x32
                                    ,0x45,0x46,0x04,0x0E,0x0D,0x35,0x37,0x0F };
      for (int i = 0; i < 15; i++) tft_dat(g[i]); }

    tft_cmd(hb_variant ? 0x21 : 0x20);      // INVON / INVOFF
    tft_cmd(0x13);                          // NORON
    tft_cmd(0x29);                          // DISON
}

/****************************************************************
 * Escena: 6 botones (grilla 3x2)
 ****************************************************************/
#define BTN_MARGIN 12
#define BTN_GAP    12
#define BTN_W      ((TFT_W - 2 * BTN_MARGIN - 2 * BTN_GAP) / 3)
#define BTN_H      ((TFT_H - 2 * BTN_MARGIN - BTN_GAP) / 2)

/****************************************************************
 * Fuente 5x7 (ASCII 0x20-0x5A, minusculas -> mayusculas)
 ****************************************************************/
static const uint8_t hb_font5x7[59][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x14,0x08,0x3E,0x08,0x14}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}  // Z
};

// Caracter 5x7 escalable (sc = tamano de pixel: 2 = 10x14, 3 = 15x21)
static void
hb_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint8_t sc)
{
    if (c >= 'a' && c <= 'z')
        c -= 32;
    int gi = (c >= 0x20 && c <= 0x5A) ? (c - 0x20) : 3;
    for (int col = 0; col < 5; col++) {
        uint8_t bits = hb_font5x7[gi][col];
        for (int row = 0; row < 7; row++)
            if (bits & (1 << row))
                tft_fill_rect(x + col * sc, y + row * sc, sc, sc, color);
    }
}

static void
hb_draw_text(uint16_t x, uint16_t y, const char *s, uint16_t color, uint8_t sc)
{
    while (*s) {
        hb_draw_char(x, y, *s++, color, sc);
        x += 6 * sc;
    }
}

static const uint16_t btn_fill[6] = {
    0x2104, 0x9E86, 0xD220, 0x42B6, 0xB8E6, 0xFFE0
};

// Labels configurables desde printer.cfg (vian klippy: comando hb_label)
static char hb_labels[6][17] = {
    "BTN1", "BTN2", "BTN3", "BTN4", "BTN5", "BTN6"
};
static char hb_labels2[6][17];         // 2da linea (opcional, vacia = 1 linea)
static uint8_t hb_big[6];              // 1 = linea 1 en fuente grande (escala 3)
static uint8_t hb_icon[6];             // indice de icono (0 = sin icono)

// Iconos 16x16 monocromos (dibujados a escala 2 = 32x32 px)
static const uint8_t hb_icons[8][16][2] = {
    // home
    {
        {0x00, 0x00},
        {0x01, 0x80},
        {0x03, 0xC0},
        {0x07, 0xE0},
        {0x0F, 0xF0},
        {0x1F, 0xF8},
        {0x3F, 0xFC},
        {0x7F, 0xFE},
        {0xFF, 0xFF},
        {0x07, 0xE0},
        {0x06, 0x60},
        {0x06, 0x60},
        {0x06, 0x60},
        {0x07, 0xE0},
        {0x00, 0x00},
        {0x00, 0x00},
    },
    // flame
    {
        {0x01, 0x00},
        {0x03, 0x00},
        {0x03, 0x80},
        {0x07, 0xC0},
        {0x0F, 0xE0},
        {0x0B, 0xD0},
        {0x1B, 0xD8},
        {0x1B, 0xD8},
        {0x3B, 0xDC},
        {0x33, 0xCC},
        {0x73, 0xCE},
        {0x63, 0xC6},
        {0x63, 0xC6},
        {0x03, 0xC0},
        {0x03, 0xC0},
        {0x00, 0x00},
    },
    // bed
    {
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x7F, 0xFE},
        {0x7F, 0xFE},
        {0x00, 0x00},
        {0x24, 0x90},
        {0x24, 0x90},
        {0xFF, 0xFF},
        {0xFF, 0xFF},
        {0x00, 0x00},
    },
    // filament
    {
        {0x00, 0x00},
        {0x07, 0xE0},
        {0x1F, 0xF8},
        {0x3C, 0x3C},
        {0x38, 0x1C},
        {0x78, 0x1E},
        {0x79, 0xEE},
        {0x7B, 0xF6},
        {0x7B, 0xF6},
        {0x79, 0xEE},
        {0x38, 0x1C},
        {0x3C, 0x3C},
        {0x1F, 0xF8},
        {0x07, 0xE0},
        {0x00, 0x00},
        {0x00, 0x00},
    },
    // pause
    {
        {0x00, 0x00},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x3C, 0x3C},
        {0x00, 0x00},
    },
    // play
    {
        {0x00, 0x00},
        {0x0C, 0x00},
        {0x0F, 0x00},
        {0x0F, 0xC0},
        {0x0F, 0xF0},
        {0x0F, 0xFC},
        {0x0F, 0xFF},
        {0x0F, 0xFF},
        {0x0F, 0xFF},
        {0x0F, 0xFF},
        {0x0F, 0xFC},
        {0x0F, 0xF0},
        {0x0F, 0xC0},
        {0x0F, 0x00},
        {0x0C, 0x00},
        {0x00, 0x00},
    },
    // menu
    {
        {0x00, 0x00},
        {0x00, 0x00},
        {0xFF, 0xFF},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0xFF, 0xFF},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0xFF, 0xFF},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
        {0x00, 0x00},
    },
    // power
    {
        {0x01, 0x80},
        {0x01, 0x80},
        {0x01, 0x80},
        {0x01, 0x80},
        {0x0B, 0xD0},
        {0x19, 0x98},
        {0x11, 0x88},
        {0x31, 0x8C},
        {0x21, 0x84},
        {0x21, 0x84},
        {0x20, 0x04},
        {0x10, 0x08},
        {0x18, 0x18},
        {0x0C, 0x30},
        {0x03, 0xC0},
        {0x00, 0x00},
    },
};
#define HB_ICON_COUNT 8
static const char *const hb_icon_names[] = {"home", "flame", "bed", "filament", "pause", "play", "menu", "power"};
static uint16_t hb_colors[6] = {
    0x2104, 0x9E86, 0xD220, 0x42B6, 0xB8E6, 0xFFE0
};
static int8_t hb_flash_btn = -1;       // boton "presionado" visualmente
static uint32_t hb_flash_deadline;

static uint16_t
btn_x(int i) { return BTN_MARGIN + (i % 3) * (BTN_W + BTN_GAP); }

static uint16_t
btn_y(int i) { return BTN_MARGIN + (i / 3) * (BTN_H + BTN_GAP); }

static void
hb_draw_icon(int i, uint16_t bx, uint16_t by, uint16_t color)
{
    const uint8_t *bm = hb_icons[hb_icon[i] - 1];
    // 16x16 a escala 2 => 32x32 px, centrado horizontal, a by+12
    uint16_t x0 = bx + (BTN_W - 32) / 2, y0 = by + 12;
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 2; c++) {
            uint8_t bits = bm[r * 2 + c];
            for (int b = 0; b < 8; b++)
                if (bits & (0x80 >> b))
                    tft_fill_rect(x0 + (c * 8 + b) * 2, y0 + r * 2, 2, 2,
                                  color);
        }
}

// Dibuja el/los labels de un boton, centrados, en el color dado
static void
hb_draw_label(int i, uint16_t color)
{
    const char *s1 = hb_labels[i];
    const char *s2 = hb_labels2[i];
    int n1 = 0, n2 = 0;
    while (s1[n1] && n1 < 16) n1++;
    while (s2[n2] && n2 < 16) n2++;
    uint16_t bx = btn_x(i), by = btn_y(i);
    if (hb_icon[i]) {
        hb_draw_icon(i, bx, by, color);
        // label(s) en la mitad inferior
        if (n2) {
            hb_draw_text(bx + (BTN_W - n1 * 12) / 2, by + BTN_H - 60,
                         s1, color, 2);
            hb_draw_text(bx + (BTN_W - n2 * 12) / 2, by + BTN_H - 32,
                         s2, color, 2);
        } else {
            hb_draw_text(bx + (BTN_W - n1 * 12) / 2, by + BTN_H - 34,
                         s1, color, 2);
        }
        return;
    }
    if (hb_big[i]) {
        // ambas lineas grandes (escala 3), centradas en su mitad
        uint16_t y1 = n2 ? (uint16_t)(by + (BTN_H / 2 - 21) / 2 + 4)
                         : (uint16_t)(by + (BTN_H - 21) / 2);
        hb_draw_text(bx + (BTN_W - n1 * 18) / 2, y1, s1, color, 3);
        if (n2) {
            uint16_t y2 = (uint16_t)(by + BTN_H / 2
                                     + (BTN_H / 2 - 21) / 2);
            hb_draw_text(bx + (BTN_W - n2 * 18) / 2, y2, s2, color, 3);
        }
    } else if (n2) {
        // dos lineas normales
        hb_draw_text(bx + (BTN_W - n1 * 12) / 2, by + BTN_H / 2 - 20,
                     s1, color, 2);
        hb_draw_text(bx + (BTN_W - n2 * 12) / 2, by + BTN_H / 2 + 8,
                     s2, color, 2);
    } else {
        hb_draw_text(bx + (BTN_W - n1 * 12) / 2, by + (BTN_H - 14) / 2,
                     s1, color, 2);
    }
}

static void
draw_button_full(int i)
{
    tft_frame_rect(btn_x(i), btn_y(i), BTN_W, BTN_H
                   , hb_colors[i], 0xFFFF, 3);
    hb_draw_label(i, 0x0000);
}

// Dibujo progresivo: fondo por bandas + un boton por llamada
#define BG_BAND_H 32
#define BG_BANDS  (TFT_H / BG_BAND_H)
#define DRAW_STEPS (BG_BANDS + 6)

/****************************************************************
 * Maquina de estados (sin bloqueos)
 ****************************************************************/
enum { HB_WAIT_RESET, HB_WAIT_SLEEP, HB_WAIT_PWR, HB_DRAW, HB_DONE };

static uint8_t hb_state = HB_WAIT_RESET;
static uint32_t hb_deadline;
static uint16_t hb_draw_step;
static uint32_t hb_test_deadline;
// Franja superior: 6 celdas; las primeras (variant+1) blancas indican
// que config de init esta mostrando la pantalla en este ciclo.
static void
hb_draw_variant_strip(void)
{
    for (int i = 0; i < 6; i++)
        tft_fill_rect(i * 40, 0, 38, 10
                      , ((hb_variant == 0 && i == 0)
                         || (hb_variant == 1 && i <= 1)) ? 0xFFFF : 0x0000);
}


/****************************************************************
 * Touch XPT2046: SPI2 en bit-bang (SCK PB13, MISO PB14, MOSI PB15,
 * CS PA7). Polling en la tarea, ~10 veces/seg. Calibracion por
 * constantes (ajustar con HB_TOUCH_INFO si hace falta).
 ****************************************************************/
#define XPT_CS_LOW()   (GPIOA->BSRR = (1 << (7 + 16)))
#define XPT_CS_HIGH()  (GPIOA->BSRR = (1 << 7))

// Rango raw del XPT2046 (calibrar con los XR/YR que muestra la pantalla)
#define XPT_MIN_RAW 300
#define XPT_MAX_RAW 3800

// SPI2 por hardware (APB1 36MHz / 256 = 140kHz, modo 0) como Marlin
#define SPI2_CR1 (*(volatile uint32_t *)0x40003800U)
#define SPI2_SR  (*(volatile uint32_t *)0x40003808U)
#define SPI2_DR  (*(volatile uint32_t *)0x4000380CU)

static uint32_t hb_touch_deadline;
static uint8_t hb_touch_state;   // 0 = libre, 1 = presionado

static void
xpt_init(void)
{
    f1_pin_out(GPIOA, 7);        // CS
    XPT_CS_HIGH();
    // SCK PB13 / MOSI PB15: AF push-pull; MISO PB14: entrada flotante
    f1_pin_afpp(GPIOB, 13);
    f1_pin_afpp(GPIOB, 15);
    {
        uint32_t v = GPIOB->CRH;
        v &= ~(0xFU << (4 * 6));
        v |= (0x4U << (4 * 6));
        GPIOB->CRH = v;
    }
    RCC_APB1ENR |= (1 << 14);    // SPI2 clock
    // MSTR | BR=f/256 (110<<3) | SPE, SSM=0 (NSS por GPIO), MSB first
    SPI2_CR1 = (1 << 8) | (1 << 2) | (6 << 3) | (1 << 6);  // SSI|MSTR|BR|SPE
}

static uint8_t
spi_xfer(uint8_t v)
{
    while (!(SPI2_SR & (1 << 1)))  ; // TXE
    SPI2_DR = v;
    while (!(SPI2_SR & 1))         ; // RXNE
    return SPI2_DR;
}

// 12 bits MSB-first: cmd, luego 2 bytes (bit7 del 1ro = busy, se descarta)

// Trama real observada: [cmd][b1: junk][b2: D11..D4][b3: D3..D0 + 0000]
static uint16_t
xpt_read(uint8_t cmd)
{
    uint8_t b2, b3;
    XPT_CS_LOW();
    spi_xfer(cmd);
    spi_xfer(0);                 // b1: junk/estado
    b2 = spi_xfer(0);
    b3 = spi_xfer(0);
    XPT_CS_HIGH();
    return (uint16_t)((((uint16_t)b2 << 4) | (b3 >> 4)) & 0xFFF);
}


// Calibracion medida con las 4 esquinas (2026-10-11):
//   sup-izq (176,1767) sup-der (1824,1778) inf-izq (211,256) inf-der (1822,286)
// => X: ~[150,1850] sin invertir ; Y: ~[220,1830] INVERTIDA (arriba=raw alto)
#define XPT_X_MIN 150
#define XPT_X_MAX 1850
#define XPT_Y_MIN 220
#define XPT_Y_MAX 1830

static uint16_t
xpt_map_x(uint16_t raw)
{
    uint32_t v = raw;
    if (v < XPT_X_MIN) v = XPT_X_MIN;
    if (v > XPT_X_MAX) v = XPT_X_MAX;
    return (v - XPT_X_MIN) * TFT_W / (XPT_X_MAX - XPT_X_MIN);
}

static uint16_t
xpt_map_y(uint16_t raw)
{
    uint32_t v = raw;
    if (v < XPT_Y_MIN) v = XPT_Y_MIN;
    if (v > XPT_Y_MAX) v = XPT_Y_MAX;
    return TFT_H - (v - XPT_Y_MIN) * TFT_H / (XPT_Y_MAX - XPT_Y_MIN);
}

static void
xpt_poll(uint32_t now)
{
    if (timer_is_before(now, hb_touch_deadline))
        return;                      // aun no toca sondear
    hb_touch_deadline = now + timer_from_us(100000);
    // presion: Z1 (cmd 0xB0); <100 = sin toque
    uint16_t z = xpt_read(0xB0);
    if (z > 40) {
        if (hb_touch_state)
            return;
        hb_touch_state = 1;
        udelay(20000);           // settle del panel tras el toque
        // descartar primera conversion de cada eje (mux recien cambiado)
        xpt_read(0x90); xpt_read(0xD0);
        uint32_t xs = 0, ys = 0;
        for (int i = 0; i < 4; i++) {
            xs += xpt_read(0x90);
            ys += xpt_read(0xD0);
        }
        uint16_t rx = xs / 4, ry = ys / 4;
        uint16_t px = xpt_map_x(rx);
        uint16_t py = xpt_map_y(ry);

        // hit-test con el mapeo actual (calibrar despues)
        int btn = 255;
        if (px < TFT_W && py < TFT_H) {
            int col = px / (BTN_W + BTN_GAP + BTN_MARGIN * 2 / 3);
            int row = py / (BTN_H + BTN_GAP + BTN_MARGIN * 2 / 2);
            if (col > 2) col = 2;
            if (row > 1) row = 1;
            // btn_x/btn_y esperan el indice de boton (0-5), no col/row
            int idx = row * 3 + col;
            uint16_t bx = btn_x(idx), by = btn_y(idx);
            if (px >= bx && px < bx + BTN_W && py >= by && py < by + BTN_H) {
                btn = idx;
                // feedback visual: boton invertido 300ms
                tft_frame_rect(bx, by, BTN_W, BTN_H, 0x0000, 0xFFFF, 3);
                hb_draw_label(btn, 0xFFFF);
                hb_flash_btn = btn;
                hb_flash_deadline = now + timer_from_us(300000);
            }
        }
        // evento SIEMPRE: x/y crudos = data de calibracion en el log
        sendf("hb_button btn=%u x=%u y=%u", btn, rx, ry);
    } else {
        hb_touch_state = 0;
    }
}

void
hb_tft_init(void)
{

    // Clocks + pines + FSMC (todo rapido, sin esperas)
    RCC_APB2ENR |= (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6);
    RCC_AHBENR  |= (1 << 8);

    f1_pin_out(GPIOD, 13);       // backlight
    GPIOD->BSRR = (1 << 13);

    f1_pin_afpp(GPIOD, 0);       // D2
    f1_pin_afpp(GPIOD, 1);       // D3
    f1_pin_afpp(GPIOD, 4);       // NOE
    f1_pin_afpp(GPIOD, 5);       // NWE
    f1_pin_afpp(GPIOD, 7);       // NE1 (CS)
    f1_pin_afpp(GPIOD, 8);       // D13
    f1_pin_afpp(GPIOD, 9);       // D14
    f1_pin_afpp(GPIOD, 10);      // D15
    f1_pin_afpp(GPIOD, 11);      // A16 (RS)
    f1_pin_afpp(GPIOD, 14);      // D0
    f1_pin_afpp(GPIOD, 15);      // D1
    for (int p = 7; p <= 15; p++)
        f1_pin_afpp(GPIOE, p);   // D4..D12

    AFIO_MAPR |= (1 << 10);      // remap NADV

    *(volatile uint32_t *)0xA0000000U = (1 << 12) | (1 << 4) | (1 << 0);
    *(volatile uint32_t *)0xA0000004U = (15 << 8) | 15;

    xpt_init();
    hb_touch_deadline = 0;

    tft_cmd(0x01);               // SWRESET (el resto lo hace la tarea)
    hb_deadline = timer_read_time() + timer_from_us(150000);
}

DECL_INIT(hb_tft_init);

void
hb_tft_task(void)
{
    uint32_t now = timer_read_time();
    if (hb_flash_btn >= 0 && !timer_is_before(now, hb_flash_deadline)) {
        draw_button_full(hb_flash_btn);
        hb_flash_btn = -1;
    }
    xpt_poll(now);

    switch (hb_state) {
    case HB_WAIT_RESET:
        if (timer_is_before(now, hb_deadline))
            return;
        tft_cmd(0x11);           // SLPOUT
        hb_deadline = now + timer_from_us(150000);
        hb_state = HB_WAIT_SLEEP;
        break;

    case HB_WAIT_SLEEP:
        if (timer_is_before(now, hb_deadline))
            return;
        ili9488_config();
        hb_deadline = now + timer_from_us(50000);
        hb_state = HB_WAIT_PWR;
        break;

    case HB_WAIT_PWR:
        if (timer_is_before(now, hb_deadline))
            return;
        hb_state = HB_DRAW;
        hb_draw_step = 0;
        break;

    case HB_DRAW:
        if (hb_draw_step < BG_BANDS) {
            // una banda de fondo por llamada (~1ms de escrituras)
            tft_fill_rect(0, 12 + hb_draw_step * BG_BAND_H, TFT_W, BG_BAND_H
                          , 0x0000);
        } else {
            // un boton (con label) por llamada
            draw_button_full(hb_draw_step - BG_BANDS);
        }
        if (++hb_draw_step >= DRAW_STEPS) {
            hb_state = HB_DONE;
            // heartbeat de prueba cada 5s: prueba la entrega al host sin
            // depender del touch ni del timing de arranque (btn=255 se
            // loguea y se ignora). QUITAR al cerrar el diagnostico.
            hb_test_deadline = now + timer_from_us(5000000);
        }
        break;

    default: ; // HB_DONE: pantalla estatica
    }
}



// Setea el label de un boton (lo manda el modulo [hbtft_buttons] de klippy
// al conectar, leido de printer.cfg). El texto llega empaquetado en 4
// uint32 (4 caracteres ASCII por palabra, byte bajo primero) para no
// depender del soporte de strings del parser.
// Copia 4 palabras uint32 (4 ASCII por palabra, byte bajo primero) a dst
static void
hb_unpack(char *dst, uint32_t *args)
{
    for (int w = 0; w < 4; w++) {
        uint32_t word = args[w];
        dst[w * 4 + 0] = (char)(word & 0xFF);
        dst[w * 4 + 1] = (char)((word >> 8) & 0xFF);
        dst[w * 4 + 2] = (char)((word >> 16) & 0xFF);
        dst[w * 4 + 3] = (char)((word >> 24) & 0xFF);
    }
    dst[16] = 0;
    // trim de espacios finales (el host rellena el label a 16)
    for (int p = 15; p >= 0; p--) {
        if (dst[p] == ' ')
            dst[p] = 0;
        if (dst[p] != 0)
            break;
    }
}

void
command_hb_label(uint32_t *args)
{
    uint32_t idx = args[0];
    if (idx > 5)
        return;
    hb_unpack(hb_labels[idx], &args[1]);
    hb_labels2[idx][0] = 0;
    hb_big[idx] = 0;
    hb_colors[idx] = (uint16_t)args[5];
    if (hb_state == HB_DONE)
        draw_button_full(idx);
}
DECL_COMMAND(command_hb_label, "hb_label idx=%u w0=%u w1=%u w2=%u w3=%u color=%u");

// Etiqueta extendida: 2 lineas y/o fuente grande (escala 3).
// big=1: linea 1 en escala 3, linea 2 en escala 2.
// big=0 y line2 vacia: igual que hb_label.
void
command_hb_label2(uint32_t *args)
{
    uint32_t idx = args[0];
    if (idx > 5)
        return;
    hb_unpack(hb_labels[idx], &args[3]);
    hb_unpack(hb_labels2[idx], &args[7]);
    hb_big[idx] = (uint8_t)args[1];
    hb_icon[idx] = (uint8_t)args[2];
    if (hb_icon[idx] > HB_ICON_COUNT)
        hb_icon[idx] = 0;
    hb_colors[idx] = (uint16_t)args[11];
    if (hb_state == HB_DONE)
        draw_button_full(idx);
}
DECL_COMMAND(command_hb_label2, "hb_label2 idx=%u big=%u icon=%u w0=%u w1=%u w2=%u w3=%u w4=%u w5=%u w6=%u w7=%u color=%u");


/****************************************************************
 * Comandos de diagnostico desde klippy (consola)
 ****************************************************************/


// Relanza la maquina de estados (redibuja la escena)

DECL_TASK(hb_tft_task);
