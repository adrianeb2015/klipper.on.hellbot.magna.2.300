// Raw bare-metal beacon for STM32F103VE @ 0x08007000 (MKS Robin Nano V1.2)
#include <stdint.h>

#define PERIPH_BASE 0x40000000U
#define RCC_BASE    (PERIPH_BASE + 0x21000)
#define GPIOA_BASE  (PERIPH_BASE + 0x10800)
#define GPIOB_BASE  (PERIPH_BASE + 0x10C00)
#define USART1_BASE (PERIPH_BASE + 0x13800)
#define USART3_BASE (PERIPH_BASE + 0x04800)

#define RCC_APB1ENR (*(volatile uint32_t *)(RCC_BASE + 0x1C))
#define RCC_APB2ENR (*(volatile uint32_t *)(RCC_BASE + 0x18))
#define RCC_CR      (*(volatile uint32_t *)(RCC_BASE + 0x00))
#define RCC_CFGR    (*(volatile uint32_t *)(RCC_BASE + 0x04))

typedef struct { volatile uint32_t CRL, CRH, IDR, ODR, BSRR, BRR, LCKR; } GPIO_t;
#define GPIOA ((GPIO_t *)GPIOA_BASE)
#define GPIOB ((GPIO_t *)GPIOB_BASE)

typedef struct { volatile uint32_t SR, DR, BRR, CR1, CR2, CR3, GTPR; } USART_t;
#define USART1 ((USART_t *)USART1_BASE)
#define USART3 ((USART_t *)USART3_BASE)

static inline void pin_afpp(GPIO_t *g, uint8_t pin)
{
    volatile uint32_t *cr = (pin < 8) ? &g->CRL : &g->CRH;
    uint8_t pos = (pin & 7) * 4;
    uint32_t v = *cr;
    v &= ~(0xFU << pos);
    v |= (0xBU << pos);
    *cr = v;
}

static void clock_init_72mhz(void)
{
    // HSE on, wait ready
    RCC_CR |= (1 << 16);
    while (!(RCC_CR & (1 << 17))) {}
    // FLASH latency 2 wait states
    (*(volatile uint32_t *)0x40022000U) |= 0x2; // FLASH_ACR
    // PLL x9 on HSE, enable, sw = PLL
    RCC_CFGR |= (1 << 16);            // PLLSRC = HSE
    RCC_CFGR &= ~(0xF << 18);
    RCC_CFGR |= (0x7 << 18);          // PLLMUL x9
    RCC_CR |= (1 << 24);              // PLLON
    while (!(RCC_CR & (1 << 25))) {}  // PLLRDY
    RCC_CFGR = (RCC_CFGR & ~0x3) | 0x2; // SW = PLL
    while ((RCC_CFGR & 0xC) != 0x8) {}  // SWS = PLL
}

static void uart_init(USART_t *u)
{
    u->BRR = 18 << 4;              // 72MHz / 16 / 250000 = 18
    u->CR1 = (1 << 13) | (1 << 3); // UE | TE
}

__attribute__((section(".isr_vector")))
const void *vector_table[] = {
    (void *)0x20010000,   // initial SP (top of 64KB RAM)
    (void *)0x08007011,   // Reset_Handler (thumb)
};

int main(void)
{
    RCC_APB2ENR |= (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 14); // A B D E + USART1
    RCC_APB1ENR |= (1 << 18);                                             // USART3

    pin_afpp(GPIOA, 9);   // USART1 TX
    pin_afpp(GPIOA, 10);
    pin_afpp(GPIOB, 10);  // USART3 TX
    pin_afpp(GPIOB, 11);
    pin_afpp(GPIOB, 0);   // FAN1
    pin_afpp(GPIOB, 1);   // FAN0

    GPIOB->BSRR = (1 << 0) | (1 << 1);  // FANS ON

    clock_init_72mhz();

    uart_init(USART1);
    uart_init(USART3);

    for (;;) {
        USART1->DR = 0x55;
        USART3->DR = 0x55;
        for (volatile uint32_t i = 0; i < 200000; i++) {}
    }
}

void Reset_Handler(void)
{
    main();
    for (;;) {}
}
