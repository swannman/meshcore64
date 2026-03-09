#include <conio.h>
#include <string.h>
#include "input.h"
#include "screen.h"

static char buf[INPUT_MAX_LEN + 1];
static uint8_t pos;  /* cursor / length */
static const char *mention;  /* current @-mention name, or NULL */

static void redraw(void) {
    screen_input_draw(buf, pos, mention);
}

void input_init(void) {
    buf[0] = '\0';
    pos = 0;
    mention = 0;
    redraw();
}

uint8_t input_poll(void) {
    char c;

    if (!kbhit()) return INPUT_NONE;

    c = cgetc();

    switch (c) {
        case 0x0D:  /* RETURN */
            return INPUT_SUBMIT;

        case 0x14:  /* DEL (backspace) */
            if (pos > 0) {
                --pos;
                buf[pos] = '\0';
                redraw();
            }
            return INPUT_NONE;

        case 0x85:  /* F1 */
            return INPUT_NEXT_CH;

        case 0x86:  /* F3 */
            return INPUT_PREV_CH;

        case 0x89:  /* F2 */
            return INPUT_SETTINGS;

        case 0x87:  /* F5 */
            return INPUT_NEXT_CONTACT;

        case 0x88:  /* F7 */
            return INPUT_PREV_CONTACT;

        case 0x91u:  /* Cursor UP */
            return INPUT_UP;

        case 0x11:  /* Cursor DOWN */
            return INPUT_DOWN;

        default:
            /* Store printable PETSCII characters directly.
               Unshifted letters: $41-$5A, shifted: $C1-$DA,
               space/punct/digits: $20-$3F */
            if (pos < INPUT_MAX_LEN &&
                ((c >= 0x20 && c <= 0x5A) ||
                 (c >= 0xC1u && c <= 0xDAu))) {
                buf[pos] = c;
                ++pos;
                buf[pos] = '\0';
                redraw();
            }
            return INPUT_NONE;
    }
}

const char *input_buf(void) {
    return buf;
}

void input_clear(void) {
    buf[0] = '\0';
    pos = 0;
    mention = 0;
    redraw();
}

void input_set_mention(const char *name) {
    mention = name;
    redraw();
}

const char *input_mention(void) {
    return mention;
}
