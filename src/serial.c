#include <string.h>
#include <time.h>
#include <stdint.h>
#include "serial.h"
#include "charset.h"

/*
 * Direct ACIA 6551 register access at $DE00 (SwiftLink cartridge address).
 *
 * Phase 1 (AT commands): Direct ACIA polling, RX interrupts disabled.
 * Phase 2 (data mode):   NMI-driven receive into ring buffer.
 *
 * After serial_dial() succeeds, call serial_start_nmi() to switch to
 * interrupt-driven receive.  All subsequent serial_recv/serial_drain
 * calls read from the NMI ring buffer instead of ACIA directly.
 */

/* Control register: 9600 baud, 8N1, internal clock */
#define CTRL_9600_8N1  0x1E

/* Command register: DTR active, RX IRQ disabled, TX no IRQ + RTS low */
#define CMD_POLL_MODE  0x0B

/* NMI ring buffer (defined in nmi_acia.s) */
extern uint8_t          nmi_rx_buf[256];
extern volatile uint8_t nmi_rx_head;   /* Written by NMI handler */
extern uint8_t          nmi_rx_tail;   /* Read by main code */
extern void             nmi_rx_init(void);

/* Flag: are we using NMI-driven receive? */
static uint8_t nmi_active;

/* ~20ms delay for AT command bytes */
static void at_delay(void) {
    volatile uint16_t i;
    for (i = 0; i < 1500u; ++i) { }
}

/* ~1.5ms delay for data bytes (one byte time at 9600 baud) */
static void byte_delay(void) {
    volatile uint16_t i;
    for (i = 0; i < 100u; ++i) { }
}

uint8_t serial_init(void) {
    uint8_t status;

    nmi_active = 0;

    status = ACIA_STATUS;
    if (status == 0xFF) return 0;  /* No ACIA present */

    ACIA_CTRL = CTRL_9600_8N1;
    ACIA_CMD = CMD_POLL_MODE;

    /* Drain any pending byte */
    if (ACIA_STATUS & ACIA_RDRF) {
        (void)ACIA_DATA;
    }

    /* Wait for ip232 DTR handshake (VICE/tcpser need this) */
    {
        clock_t t = clock() + CLK_TCK;
        while (clock() < t) { }
    }

    return 1;
}

/* Send one byte with AT-speed delay */
static void send_at_byte(uint8_t b) {
    ACIA_DATA = b;
    at_delay();
}

/* Send AT command string (PETSCII -> ASCII conversion) */
static void send_at_str(const char *s) {
    while (*s) {
        send_at_byte(petscii_to_ascii((uint8_t)*s));
        ++s;
    }
}

uint8_t serial_dial(const char *hostport) {
    /* Send AT dial command */
    send_at_str("ATDT");
    send_at_str(hostport);
    send_at_byte(0x0D);  /* CR */

    /* Wait for TCP connection + drain unsolicited device data.
       3 seconds is enough for tcpser to establish the TCP connection. */
    {
        clock_t t = clock() + 3 * CLK_TCK;
        while (clock() < t) {
            if (ACIA_STATUS & ACIA_RDRF) {
                (void)ACIA_DATA;
            }
        }
    }

    return 1;  /* TODO: detect CONNECT vs ERROR if needed */
}

void serial_start_nmi(void) {
    /* Switch to NMI-driven receive.
       Must be called after serial_dial() succeeds. */
    nmi_rx_init();
    nmi_active = 1;
}

void serial_send(const uint8_t *buf, uint16_t len) {
    uint16_t i;
    for (i = 0; i < len; ++i) {
        ACIA_DATA = buf[i];
        byte_delay();
    }
}

uint16_t serial_recv(uint8_t *buf, uint16_t maxlen, uint16_t silence_limit) {
    uint16_t count = 0;
    uint16_t silence = 0;

    if (nmi_active) {
        /* Read from NMI ring buffer — bytes arrive via interrupt,
           no risk of loss during processing */
        while (count < maxlen && silence < silence_limit) {
            if (nmi_rx_head != nmi_rx_tail) {
                buf[count++] = nmi_rx_buf[nmi_rx_tail++];
                /* nmi_rx_tail wraps at 256 naturally (uint8_t) */
                silence = 0;
            } else {
                ++silence;
            }
        }
    } else {
        /* Direct ACIA polling (used during AT command phase) */
        while (count < maxlen && silence < silence_limit) {
            if (ACIA_STATUS & ACIA_RDRF) {
                buf[count++] = ACIA_DATA;
                silence = 0;
            } else {
                ++silence;
            }
        }
    }
    return count;
}

uint16_t serial_drain(uint16_t silence_limit) {
    uint16_t count = 0;
    uint16_t silence = 0;

    if (nmi_active) {
        while (silence < silence_limit) {
            if (nmi_rx_head != nmi_rx_tail) {
                ++nmi_rx_tail;
                ++count;
                silence = 0;
            } else {
                ++silence;
            }
        }
    } else {
        while (silence < silence_limit) {
            if (ACIA_STATUS & ACIA_RDRF) {
                (void)ACIA_DATA;
                ++count;
                silence = 0;
            } else {
                ++silence;
            }
        }
    }
    return count;
}

void serial_hangup(void) {
    clock_t t;

    /* Guard time: 1 second silence */
    t = clock() + CLK_TCK;
    while (clock() < t) { }

    send_at_str("+++");

    t = clock() + CLK_TCK;
    while (clock() < t) { }

    send_at_str("ATH");
    send_at_byte(0x0D);
}
