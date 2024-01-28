/*
 * Copyright (c) 2016, Devan Lai
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice
 * appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Common STM32F103 target functions */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/st_usbfs.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/desig.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/vector.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/nvic.h>

#include "flash_wch_ext.h"

#include "dapboot.h"
#include "dfu.h"
#include "target.h"
#include "config.h"
#include "backup.h"

#ifndef USES_GPIOA
#if (HAVE_USB_PULLUP_CONTROL == 0)
#define USES_GPIOA 1
#else
#define USES_GPIOA 0
#endif
#endif

#ifndef USES_GPIOB
#define USES_GPIOB 0
#endif

#ifndef USES_GPIOC
#define USES_GPIOC 0
#endif

#ifndef BUTTON_USES_PULL
#define BUTTON_USES_PULL 1
#endif

#ifdef FLASH_SIZE_OVERRIDE
_Static_assert((FLASH_BASE + FLASH_SIZE_OVERRIDE >= APP_BASE_ADDRESS),
               "Incompatible flash size");
#endif

#ifndef REG_BOOT
#define REG_BOOT BKP1
#endif

#ifndef CMD_BOOT
#define CMD_BOOT 0x4F42UL
#endif



void target_flash_erase_page(uint32_t adr);
bool target_flash_page ( uint32_t *adr, size_t sz, const uint16_t *buf);
#define UART_BUF_SIZE      1024
uint8_t ringbuf[UART_BUF_SIZE];

uint32_t ringbuf_rd = 0;
uint32_t ringbuf_wr = 0;
void uart_send_data ( uint8_t *buf, int32_t size);

static void target_usart_setup(void)
{
    /* Setup GPIO pin GPIO_USART1_TX/GPIO9 on GPIO port A for transmit. */
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
              GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);

    /* Enable the USART1 interrupt. */
    nvic_enable_irq(NVIC_USART1_IRQ);

    /* Setup UART parameters. */
    usart_set_baudrate(USART1, 115200);
    usart_set_databits(USART1, 8);
    usart_set_stopbits(USART1, USART_STOPBITS_1);
    usart_set_mode(USART1, USART_MODE_TX);
    usart_set_parity(USART1, USART_PARITY_NONE);
    usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);

    /* Finally enable the USART. */
    usart_enable(USART1);
}


void usart1_isr(void)
{
    /* Check if we were called because of TXE. */
    if (((USART_CR1(USART1) & USART_CR1_TXEIE) != 0) &&
        ((USART_SR(USART1) & USART_SR_TXE) != 0)) {
        if (ringbuf_rd == ringbuf_wr) {
            /* Disable the TXE interrupt, it's no longer needed. */
            USART_CR1(USART1) &= ~USART_CR1_TXEIE;
        } else {
            /* Put data into the transmit register. */
            usart_send(USART1, ringbuf[ringbuf_rd++]);
            if (ringbuf_rd>=UART_BUF_SIZE) ringbuf_rd = 0;
        }
    }
}

void uart_send_data ( uint8_t *buf, int32_t size)
{
    uint32_t wr_nxt;
    while (size > 0) {
        wr_nxt = ringbuf_wr +1;
        if ( wr_nxt >= UART_BUF_SIZE) wr_nxt = 0;
        if ( wr_nxt == ringbuf_rd ) {
            // overflow, discard remaning data
            return;
        }
        ringbuf[ringbuf_wr] = *buf++;
        size--;
        ringbuf_wr = wr_nxt;
    }
    USART_CR1(USART1) |= USART_CR1_TXEIE;
}

void target_clock_setup(void) {
#ifdef USE_HSI
    /* Set the system clock to 48MHz from the internal RC oscillator.
       The clock tolerance doesn't meet the official USB spec, but
       it's better than nothing. */
    rcc_clock_setup_in_hsi_out_48mhz();
#else
    /* Set system clock to 72 MHz from an external crystal */
    rcc_clock_setup_in_hse_8mhz_out_72mhz();
#endif
    /* Enable clocks for GPIO port A (for GPIO_USART1_TX) and USART1. */
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_USART1);
}

void target_gpio_setup(void) {
    /* Enable GPIO clocks */
#if USES_GPIOA
    rcc_periph_clock_enable(RCC_GPIOA);
#endif
#if USES_GPIOB
    rcc_periph_clock_enable(RCC_GPIOB);
#endif
#if USES_GPIOC
    rcc_periph_clock_enable(RCC_GPIOC);
#endif

    /* Disable SWD if PA13 and/or PA14 are used for another purpose */
#if ((HAVE_LED && LED_GPIO_PORT == GPIOA && (LED_GPIO_PORT == GPIO13 || LED_GPIO_PORT == GPIO14)) || \
    (HAVE_BUTTON && BUTTON_GPIO_PORT == GPIOA && (BUTTON_GPIO_PIN == GPIO13 || BUTTON_GPIO_PIN == GPIO14)) || \
    (HAVE_USB_PULLUP_CONTROL && USB_PULLUP_GPIO_PORT == GPIOA && \
        (USB_PULLUP_GPIO_PIN == GPIO13 || USB_PULLUP_GPIO_PIN == GPIO14)))
    {
        rcc_periph_clock_enable(RCC_AFIO);
        gpio_primary_remap(AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_OFF, 0);
    }
#endif

    /* Setup LEDs */
#if HAVE_LED
    {
        const uint8_t mode = GPIO_MODE_OUTPUT_10_MHZ;
        const uint8_t conf = (LED_OPEN_DRAIN ? GPIO_CNF_OUTPUT_OPENDRAIN
                                             : GPIO_CNF_OUTPUT_PUSHPULL);
        if (LED_OPEN_DRAIN) {
            gpio_set(LED_GPIO_PORT, LED_GPIO_PIN);
        } else {
            gpio_clear(LED_GPIO_PORT, LED_GPIO_PIN);
        }
        gpio_set_mode(LED_GPIO_PORT, mode, conf, LED_GPIO_PIN);

        /* add systick for LED blinking */
        systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
        systick_set_reload(899999);
        systick_interrupt_enable();
        systick_counter_enable();
    }
#endif

    /* Setup the internal pull-up/pull-down for the button */
#if HAVE_BUTTON
    {
        const uint8_t mode = GPIO_MODE_INPUT;
        const uint8_t conf = (BUTTON_USES_PULL ? GPIO_CNF_INPUT_PULL_UPDOWN
                                               : GPIO_CNF_INPUT_FLOAT);
        gpio_set_mode(BUTTON_GPIO_PORT, mode, conf, BUTTON_GPIO_PIN);
        if (BUTTON_USES_PULL) {
            if (BUTTON_ACTIVE_HIGH) {
                gpio_clear(BUTTON_GPIO_PORT, BUTTON_GPIO_PIN);
            } else {
                gpio_set(BUTTON_GPIO_PORT, BUTTON_GPIO_PIN);
            }
        }
    }
#endif

#if HAVE_USB_PULLUP_CONTROL
    {
        const uint8_t mode = GPIO_MODE_OUTPUT_10_MHZ;
        const uint8_t conf = (USB_PULLUP_OPEN_DRAIN ? GPIO_CNF_OUTPUT_OPENDRAIN
                                                    : GPIO_CNF_OUTPUT_PUSHPULL);
        /* Configure USB pullup transistor, initially disabled */
        if (USB_PULLUP_ACTIVE_HIGH) {
            gpio_clear(USB_PULLUP_GPIO_PORT, USB_PULLUP_GPIO_PIN);
        } else {
            gpio_set(USB_PULLUP_GPIO_PORT, USB_PULLUP_GPIO_PIN);
        }
        gpio_set_mode(USB_PULLUP_GPIO_PORT, mode, conf, USB_PULLUP_GPIO_PIN);
    }
#else
    {
        /* Drive the USB DP pin to override the pull-up */
        gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_10_MHZ,
                      GPIO_CNF_OUTPUT_PUSHPULL, GPIO12);
    }
#endif

    target_usart_setup();
}

const usbd_driver* target_usb_init(void) {
    rcc_periph_reset_pulse(RST_USB);

#if HAVE_USB_PULLUP_CONTROL
    /* Enable USB pullup to connect */
    if (USB_PULLUP_ACTIVE_HIGH) {
        gpio_set(USB_PULLUP_GPIO_PORT, USB_PULLUP_GPIO_PIN);
    } else {
        gpio_clear(USB_PULLUP_GPIO_PORT, USB_PULLUP_GPIO_PIN);
    }
#else
    /* Override hard-wired USB pullup to disconnect and reconnect */
    gpio_clear(GPIOA, GPIO12);
    int i;
    for (i = 0; i < 800000; i++) {
        __asm__("nop");
    }
#endif

    return &st_usbfs_v1_usb_driver;
}

bool target_get_force_bootloader(void) {
    bool force = false;
    /* Check the RTC backup register */
    uint16_t cmd = backup_read(REG_BOOT);
    if (cmd == CMD_BOOT) {
        force = true;
    }

    /* Clear the RTC backup register */
    backup_write(REG_BOOT, 0);

#if HAVE_BUTTON
    /* Wait some time in case the button has some debounce capacitor */
    int i;
    for (i = 0; i < BUTTON_SAMPLE_DELAY_CYCLES; i++) {
        __asm__("nop");
    }
    /* Check if the user button is held down */
    if (BUTTON_ACTIVE_HIGH) {
        if (gpio_get(BUTTON_GPIO_PORT, BUTTON_GPIO_PIN)) {
            force = true;
        }
    } else {
        if (!gpio_get(BUTTON_GPIO_PORT, BUTTON_GPIO_PIN)) {
            force = true;
        }
    }
#endif

    return force;
}

void target_get_serial_number(char* dest, size_t max_chars) {
    desig_get_unique_id_as_string(dest, max_chars+1);
}

static uint16_t* get_flash_end(void) {
#ifdef FLASH_SIZE_OVERRIDE
    /* Allow access to the unofficial full 128KiB flash size */
    return (uint16_t*)(FLASH_BASE + FLASH_SIZE_OVERRIDE);
#else
    /* Only allow access to the chip's self-reported flash size */
    return (uint16_t*)(FLASH_BASE + ((size_t)desig_get_flash_size())*1024);
#endif
}

size_t target_get_max_firmware_size(void) {
    uint8_t* flash_end = (uint8_t*)get_flash_end();
    uint8_t* flash_start = (uint8_t*)(APP_BASE_ADDRESS);

    return (flash_end >= flash_start) ? (size_t)(flash_end - flash_start) : 0;
}

void target_relocate_vector_table(void) {
    SCB_VTOR = APP_BASE_ADDRESS & 0xFFFF;
}

#if defined(CH32F1) && (CH32F1 == 1)
void target_flash_unlock(void){
    FLASH_KEYR = FLASH_KEYR_KEY1;
    FLASH_KEYR = FLASH_KEYR_KEY2;
    FLASH_MODEKEYP = FLASH_KEYR_KEY1;
    FLASH_MODEKEYP = FLASH_KEYR_KEY2;
    uint8_t ubuf[1];
    ubuf[0] = 'U';
    uart_send_data(ubuf,1);
}

void target_flash_lock(void) {
    FLASH_CR |= FLASH_CR_LOCK;
    uint8_t ubuf[1];
    ubuf[0] = 'L';
    uart_send_data(ubuf,1);
}

void target_flash_erase_page(uint32_t adr) {
    adr &= ~(0x7f);
    FLASH_CR |= FLASH_CR_PAGE_ERASE;
    FLASH_AR = adr;
    FLASH_CR |= FLASH_CR_STRT;
    while ( FLASH_SR & FLASH_SR_BSY ) { }
    FLASH_CR &= ~FLASH_CR_PAGE_ERASE;
    *(volatile uint32_t*)0x40022034 = *(volatile uint32_t*)(adr^ 0x00000100);    // taken from example
    uint8_t ubuf[1];
    ubuf[0] = 'E';
    uart_send_data(ubuf,1);
}

bool target_flash_page ( uint32_t *adr, size_t sz, const uint16_t *buf) {
    uint8_t msg[18] = {'A','=',0,0,0,0,0,0,0,0,',','C','=',0,0,0,0,'\n'};

    uint32_t msgdata = (uint32_t) adr;
    for (int i = 0; i < 8; i++) {
        msg[9-i] = "0123456789ABCDEF"[msgdata & 0xf];
        msgdata>>=4;
    }
    msgdata = (uint32_t) sz;
    for (int i = 0; i < 4; i++) {
        msg[16-i] = "0123456789ABCDEF"[msgdata & 0xf];
        msgdata>>=4;
    }
    uart_send_data(msg,18);

    /* if not aligned return false */
    uint32_t    prg_adr = (uint32_t) adr;
    if ( (prg_adr & 0x7f) != 0 ) return false;
    if ( sz > 64 ) return false;          // should less than 128byte
    FLASH_CR |= FLASH_CR_PAGE_PROGRAM;    // enable page programing
    FLASH_CR |= FLASH_CR_BUF_RST;         // page buffer reset
    while ( FLASH_SR & FLASH_SR_BSY );
    FLASH_CR &= ~FLASH_CR_PAGE_PROGRAM;

    while (sz) {
        uint32_t cnt = 4;
        FLASH_CR |= FLASH_CR_PAGE_PROGRAM;    // enable page programing
        uint32_t    s_adr = (uint32_t) adr;
        while ( (sz > 1) && cnt ) {
            *adr = * ((uint32_t *)buf);
            adr ++;
            buf += 2;
            sz -= 2;
            cnt --;
        }
        if ( (sz != 0) && cnt ) {
            *adr = 0xffff0000 | (*buf);
            adr ++;
            buf ++;
            sz --;
            cnt --;
        }
        FLASH_CR |= FLASH_CR_BUF_LOAD;
        while ( FLASH_SR & FLASH_SR_BSY );
        FLASH_CR &= ~FLASH_CR_PAGE_PROGRAM;
        *(volatile uint32_t*)0x40022034 = *(volatile uint32_t*)(s_adr ^ 0x00000100);    // taken from example
    }

    FLASH_CR |= FLASH_CR_PAGE_PROGRAM;    // enable page programing
    FLASH_AR = prg_adr;
    FLASH_CR |= FLASH_CR_STRT;
    while ( FLASH_SR & FLASH_SR_BSY );
    FLASH_CR &= ~FLASH_CR_PAGE_PROGRAM;
    *(volatile uint32_t*)0x40022034 = *(volatile uint32_t*)(prg_adr ^ 0x00000100);      // taken from example
    if ( FLASH_SR  & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR) ) {
        FLASH_SR  |= FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
        return false;
    }
    uint8_t ubuf[1];
    ubuf[0] = 'P';
    uart_send_data(ubuf,1);
    return true;
}

bool target_flash_program_array(uint16_t* dest, const uint16_t* data, size_t half_word_count) {
    size_t    cnt;

    uint8_t msg[18] = {'A','=',0,0,0,0,0,0,0,0,',','C','=',0,0,0,0,'\n'};

    uint32_t msgdata = (uint32_t) dest;
    for (int i = 0; i < 8; i++) {
        msg[9-i] = "0123456789ABCDEF"[msgdata & 0xf];
        msgdata>>=4;
    }
    msgdata = (uint32_t) half_word_count;
    for (int i = 0; i < 4; i++) {
        msg[16-i] = "0123456789ABCDEF"[msgdata & 0xf];
        msgdata>>=4;
    }
    uart_send_data(msg,18);
    if (  ( ((uint32_t) dest ) & 0x7f ) != 0 ) return false;

    for ( cnt = 0; cnt < half_word_count ; cnt+=64 ){
        uint32_t ptr = cnt*2 + ((uint32_t) dest);
        size_t sz = half_word_count -cnt;
        if ( sz > 64) sz = 64;
        target_flash_erase_page( ptr );
        if (  ! target_flash_page( (uint32_t *) ptr, sz, data + cnt ) ) return false;
        for ( uint32_t i = 0; i < sz; i++) if ( dest[cnt+i] != data[cnt+i]) return false;
    }

    return true;
}

#else

void target_flash_unlock(void) {
    flash_unlock();
}

void target_flash_lock(void) {
    flash_lock();
}

static inline uint16_t* get_flash_page_address(uint16_t* dest) {
    return (uint16_t*)(((uint32_t)dest / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE);
}

bool target_flash_program_array(uint16_t* dest, const uint16_t* data, size_t half_word_count) {
    bool verified = true;

    /* Remember the bounds of erased data in the current page */
    static uint16_t* erase_start;
    static uint16_t* erase_end;

    {
        uint8_t buf[8];
        size_t sz = half_word_count;
        buf[0] = 'S';
        buf[7] = '|';
        for ( int i = 0; i < 6; i++) {
            buf[6-i] = '0' + sz % 10;
            sz /= 10;
        }
        uart_send_data(buf,8);
    }
    const uint16_t* flash_end = get_flash_end();
    while (half_word_count > 0) {
        /* Avoid writing past the end of flash */
        if (dest >= flash_end) {
            verified = false;
            break;
        }

        if (dest >= erase_end || dest < erase_start) {
            erase_start = get_flash_page_address(dest);
            erase_end = erase_start + (FLASH_PAGE_SIZE)/sizeof(uint16_t);
            flash_erase_page((uint32_t)erase_start);
        }
        flash_program_half_word((uint32_t)dest, *data);
        erase_start = dest + 1;
        if (*dest != *data) {
            verified = false;
            break;
        }
        dest++;
        data++;
        half_word_count--;
    }

    return verified;
}
#endif

#if HAVE_LED
void sys_tick_handler(void)
{
    static uint8_t count = 0 ;
    static uint8_t msg[8] = {'.','I','=',0,0,0,0,'\n'};
    count ++;
    if ( count >= ( dfu_is_idle() ? 5 : 1 ) ){
        uint16_t msgdata = dfu_state();
        for (int i = 0; i < 4; i++) {
            msg[6-i] = "0123456789ABCDEF"[msgdata & 0xf];
            msgdata>>=4;
        }
        uart_send_data(msg,8);
        count = 0;
        gpio_toggle(LED_GPIO_PORT, LED_GPIO_PIN);
    }
}

#endif
