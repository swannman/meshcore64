#ifndef CHARSET_H
#define CHARSET_H

#include <stdint.h>

/* Convert PETSCII byte to screen code for direct screen RAM write.
   Works with cc65 string literals (which are already PETSCII). */
static uint8_t petscii_to_screen(uint8_t c) {
    if (c < 0x20u) return c + 0x80u;
    if (c < 0x40u) return c;          /* space, digits, punct */
    if (c < 0x60u) return c - 0x40u;  /* unshifted letters → $00-$1F (lowercase) */
    if (c < 0x80u) return c - 0x20u;
    if (c < 0xA0u) return c + 0x40u;
    if (c < 0xC0u) return c - 0x40u;
    return c - 0x80u;                  /* shifted letters → $40-$5F (uppercase) */
}

/* Convert network ASCII byte to PETSCII for internal storage.
   After this, petscii_to_screen() can be used for display.
   IMPORTANT: cc65 char literals are PETSCII, not ASCII.
   Must use hex constants for ASCII range checks. */
static uint8_t ascii_to_petscii(uint8_t c) {
    if (c >= 0x61u && c <= 0x7Au) return c - 0x20u;           /* ASCII a-z → PETSCII 0x41-0x5A */
    if (c >= 0x41u && c <= 0x5Au) return (uint8_t)(c + 0x80u); /* ASCII A-Z → PETSCII 0xC1-0xDA */
    if (c >= 0x20u && c <= 0x5Fu) return c;                    /* space, digits, punct, @, []\^_ */
    return 0;  /* 0 = skip (control chars and unmapped) */
}

/* Convert PETSCII keystroke/literal to ASCII for network transmission.
   Note: cc65 '\r' is 0x0A (PETSCII CR), must convert to ASCII 0x0D. */
static uint8_t petscii_to_ascii(uint8_t c) {
    if (c == 0x0Au) return 0x0Du;                      /* cc65 CR → ASCII CR */
    if (c >= 0xC1u && c <= 0xDAu) return c - 0x80u;   /* shifted → A-Z */
    if (c >= 0x41u && c <= 0x5Au) return c + 0x20u;    /* unshifted → a-z */
    return c;
}

#endif
