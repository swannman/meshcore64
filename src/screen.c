#include <conio.h>
#include <string.h>
#include <c64.h>
#include "screen.h"
#include "charset.h"

/* Screen RAM base address (COLOR_RAM already in c64.h) */
#define SCREEN_RAM  ((uint8_t *)0x0400)

/* Colors */
#define COL_BORDER    0   /* black */
#define COL_BG        0   /* black */
#define COL_TEXT     14   /* light blue */
#define COL_SENDER   1   /* white */
#define COL_SYSTEM   7   /* yellow */
#define COL_MSG_NAME  6   /* dark blue - other sender names */
#define COL_MSG_TEXT 15   /* light gray - other message text */
#define COL_MY_NAME  14   /* light blue - my name */
#define COL_MY_TEXT   1   /* white - my message text */
#define COL_PROMPT    7   /* yellow - input prompt */

/* Size of message area in bytes */
#define BUF_SIZE  (MSG_LINES * SCR_W)   /* 21 * 40 = 840 */

/* Per-channel screen buffer */
typedef struct {
    uint8_t scr[BUF_SIZE];
    uint8_t col[BUF_SIZE];
    uint8_t next_line;   /* 0-based: next line within buffer (0 = top) */
} chan_buf_t;

static chan_buf_t chan_bufs[MAX_SCREEN_CHANNELS];
static uint8_t cur_ch;

/* Local device name for @-mention highlighting */
static char my_name[20];
static uint8_t my_name_len;

void screen_set_my_name(const char *name) {
    uint8_t i = 0;
    while (name[i] && i < 19) {
        my_name[i] = name[i];
        ++i;
    }
    my_name[i] = '\0';
    my_name_len = i;
}

/* PETSCII horizontal line character */
#define HLINE_CHAR  0x40

static void draw_hline(uint8_t row, uint8_t color) {
    uint8_t i;
    uint8_t *sp = SCREEN_RAM + (uint16_t)row * SCR_W;
    uint8_t *cp = COLOR_RAM + (uint16_t)row * SCR_W;
    for (i = 0; i < SCR_W; ++i) {
        sp[i] = HLINE_CHAR;
        cp[i] = color;
    }
}

void screen_init(void) {
    /* Switch to lowercase charset */
    *(uint8_t *)0xD018 = 0x17;

    bordercolor(COL_BORDER);
    bgcolor(COL_BG);
    textcolor(COL_TEXT);
    clrscr();

    draw_hline(SEP_ROW2, 6);  /* dark blue */

    cur_ch = 0;
}

void screen_init_channel_bufs(void) {
    uint8_t i;
    for (i = 0; i < MAX_SCREEN_CHANNELS; ++i) {
        memset(chan_bufs[i].scr, 0x20, BUF_SIZE);
        memset(chan_bufs[i].col, COL_TEXT, BUF_SIZE);
        chan_bufs[i].next_line = 0;
    }
}

/* Copy channel buffer to screen RAM message area */
static void buf_to_screen(uint8_t ch) {
    uint16_t off = (uint16_t)MSG_TOP * SCR_W;
    memcpy(SCREEN_RAM + off, chan_bufs[ch].scr, BUF_SIZE);
    memcpy(COLOR_RAM + off, chan_bufs[ch].col, BUF_SIZE);
}

void screen_set_channel(uint8_t ch) {
    if (ch >= MAX_SCREEN_CHANNELS) return;
    cur_ch = ch;
    buf_to_screen(ch);
}

void screen_status(const char *companion, const char *channel, uint8_t dm_mode) {
    uint8_t i;
    uint8_t *sp = SCREEN_RAM;
    uint8_t *cp = COLOR_RAM;
    const char *src;
    uint8_t col;

    /* Fill row 0 with dark blue (reverse video = dark blue background) */
    for (i = 0; i < SCR_W; ++i) {
        sp[i] = 0x20;
        cp[i] = 6;
    }

    /* Write companion name */
    col = 1;
    src = companion;
    while (*src && col < 20) {
        sp[col] = petscii_to_screen((uint8_t)*src);
        ++src;
        ++col;
    }

    /* Separator */
    sp[col] = 0x20;
    ++col;

    /* Write channel name */
    src = channel;
    while (*src && col < SCR_W - 8) {
        sp[col] = petscii_to_screen((uint8_t)*src);
        ++src;
        ++col;
    }

    /* Nav hint right-aligned: "<F1 F3>" or "<F5 F7>" */
    {
        static const uint8_t hint_ch[] = {
            0x3c, 0x46, 0x31,       /* <F1 */
            0x20,                    /* space */
            0x46, 0x33, 0x3e        /* F3> */
        };
        static const uint8_t hint_dm[] = {
            0x3c, 0x46, 0x35,       /* <F5 */
            0x20,                    /* space */
            0x46, 0x37, 0x3e        /* F7> */
        };
        const uint8_t *hint = dm_mode ? hint_dm : hint_ch;
        uint8_t h;
        for (h = 0; h < 7; ++h) {
            sp[SCR_W - 7 + h] = hint[h];
        }
    }

    /* Set reverse video by OR'ing $80 into screen codes */
    for (i = 0; i < SCR_W; ++i) {
        sp[i] |= 0x80;
    }
}

/* ---- Buffer-based rendering ---- */

/* Scroll a channel buffer up by one line */
static void buf_scroll_up(chan_buf_t *buf) {
    memmove(buf->scr, buf->scr + SCR_W, (MSG_LINES - 1) * SCR_W);
    memmove(buf->col, buf->col + SCR_W, (MSG_LINES - 1) * SCR_W);
    memset(buf->scr + (MSG_LINES - 1) * SCR_W, 0x20, SCR_W);
    memset(buf->col + (MSG_LINES - 1) * SCR_W, COL_TEXT, SCR_W);
}

/* Write one line into a channel buffer at given line offset (0-based) */
static void buf_write_line(chan_buf_t *buf, uint8_t line,
                            const char *sender, const char *text,
                            uint8_t sender_color, uint8_t text_color) {
    uint8_t *sp = buf->scr + (uint16_t)line * SCR_W;
    uint8_t *cp = buf->col + (uint16_t)line * SCR_W;
    uint8_t col = 0;
    const char *src;

    /* Clear the line */
    memset(sp, 0x20, SCR_W);
    memset(cp, text_color, SCR_W);

    /* Write sender if present */
    if (sender && *sender) {
        src = sender;
        while (*src && col < SCR_W) {
            sp[col] = petscii_to_screen((uint8_t)*src);
            cp[col] = sender_color;
            ++src;
            ++col;
        }
        if (col < SCR_W - 1) {
            sp[col] = petscii_to_screen(':');
            cp[col] = sender_color;
            ++col;
        }
        if (col < SCR_W) {
            ++col;  /* space after colon */
        }
    }

    /* Write text, highlighting @[...] mentions.
       @[my_name] = yellow, other @[...] = light blue */
    {
        uint8_t in_mention = 0;
        uint8_t mention_color = COL_TEXT;
        src = text;
        while (*src && col < SCR_W) {
            /* Detect start of @[ and determine color */
            if (!in_mention && *src == '@' && *(src + 1) == '[') {
                in_mention = 1;
                /* Check if this mentions my name: @[name] */
                if (my_name_len > 0 &&
                    memcmp(src + 2, my_name, my_name_len) == 0 &&
                    src[2 + my_name_len] == ']') {
                    mention_color = COL_SYSTEM;  /* yellow */
                } else {
                    mention_color = COL_TEXT;  /* light blue */
                }
            }
            sp[col] = petscii_to_screen((uint8_t)*src);
            cp[col] = in_mention ? mention_color : text_color;
            if (in_mention && *src == ']') {
                in_mention = 0;
            }
            ++src;
            ++col;
        }
    }
}

/* Add a message to a specific channel's buffer with explicit colors */
void screen_add_message_ch(uint8_t ch, const char *sender, const char *text,
                           uint8_t name_color, uint8_t text_color) {
    chan_buf_t *buf;
    const char *p;
    char line[SCR_W + 1];
    uint8_t line_len;
    uint8_t first;
    uint8_t avail;
    char parsed_sender[SCR_W + 1];

    if (ch >= MAX_SCREEN_CHANNELS) return;
    buf = &chan_bufs[ch];

    /* If no explicit sender, try to parse "name: text" from the text */
    if ((!sender || !*sender) && text) {
        const char *colon = strchr(text, ':');
        if (colon && (colon - text) < SCR_W - 2) {
            uint8_t slen = (uint8_t)(colon - text);
            memcpy(parsed_sender, text, slen);
            parsed_sender[slen] = '\0';
            sender = parsed_sender;
            text = colon + 1;
            if (*text == ' ') ++text;  /* skip space after colon */
        }
    }

    p = text;
    first = 1;

    while (*p) {
        /* Calculate available width */
        if (first && sender && *sender) {
            avail = SCR_W - (uint8_t)strlen(sender) - 2;  /* "name: " */
        } else {
            avail = SCR_W;
        }

        /* Copy up to avail chars */
        line_len = 0;
        while (*p && line_len < avail) {
            line[line_len++] = *p++;
        }
        line[line_len] = '\0';

        /* Scroll if needed */
        if (buf->next_line >= MSG_LINES) {
            buf_scroll_up(buf);
            buf->next_line = MSG_LINES - 1;
        }

        if (first) {
            buf_write_line(buf, buf->next_line, sender, line,
                           name_color, text_color);
            first = 0;
        } else {
            buf_write_line(buf, buf->next_line, NULL, line,
                           text_color, text_color);
        }
        ++(buf->next_line);
    }

    /* Handle empty text */
    if (first) {
        if (buf->next_line >= MSG_LINES) {
            buf_scroll_up(buf);
            buf->next_line = MSG_LINES - 1;
        }
        buf_write_line(buf, buf->next_line, sender, "", name_color, text_color);
        ++(buf->next_line);
    }

    /* If this is the displayed channel, update screen */
    if (ch == cur_ch) {
        buf_to_screen(ch);
    }
}

/* Add message to current channel (convenience) — uses "other" colors */
void screen_add_message(const char *sender, const char *text) {
    screen_add_message_ch(cur_ch, sender, text, COL_MSG_NAME, COL_MSG_TEXT);
}

void screen_clear_messages(void) {
    chan_buf_t *buf = &chan_bufs[cur_ch];
    memset(buf->scr, 0x20, BUF_SIZE);
    memset(buf->col, COL_TEXT, BUF_SIZE);
    buf->next_line = 0;
    buf_to_screen(cur_ch);
}

void screen_input_draw(const char *buf, uint8_t cursor_pos,
                       const char *mention) {
    uint8_t *sp = SCREEN_RAM + (uint16_t)INPUT_ROW * SCR_W;
    uint8_t *cp = COLOR_RAM + (uint16_t)INPUT_ROW * SCR_W;
    uint8_t col = 0;
    uint8_t prefix_len = 0;
    const char *src;

    /* Clear line */
    memset(sp, 0x20, SCR_W);
    memset(cp, COL_TEXT, SCR_W);

    /* Mention prefix: "@[name] " in yellow */
    if (mention && *mention) {
        sp[col] = petscii_to_screen('@'); cp[col] = COL_SYSTEM; ++col;
        sp[col] = petscii_to_screen('['); cp[col] = COL_SYSTEM; ++col;
        src = mention;
        while (*src && col < SCR_W - 4) {
            sp[col] = petscii_to_screen((uint8_t)*src);
            cp[col] = COL_SYSTEM;
            ++src; ++col;
        }
        sp[col] = petscii_to_screen(']'); cp[col] = COL_SYSTEM; ++col;
        sp[col] = 0x20; ++col;  /* space after mention */
        prefix_len = col;
    }

    /* Prompt "> " (yellow) */
    sp[col] = petscii_to_screen('>');
    cp[col] = COL_PROMPT;
    ++col;
    sp[col] = 0x20;
    ++col;

    prefix_len = col;  /* total prefix before user text */

    /* Buffer text (white) */
    src = buf;
    while (*src && col < SCR_W) {
        sp[col] = petscii_to_screen((uint8_t)*src);
        cp[col] = COL_SENDER;
        ++src;
        ++col;
    }

    /* Cursor: reverse-video space at cursor position */
    col = prefix_len + cursor_pos;
    if (col < SCR_W) {
        sp[col] |= 0x80;
    }
}

void screen_system_msg(const char *text) {
    chan_buf_t *buf = &chan_bufs[cur_ch];
    const char *p;
    char line[SCR_W + 1];
    uint8_t line_len;

    p = text;

    while (*p) {
        line_len = 0;
        while (*p && line_len < SCR_W) {
            line[line_len++] = *p++;
        }
        line[line_len] = '\0';

        if (buf->next_line >= MSG_LINES) {
            buf_scroll_up(buf);
            buf->next_line = MSG_LINES - 1;
        }
        buf_write_line(buf, buf->next_line, NULL, line, COL_SYSTEM, COL_SYSTEM);
        ++(buf->next_line);
    }

    buf_to_screen(cur_ch);
}

void screen_confirm_line(uint8_t ch, uint8_t skip,
                         uint8_t old_color, uint8_t new_color) {
    chan_buf_t *b;
    uint8_t *cp;
    uint8_t i, l, limit;

    if (ch >= MAX_SCREEN_CHANNELS) return;
    b = &chan_bufs[ch];

    limit = b->next_line;
    if (limit > MSG_LINES) limit = MSG_LINES;

    /* Scan forward for sender lines with old_color.
       Skip system messages (where ALL columns are old_color)
       by checking that column SCR_W-1 differs from old_color.
       Skip `skip` matches before confirming. */
    for (l = 0; l < limit; ++l) {
        cp = b->col + (uint16_t)l * SCR_W;
        if (cp[0] == old_color && cp[SCR_W - 1] != old_color) {
            if (skip > 0) {
                --skip;
                continue;
            }
            for (i = 0; i < SCR_W; ++i) {
                if (cp[i] == old_color) {
                    cp[i] = new_color;
                } else {
                    break;
                }
            }
            if (ch == cur_ch) {
                buf_to_screen(ch);
            }
            return;
        }
    }
}
