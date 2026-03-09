#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/* ACIA registers (SwiftLink at $DE00) */
#define ACIA_DATA   (*(volatile uint8_t *)0xDE00)
#define ACIA_STATUS (*(volatile uint8_t *)0xDE01)
#define ACIA_CMD    (*(volatile uint8_t *)0xDE02)
#define ACIA_CTRL   (*(volatile uint8_t *)0xDE03)

#define ACIA_RDRF   0x08  /* Receive Data Register Full */
#define ACIA_TDRE   0x10  /* Transmit Data Register Empty */

/* Initialize ACIA for 9600 8N1 polling mode.
   Includes DTR handshake wait. Returns 1 on success, 0 if no ACIA. */
uint8_t serial_init(void);

/* Dial host:port via AT modem. Waits for CONNECT + drains unsolicited data.
   Returns 1 on success, 0 on failure/timeout. */
uint8_t serial_dial(const char *hostport);

/* Switch to NMI-driven receive (call after serial_dial succeeds).
   After this, serial_recv reads from an interrupt-driven ring buffer
   so no bytes are lost during CPU-intensive processing. */
void serial_start_nmi(void);

/* Send raw bytes with per-byte delay (~1.5ms, safe for 9600 baud). */
void serial_send(const uint8_t *buf, uint16_t len);

/* Receive into buffer using tight silence-based polling.
   No clock() calls — uses iteration counter for timeout.
   silence_limit: idle iterations before returning (30000 ~ 0.5s).
   Returns number of bytes read. */
uint16_t serial_recv(uint8_t *buf, uint16_t maxlen, uint16_t silence_limit);

/* Discard all pending RX data. Returns count of bytes discarded. */
uint16_t serial_drain(uint16_t silence_limit);

/* Hangup modem (+++ATH) */
void serial_hangup(void);

#endif
