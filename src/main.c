#include <string.h>
#include <time.h>
#include <conio.h>
#include "screen.h"
#include "serial.h"
#include "meshcore.h"
#include "input.h"
#include "config.h"
#include "charset.h"

/* Default connection target (fallback if no config) */
#define DEFAULT_HOST "192.168.2.145:5000"

/* Polling interval in clock ticks (3 seconds = 3 * 60 on PAL) */
#define POLL_INTERVAL (3 * CLK_TCK)

/* Colors for sent message states */
#define COL_PENDING   7   /* yellow - sent to radio */
#define COL_CONFIRMED 5   /* green - delivery confirmed */

static char host_buf[CONFIG_MAX_HOST];
static char companion_name[20];
static uint8_t current_ch;
static uint8_t current_contact;
static uint8_t dm_mode;   /* 0 = channel mode, 1 = DM mode */

/* ---- @-mention cycling ---- */

#define MAX_MENTIONS 4

typedef struct {
    char names[MAX_MENTIONS][CONTACT_NAME_LEN + 1];
    uint8_t count;
} mention_list_t;

static mention_list_t ch_mentions[MAX_CHANNELS];
static uint8_t mention_sel;  /* 0 = no mention, 1..count */

/* Add a sender name to a channel's mention list */
static void mention_add(uint8_t ch, const char *name) {
    mention_list_t *ml;
    uint8_t i;
    if (!name || !*name || ch >= MAX_CHANNELS) return;
    if (strcmp(name, companion_name) == 0) return;
    if (strcmp(name, "Me") == 0) return;
    ml = &ch_mentions[ch];
    for (i = 0; i < ml->count; ++i) {
        if (strcmp(ml->names[i], name) == 0) return;
    }
    if (ml->count < MAX_MENTIONS) {
        strncpy(ml->names[ml->count], name, CONTACT_NAME_LEN);
        ml->names[ml->count][CONTACT_NAME_LEN] = '\0';
        ++ml->count;
    }
}

static void mention_cycle(int8_t dir) {
    mention_list_t *ml = &ch_mentions[current_ch];
    uint8_t total = ml->count + 1;  /* +1 for "no mention" */
    mention_sel = (mention_sel + total + dir) % total;
    if (mention_sel == 0) {
        input_set_mention(0);
    } else {
        input_set_mention(ml->names[mention_sel - 1]);
    }
}

/* ---- Pending message tracking ---- */

#define MAX_PENDING 4

typedef struct {
    uint8_t active;
    uint8_t ch;
    uint8_t ack_code[4];
    uint8_t is_dm;
} pending_msg_t;

static pending_msg_t pending[MAX_PENDING];
static uint8_t pending_idx;

static void pending_add(uint8_t ch,
                        uint8_t is_dm, const uint8_t *ack_code) {
    pending_msg_t *p = &pending[pending_idx];
    p->active = 1;
    p->ch = ch;
    p->is_dm = is_dm;
    if (ack_code) {
        memcpy(p->ack_code, ack_code, 4);
    } else {
        memset(p->ack_code, 0, 4);
    }
    pending_idx = (pending_idx + 1) % MAX_PENDING;
}

static void pending_confirm_ack(const uint8_t *ack_code) {
    uint8_t i, idx, match_idx;
    uint8_t skip;

    /* Find the pending entry matching this ack_code */
    match_idx = 0xFF;
    for (i = 0; i < MAX_PENDING; ++i) {
        if (pending[i].active && pending[i].is_dm &&
            memcmp(pending[i].ack_code, ack_code, 4) == 0) {
            match_idx = i;
            break;
        }
    }
    if (match_idx == 0xFF) return;

    /* Count how many older active DM entries for the same channel
       appear before this one in the ring buffer (oldest-first order) */
    skip = 0;
    for (i = 0; i < MAX_PENDING; ++i) {
        idx = (pending_idx + i) % MAX_PENDING;
        if (idx == match_idx) break;  /* reached our entry */
        if (pending[idx].active && pending[idx].is_dm &&
            pending[idx].ch == pending[match_idx].ch) {
            ++skip;
        }
    }

    screen_confirm_line(pending[match_idx].ch, skip,
                        COL_PENDING, COL_CONFIRMED);
    pending[match_idx].active = 0;
}

/* ---- Setup screen ---- */

/* Draw the setup screen UI.
   Returns: 0 = RETURN (confirmed), or INPUT_NEXT_CH/INPUT_PREV_CH/
   INPUT_NEXT_CONTACT/INPUT_PREV_CONTACT if user pressed a nav key. */
static uint8_t show_setup_screen(char *buf, uint8_t allow_cancel) {
    uint8_t pos;
    uint8_t *sp;
    uint8_t *cp;
    uint8_t i;
    char c;

    clrscr();

    /* Title bar (row 0, reverse video dark blue) */
    sp = (uint8_t *)0x0400;
    cp = (uint8_t *)0xD800;
    for (i = 0; i < 40; ++i) {
        sp[i] = 0xA0;  /* reverse space */
        cp[i] = 6;     /* dark blue */
    }
    /* "MeshCore64" at col 1 */
    {
        static const char title[] = "MeshCore64";
        const char *t = title;
        i = 1;
        while (*t) {
            sp[i] = petscii_to_screen((uint8_t)*t) | 0x80;
            ++t;
            ++i;
        }
    }

    /* "Companion address:" at row 2 */
    gotoxy(0, 3);
    textcolor(15);  /* light gray */
    cputs("Companion address:");

    /* Hint text */
    gotoxy(0, 6);
    textcolor(7);  /* yellow */
    if (allow_cancel) {
        cputs("RETURN to reconnect");
        gotoxy(0, 8);
        textcolor(15);
        cputs("F1/F3 channels  F5/F7 contacts");
    } else {
        cputs("RETURN to connect");
    }

    pos = (uint8_t)strlen(buf);

    while (1) {
        /* Draw input line at row 4 */
        sp = (uint8_t *)0x0400 + 4 * 40;
        cp = (uint8_t *)0xD800 + 4 * 40;
        memset(sp, 0x20, 40);
        memset(cp, 1, 40);  /* white */

        /* "> " prompt */
        sp[0] = petscii_to_screen('>');
        cp[0] = 7;  /* yellow */
        sp[1] = 0x20;

        /* Buffer text */
        for (i = 0; i < pos && i < 37; ++i) {
            sp[2 + i] = petscii_to_screen((uint8_t)buf[i]);
        }

        /* Cursor */
        if (2 + pos < 40) {
            sp[2 + pos] |= 0x80;
        }

        c = cgetc();

        if (c == 0x0D) {  /* RETURN */
            buf[pos] = '\0';
            return 0;
        }

        if (c == 0x14 && pos > 0) {  /* DEL */
            --pos;
            buf[pos] = '\0';
            continue;
        }

        /* Nav keys — cancel if allowed */
        if (allow_cancel) {
            if (c == (char)0x85) return INPUT_NEXT_CH;
            if (c == (char)0x86) return INPUT_PREV_CH;
            if (c == (char)0x87) return INPUT_NEXT_CONTACT;
            if (c == (char)0x88) return INPUT_PREV_CONTACT;
        }

        /* Printable characters (digits, letters, punct for IP:port) */
        if (pos < CONFIG_MAX_HOST - 1 &&
            c >= 0x20 && c <= 0x7A) {
            buf[pos] = c;
            ++pos;
            buf[pos] = '\0';
        }
    }
}

/* ---- Helpers ---- */

static uint8_t find_channel(const char *name) {
    uint8_t i;
    for (i = 0; i < mesh_num_channels; ++i) {
        if (mesh_channels[i].valid &&
            strcmp(mesh_channels[i].name, name) == 0) {
            return i;
        }
    }
    return 0;
}

static const char *current_channel_name(void) {
    if (current_ch < mesh_num_channels && mesh_channels[current_ch].valid) {
        return mesh_channels[current_ch].name;
    }
    return "---";
}

static void next_channel(void) {
    uint8_t i;
    for (i = 1; i <= mesh_num_channels; ++i) {
        uint8_t idx = (current_ch + i) % mesh_num_channels;
        if (mesh_channels[idx].valid) {
            current_ch = idx;
            return;
        }
    }
}

static void prev_channel(void) {
    uint8_t i;
    for (i = 1; i <= mesh_num_channels; ++i) {
        uint8_t idx = (current_ch + mesh_num_channels - i) % mesh_num_channels;
        if (mesh_channels[idx].valid) {
            current_ch = idx;
            return;
        }
    }
}

/* Map contact index to its DM screen buffer index.
   Contacts beyond MAX_DM_BUFFERS share the last buffer. */
static uint8_t dm_buf_for(uint8_t contact_idx) {
    if (contact_idx < MAX_DM_BUFFERS) {
        return DM_BUFFER_BASE + contact_idx;
    }
    return DM_BUFFER_BASE + MAX_DM_BUFFERS - 1;
}

/* Find contact index by sender name. Returns 0xFF if not found. */
static uint8_t find_contact_by_name(const char *name) {
    uint8_t i;
    for (i = 0; i < mesh_num_contacts; ++i) {
        if (strcmp(mesh_contacts[i].name, name) == 0) {
            return i;
        }
    }
    return 0xFF;
}

static void update_status(void) {
    if (dm_mode && mesh_num_contacts > 0) {
        screen_status(companion_name,
                      mesh_contacts[current_contact].name, 1);
    } else {
        screen_status(companion_name, current_channel_name(), 0);
    }
}

static void drain_messages(void) {
    mesh_msg_t msg;
    uint8_t got;

    do {
        got = mesh_poll(&msg);
        if (got) {
            if (msg.is_channel) {
                /* Track sender for @-mention cycling.
                   Channel msgs have sender embedded as "name: text". */
                if (msg.channel_idx < MAX_CHANNELS) {
                    const char *colon = strchr(msg.text, ':');
                    if (colon && (colon - msg.text) < CONTACT_NAME_LEN) {
                        char sname[CONTACT_NAME_LEN + 1];
                        uint8_t slen = (uint8_t)(colon - msg.text);
                        memcpy(sname, msg.text, slen);
                        sname[slen] = '\0';
                        mention_add(msg.channel_idx, sname);
                    }
                }
                screen_add_message_ch(msg.channel_idx, msg.sender, msg.text,
                                      14, 15);
            } else {
                uint8_t ci = find_contact_by_name(msg.sender);
                screen_add_message_ch(dm_buf_for(ci != 0xFF ? ci : 0),
                                      msg.sender, msg.text, 14, 15);
            }
        }
    } while (got);
}

/* ---- Connect sequence ---- */

static uint8_t do_connect(void) {
    screen_init();
    screen_init_channel_bufs();
    screen_status("MeshCore64", "Connecting...", 0);
    screen_system_msg("Initializing serial...");

    if (!serial_init()) {
        screen_system_msg("SERIAL INIT FAILED!");
        screen_system_msg("SwiftLink not found at $DE00");
        return 0;
    }

    screen_system_msg("Dialing companion...");
    screen_system_msg(host_buf);

    if (!mesh_connect(host_buf, companion_name)) {
        screen_system_msg("CONNECTION FAILED!");
        screen_system_msg("Check host & tcpser.");
        return 0;
    }

    screen_system_msg("Connected! Querying device...");
    mesh_query_device();
    mesh_get_channels();

    screen_system_msg("Fetching contacts...");
    mesh_get_contacts();

    screen_set_my_name(companion_name);

    current_ch = find_channel("#c64");
    current_contact = 0;
    dm_mode = 0;
    pending_idx = 0;
    memset(pending, 0, sizeof(pending));
    memset(ch_mentions, 0, sizeof(ch_mentions));
    mention_sel = 0;

    screen_init_channel_bufs();
    screen_set_channel(current_ch);
    update_status();

    input_init();
    drain_messages();

    return 1;
}

/* ---- Main ---- */

int main(void) {
    clock_t last_poll;
    uint8_t action;
    uint8_t incoming;
    const char *ibuf;
    uint8_t ack_code[4];
    uint8_t setup_result;

    screen_init();

    /* Load config or show first-launch setup */
    if (!config_load(host_buf)) {
        strcpy(host_buf, DEFAULT_HOST);
        show_setup_screen(host_buf, 0);  /* first launch, no cancel */
        config_save(host_buf);
    }

    /* Connect loop — retries on failure */
    while (!do_connect()) {
        cgetc();
        show_setup_screen(host_buf, 0);
        config_save(host_buf);
    }

    last_poll = clock();

    /* ---- Main loop ---- */
    while (1) {
        /* Check for push notifications */
        incoming = mesh_process_incoming();
        if (incoming & 0x01) {
            drain_messages();
        }
        /* Process any buffered acks (from mesh_process_incoming or cmd_wait) */
        while (mesh_next_ack(ack_code)) {
            pending_confirm_ack(ack_code);
        }

        /* Handle keyboard */
        action = input_poll();

        switch (action) {
            case INPUT_SUBMIT:
                ibuf = input_buf();
                if (*ibuf) {
                    if (dm_mode && mesh_num_contacts > 0) {
                        mesh_send_dm(current_contact, ibuf, ack_code);
                        screen_add_message_ch(dm_buf_for(current_contact),
                                              "Me", ibuf, COL_PENDING, 1);
                        pending_add(dm_buf_for(current_contact), 1, ack_code);
                    } else {
                        /* Prepend @[name] if mention is active */
                        if (input_mention()) {
                            char send_buf[MSG_TEXT_LEN + 1];
                            strcpy(send_buf, "@[");
                            strncat(send_buf, input_mention(),
                                    MSG_TEXT_LEN - 4);
                            strcat(send_buf, "] ");
                            strncat(send_buf, ibuf,
                                    MSG_TEXT_LEN - strlen(send_buf));
                            send_buf[MSG_TEXT_LEN] = '\0';
                            mesh_send(current_ch, send_buf, ack_code);
                            screen_add_message_ch(current_ch, "Me", send_buf,
                                                  COL_PENDING, 1);
                        } else {
                            mesh_send(current_ch, ibuf, ack_code);
                            screen_add_message_ch(current_ch, "Me", ibuf,
                                                  COL_PENDING, 1);
                        }
                    }
                }
                input_clear();
                break;

            case INPUT_UP:
                if (!dm_mode) mention_cycle(-1);
                break;

            case INPUT_DOWN:
                if (!dm_mode) mention_cycle(1);
                break;

            case INPUT_NEXT_CH:
                next_channel();
                dm_mode = 0;
                mention_sel = 0;
                input_set_mention(0);
                screen_set_channel(current_ch);
                update_status();
                break;

            case INPUT_PREV_CH:
                prev_channel();
                dm_mode = 0;
                mention_sel = 0;
                input_set_mention(0);
                screen_set_channel(current_ch);
                update_status();
                break;

            case INPUT_NEXT_CONTACT:
                if (mesh_num_contacts > 0) {
                    current_contact = (current_contact + 1) % mesh_num_contacts;
                    dm_mode = 1;
                    mention_sel = 0;
                    input_set_mention(0);
                    screen_set_channel(dm_buf_for(current_contact));
                    update_status();
                }
                break;

            case INPUT_PREV_CONTACT:
                if (mesh_num_contacts > 0) {
                    current_contact = (current_contact + mesh_num_contacts - 1)
                                      % mesh_num_contacts;
                    dm_mode = 1;
                    screen_set_channel(dm_buf_for(current_contact));
                    update_status();
                }
                break;

            case INPUT_SETTINGS:
                setup_result = show_setup_screen(host_buf, 1);
                if (setup_result == 0) {
                    /* User confirmed — save, disconnect, reconnect */
                    config_save(host_buf);
                    serial_hangup();
                    while (!do_connect()) {
                        cgetc();
                        show_setup_screen(host_buf, 0);
                        config_save(host_buf);
                    }
                } else {
                    /* User pressed nav key — restore screen UI.
                       Channel buffers are in RAM and untouched by clrscr,
                       so just redraw the chrome and refresh the display. */
                    screen_init();
                    if (setup_result == INPUT_NEXT_CH) {
                        next_channel();
                        dm_mode = 0;
                    } else if (setup_result == INPUT_PREV_CH) {
                        prev_channel();
                        dm_mode = 0;
                    } else if (setup_result == INPUT_NEXT_CONTACT &&
                               mesh_num_contacts > 0) {
                        current_contact = (current_contact + 1) % mesh_num_contacts;
                        dm_mode = 1;
                    } else if (setup_result == INPUT_PREV_CONTACT &&
                               mesh_num_contacts > 0) {
                        current_contact = (current_contact + mesh_num_contacts - 1)
                                          % mesh_num_contacts;
                        dm_mode = 1;
                    }
                    if (dm_mode) {
                        screen_set_channel(dm_buf_for(current_contact));
                    } else {
                        screen_set_channel(current_ch);
                    }
                    update_status();
                    input_init();
                }
                last_poll = clock();
                break;
        }

        /* Periodic poll */
        if (clock() - last_poll >= POLL_INTERVAL) {
            last_poll = clock();
            drain_messages();
        }

        /* Detect connection loss and auto-reconnect */
        if (mesh_fail_count >= MESH_FAIL_THRESHOLD) {
            screen_system_msg("Connection lost. Reconnecting...");
            if (mesh_reconnect(host_buf, companion_name)) {
                screen_system_msg("Reconnected!");
                update_status();
                drain_messages();
            } else {
                screen_system_msg("Reconnect failed. Retrying...");
            }
            last_poll = clock();
        }
    }

    return 0;
}
