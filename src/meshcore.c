#include <string.h>
#include <time.h>
#include "meshcore.h"
#include "serial.h"
#include "charset.h"

/* Protocol version we advertise */
#define APP_VERSION  0x03

/* Silence limit for response reads (~40ms at 1MHz).
   With NMI-driven receive, bytes arrive reliably in the ring buffer.
   We only need enough silence to detect inter-frame gaps.
   At 9600 baud, bytes arrive every ~1ms; 2000 iters ≈ 40ms gap. */
#define RX_SILENCE   2000u

/* Global state */
channel_t  mesh_channels[MAX_CHANNELS];
uint8_t    mesh_num_channels;
contact_t  mesh_contacts[MAX_CONTACTS];
uint8_t    mesh_num_contacts;
uint8_t    mesh_fail_count;

/* Ack ring buffer — holds multiple PUSH_ACK codes between polls */
#define MAX_ACK_BUF 8
static uint8_t ack_buf[MAX_ACK_BUF][4];
static uint8_t ack_head;
static uint8_t ack_tail;

/* Time offset: real_unix_time ≈ time(NULL) + time_offset.
   Calibrated from the first received message timestamp. */
static uint32_t time_offset;
static uint8_t  time_synced;

/* Extract LE32 timestamp from buffer */
static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Sync clock from a received message timestamp */
static void sync_time(const uint8_t *ts_bytes) {
    uint32_t msg_ts = read_le32(ts_bytes);
    if (msg_ts > 1700000000UL) {  /* sanity: after ~Nov 2023 */
        time_offset = msg_ts - (uint32_t)time(NULL);
        time_synced = 1;
    }
}

/* Get current Unix timestamp (calibrated if synced) */
static uint32_t mesh_now(void) {
    return (uint32_t)time(NULL) + time_offset;
}

/* RX accumulation buffer */
static uint8_t  rx_buf[320];
static uint16_t rx_len;

/* Temp buffer for building outbound payloads */
static uint8_t  tx_buf[180];

/* Buffer for extracted frame payload */
static uint8_t  frame_buf[MAX_FRAME_SIZE];

/* ---- Low-level frame I/O ---- */

/* Send a frame: 0x3C + LE16 length + payload */
static void send_frame(const uint8_t *payload, uint16_t len) {
    uint8_t hdr[3];
    hdr[0] = FRAME_TO_DEVICE;
    hdr[1] = (uint8_t)(len & 0xFF);
    hdr[2] = (uint8_t)(len >> 8);
    serial_send(hdr, 3);
    serial_send(payload, len);
}

/* Pull bytes from ACIA into rx_buf using tight polling.
   This is the ONLY place we read from serial during protocol ops. */
static void rx_fill(uint16_t silence_limit) {
    uint16_t space = (uint16_t)sizeof(rx_buf) - rx_len;
    if (space > 0) {
        rx_len += serial_recv(rx_buf + rx_len, space, silence_limit);
    }
}

/* Try to extract one complete frame from rx_buf.
   Frame format: 0x3E + LE16(len) + payload[len]
   Returns payload length (copied to frame_buf), or 0 if incomplete.
   Note: 0x3E can appear as data within frames, so we rely on
   sequential parsing, not scanning for 0x3E markers. */
static uint16_t extract_frame(void) {
    uint16_t frame_len, total;

    /* Need at least header (3 bytes) */
    if (rx_len < 3) return 0;

    /* First byte should be frame marker */
    if (rx_buf[0] != FRAME_FROM_DEVICE) {
        /* Discard until we find a marker */
        uint16_t i;
        for (i = 1; i < rx_len; ++i) {
            if (rx_buf[i] == FRAME_FROM_DEVICE) break;
        }
        if (i >= rx_len) {
            rx_len = 0;  /* No marker found, discard all */
            return 0;
        }
        rx_len -= i;
        memmove(rx_buf, rx_buf + i, rx_len);
        if (rx_len < 3) return 0;
    }

    /* Parse LE16 length */
    frame_len = (uint16_t)rx_buf[1] | ((uint16_t)rx_buf[2] << 8);

    /* Sanity check */
    if (frame_len > MAX_FRAME_SIZE) {
        /* Invalid length — skip this marker and try next */
        rx_len -= 1;
        memmove(rx_buf, rx_buf + 1, rx_len);
        return 0;
    }

    total = 3 + frame_len;
    if (rx_len < total) return 0;  /* Incomplete frame */

    /* Copy payload to frame_buf */
    memcpy(frame_buf, rx_buf + 3, frame_len);

    /* Remove consumed bytes */
    rx_len -= total;
    if (rx_len > 0) {
        memmove(rx_buf, rx_buf + total, rx_len);
    }

    return frame_len;
}

/* Send command payload and wait for one response frame.
   Skips push notifications (packet type >= 0x80).
   Returns payload length or 0 on timeout. */
static uint16_t cmd_wait(const uint8_t *cmd, uint16_t cmd_len) {
    uint16_t plen;
    uint8_t attempts;
    uint8_t pushes = 0;

    send_frame(cmd, cmd_len);

    /* Try up to 10 rounds of filling + extracting.
       Each rx_fill with RX_SILENCE waits ~40ms if no data.
       Skip push notifications (0x80+) — they're async.
       Give up after 5 consecutive push notifications (response lost). */
    for (attempts = 0; attempts < 10; ++attempts) {
        rx_fill(RX_SILENCE);
        plen = extract_frame();
        if (plen > 0) {
            if (frame_buf[0] >= 0x80) {
                mesh_fail_count = 0;  /* Got data from device */
                /* Buffer any PUSH_ACKs so they aren't lost */
                if (frame_buf[0] == PUSH_ACK && plen >= 5) {
                    uint8_t next = (ack_head + 1) % MAX_ACK_BUF;
                    if (next != ack_tail) {
                        memcpy(ack_buf[ack_head], frame_buf + 1, 4);
                        ack_head = next;
                    }
                }
                if (++pushes >= 5) break;  /* Too many pushes, give up */
                continue;
            }
            mesh_fail_count = 0;  /* Got valid response */
            return plen;
        }
        if (rx_len == 0 && attempts > 1) break;  /* Nothing coming */
    }

    ++mesh_fail_count;
    return 0;
}

/* ---- Contact lookup ---- */

static const char *find_contact_name(const uint8_t *prefix6) {
    uint8_t i;
    for (i = 0; i < mesh_num_contacts; ++i) {
        if (memcmp(mesh_contacts[i].prefix, prefix6, 6) == 0) {
            return mesh_contacts[i].name;
        }
    }
    return "???";
}

/* Copy ASCII bytes from frame to PETSCII string, skipping unprintable chars */
static void copy_ascii_str(char *dst, const uint8_t *src,
                            uint8_t maxlen) {
    uint8_t si = 0, di = 0;
    while (si < maxlen && src[si] != '\0') {
        uint8_t p = ascii_to_petscii(src[si]);
        if (p != 0) {
            dst[di++] = (char)p;
        }
        ++si;
    }
    dst[di] = '\0';
}

/* ---- Public API ---- */

uint8_t mesh_connect(const char *ip_port, char *companion_name) {
    uint16_t plen;

    rx_len = 0;
    mesh_num_channels = 0;
    mesh_num_contacts = 0;
    mesh_fail_count = 0;
    time_offset = 0;
    time_synced = 0;

    /* Dial modem — includes wait + drain */
    if (!serial_dial(ip_port)) {
        return 0;
    }

    /* Switch to NMI-driven receive — bytes now buffered by interrupt,
       no loss during CPU-intensive processing (memcpy, cprintf, etc.) */
    serial_start_nmi();

    /* APP_START handshake (must be first command).
       Payload: 0x01 0x03 0x20 0x20 0x20 0x20 0x20 0x20 "mccli"
       Matches meshcore_py exactly: bytearray(b"\x01\x03      mccli") */
    tx_buf[0] = CMD_APP_START;
    tx_buf[1] = APP_VERSION;
    memset(tx_buf + 2, 0x20, 6);   /* 6 spaces */
    memcpy(tx_buf + 8, "mccli", 5);

    plen = cmd_wait(tx_buf, 13);

    if (plen == 0 || frame_buf[0] != PKT_SELF_INFO) {
        strcpy(companion_name, "?");
        return 0;
    }

    /* SELF_INFO payload layout (verified against test vectors):
       [0]     packet_type (0x05)
       [1]     adv_type
       [2]     tx_power
       [3]     max_tx_power
       [4-35]  public_key (32 bytes)
       [36-39] adv_lat (LE32)
       [40-43] adv_lon (LE32)
       [44]    multi_acks
       [45]    adv_loc_policy
       [46]    telemetry_mode
       [47]    manual_add_contacts
       [48-51] radio_freq (LE32)
       [52-55] radio_bw (LE32)
       [56]    radio_sf
       [57]    radio_cr
       [58+]   name (ASCII, variable length) */
    if (plen > 58) {
        copy_ascii_str(companion_name, frame_buf + 58,
                       (uint8_t)(plen - 58 < 20 ? plen - 58 : 19));
    } else {
        strcpy(companion_name, "meshcore");
    }

    return 1;
}

uint8_t mesh_query_device(void) {
    uint16_t plen;

    /* DEVICE_QUERY: 0x16 0x03 */
    tx_buf[0] = CMD_DEVICE_QUERY;
    tx_buf[1] = APP_VERSION;
    plen = cmd_wait(tx_buf, 2);

    if (plen >= 4 && frame_buf[0] == PKT_DEVICE_INFO) {
        /* [0] pkt_type (0x0D)
           [1] fw_ver
           [2] max_contacts / 2
           [3] max_channels */
        mesh_num_channels = frame_buf[3];
        if (mesh_num_channels > MAX_CHANNELS)
            mesh_num_channels = MAX_CHANNELS;
    }

    return mesh_num_channels;
}

uint8_t mesh_get_channels(void) {
    uint8_t i;
    uint16_t plen;

    for (i = 0; i < mesh_num_channels; ++i) {
        tx_buf[0] = CMD_GET_CHANNEL;
        tx_buf[1] = i;
        plen = cmd_wait(tx_buf, 2);

        mesh_channels[i].valid = 0;

        if (plen >= 34 && frame_buf[0] == PKT_CHANNEL_INFO) {
            /* [0]     pkt_type (0x12)
               [1]     channel_idx
               [2-33]  name (32 bytes, null-terminated)
               [34-49] secret (16 bytes) */
            copy_ascii_str(mesh_channels[i].name,
                          frame_buf + 2, CHANNEL_NAME_LEN);
            mesh_channels[i].valid = (mesh_channels[i].name[0] != '\0');
        }
    }

    return mesh_num_channels;
}

uint8_t mesh_get_contacts(void) {
    uint16_t plen;

    mesh_num_contacts = 0;

    /* Send GET_CONTACTS (no lastmod parameter) */
    tx_buf[0] = CMD_GET_CONTACTS;
    send_frame(tx_buf, 1);

    /* Read responses until CONTACTS_END or timeout.
       Each contact is ~147 bytes, so we may need many rounds.
       Skip push notifications that arrive during the stream. */
    {
        uint8_t rounds;
        for (rounds = 0; rounds < 50 && mesh_num_contacts < MAX_CONTACTS; ++rounds) {
            rx_fill(RX_SILENCE);
            plen = extract_frame();
            if (plen == 0) {
                if (rx_len == 0 && rounds > 2) break;
                continue;
            }

            /* Skip push notifications */
            if (frame_buf[0] >= 0x80) {
                continue;
            }

            if (frame_buf[0] == PKT_CONTACTS_START) {
                continue;
            }

            if (frame_buf[0] == PKT_CONTACTS_END) {
                break;
            }

            if (frame_buf[0] == PKT_CONTACT && plen >= 132) {
                /* CONTACT payload:
                   [0]       pkt_type (0x03)
                   [1-32]    public_key (32 bytes)
                   [33]      type
                   [34]      flags
                   [35]      path_len_packed
                   [36-99]   out_path (64 bytes)
                   [100-131] name (32 bytes, null-terminated)
                   [132+]    timestamps etc. */
                memcpy(mesh_contacts[mesh_num_contacts].prefix,
                       frame_buf + 1, 6);
                copy_ascii_str(mesh_contacts[mesh_num_contacts].name,
                              frame_buf + 100, CONTACT_NAME_LEN);
                ++mesh_num_contacts;
            }
        }
    }

    return mesh_num_contacts;
}

uint8_t mesh_poll(mesh_msg_t *msg) {
    uint16_t plen;
    const char *sender_name;

    tx_buf[0] = CMD_SYNC_NEXT_MSG;
    plen = cmd_wait(tx_buf, 1);
    if (plen == 0) return 0;

    /* Channel message (0x08):
       [0]    pkt_type
       [1]    channel_idx
       [2]    path_len
       [3]    txt_type
       [4-7]  timestamp
       [8+]   text */
    if (frame_buf[0] == PKT_CHANNEL_MSG && plen > 8) {
        if (!time_synced) sync_time(frame_buf + 4);
        msg->is_channel = 1;
        msg->channel_idx = frame_buf[1];
        msg->sender[0] = '\0';
        copy_ascii_str(msg->text, frame_buf + 8,
                      (uint8_t)(plen - 8 < MSG_TEXT_LEN ? plen - 8 : MSG_TEXT_LEN));
        return 1;
    }

    /* Channel message V3 (0x11):
       [0]    pkt_type
       [1]    snr
       [2-3]  reserved
       [4]    channel_idx
       [5]    path_len
       [6]    txt_type
       [7-10] timestamp
       [11+]  text */
    if (frame_buf[0] == PKT_CHANNEL_MSG_V3 && plen > 11) {
        if (!time_synced) sync_time(frame_buf + 7);
        msg->is_channel = 1;
        msg->channel_idx = frame_buf[4];
        msg->sender[0] = '\0';
        copy_ascii_str(msg->text, frame_buf + 11,
                      (uint8_t)(plen - 11 < MSG_TEXT_LEN ? plen - 11 : MSG_TEXT_LEN));
        return 1;
    }

    /* DM (0x07):
       [0]    pkt_type
       [1-6]  pubkey_prefix (6 bytes)
       [7]    path_len
       [8]    txt_type
       [9-12] timestamp
       [13+]  text (or [13-16] signature + [17+] text if txt_type==2) */
    if (frame_buf[0] == PKT_CONTACT_MSG && plen > 13) {
        uint16_t toff = 13;
        if (!time_synced) sync_time(frame_buf + 9);
        msg->is_channel = 0;
        msg->channel_idx = 0xFF;
        sender_name = find_contact_name(frame_buf + 1);
        strncpy(msg->sender, sender_name, CONTACT_NAME_LEN);
        msg->sender[CONTACT_NAME_LEN] = '\0';
        if (frame_buf[8] == 2 && plen > 17) toff = 17; /* signed msg */
        copy_ascii_str(msg->text, frame_buf + toff,
                      (uint8_t)(plen - toff < MSG_TEXT_LEN ? plen - toff : MSG_TEXT_LEN));
        return 1;
    }

    /* DM V3 (0x10):
       [0]    pkt_type
       [1]    snr
       [2-3]  reserved
       [4-9]  pubkey_prefix (6 bytes)
       [10]   path_len
       [11]   txt_type
       [12-15] timestamp
       [16+]  text (or [16-19] signature + [20+] text if txt_type==2) */
    if (frame_buf[0] == PKT_CONTACT_MSG_V3 && plen > 16) {
        uint16_t toff = 16;
        if (!time_synced) sync_time(frame_buf + 12);
        msg->is_channel = 0;
        msg->channel_idx = 0xFF;
        sender_name = find_contact_name(frame_buf + 4);
        strncpy(msg->sender, sender_name, CONTACT_NAME_LEN);
        msg->sender[CONTACT_NAME_LEN] = '\0';
        if (frame_buf[11] == 2 && plen > 20) toff = 20; /* signed msg */
        copy_ascii_str(msg->text, frame_buf + toff,
                      (uint8_t)(plen - toff < MSG_TEXT_LEN ? plen - toff : MSG_TEXT_LEN));
        return 1;
    }

    /* No more messages or unrecognized packet */
    return 0;
}

/* Extract expected_ack from MSG_SENT response in frame_buf.
   MSG_SENT: [0]=0x06 [1]=msg_type [2-5]=expected_ack [6-9]=timeout */
static void extract_ack(uint8_t *ack_out, uint16_t plen) {
    if (ack_out && plen >= 6 && frame_buf[0] == PKT_MSG_SENT) {
        memcpy(ack_out, frame_buf + 2, 4);
    }
}

uint8_t mesh_send(uint8_t channel_idx, const char *text, uint8_t *ack_out) {
    uint16_t plen;
    uint16_t text_len;
    uint32_t ts;
    uint16_t i;

    /* SEND_CHANNEL_TXT_MSG:
       [0]    cmd (0x03)
       [1]    txt_type (0x00 = plain)
       [2]    channel_idx
       [3-6]  timestamp (LE32)
       [7+]   text (ASCII) */
    tx_buf[0] = CMD_SEND_CHAN_TXT_MSG;
    tx_buf[1] = 0x00;
    tx_buf[2] = channel_idx;

    ts = mesh_now();
    tx_buf[3] = (uint8_t)(ts);
    tx_buf[4] = (uint8_t)(ts >> 8);
    tx_buf[5] = (uint8_t)(ts >> 16);
    tx_buf[6] = (uint8_t)(ts >> 24);

    text_len = (uint16_t)strlen(text);
    if (text_len > MSG_TEXT_LEN) text_len = MSG_TEXT_LEN;

    for (i = 0; i < text_len; ++i) {
        tx_buf[7 + i] = petscii_to_ascii((uint8_t)text[i]);
    }

    plen = cmd_wait(tx_buf, 7 + text_len);

    extract_ack(ack_out, plen);
    return (plen > 0 && (frame_buf[0] == PKT_OK ||
                         frame_buf[0] == PKT_MSG_SENT));
}

uint8_t mesh_send_dm(uint8_t contact_idx, const char *text, uint8_t *ack_out) {
    uint16_t plen;
    uint16_t text_len;
    uint32_t ts;
    uint16_t i;

    if (contact_idx >= mesh_num_contacts) return 0;

    /* SEND_TXT_MSG:
       [0]    cmd (0x02)
       [1]    subtype (0x00 = regular message)
       [2]    attempt (0x00)
       [3-6]  timestamp (LE32)
       [7-12] pubkey_prefix (6 bytes)
       [13+]  text (ASCII) */
    tx_buf[0] = CMD_SEND_TXT_MSG;
    tx_buf[1] = 0x00;
    tx_buf[2] = 0x00;

    ts = mesh_now();
    tx_buf[3] = (uint8_t)(ts);
    tx_buf[4] = (uint8_t)(ts >> 8);
    tx_buf[5] = (uint8_t)(ts >> 16);
    tx_buf[6] = (uint8_t)(ts >> 24);

    memcpy(tx_buf + 7, mesh_contacts[contact_idx].prefix, 6);

    text_len = (uint16_t)strlen(text);
    if (text_len > MSG_TEXT_LEN) text_len = MSG_TEXT_LEN;

    for (i = 0; i < text_len; ++i) {
        tx_buf[13 + i] = petscii_to_ascii((uint8_t)text[i]);
    }

    plen = cmd_wait(tx_buf, 13 + text_len);

    extract_ack(ack_out, plen);
    return (plen > 0 && (frame_buf[0] == PKT_OK ||
                         frame_buf[0] == PKT_MSG_SENT));
}

uint8_t mesh_process_incoming(void) {
    uint16_t plen;
    uint8_t result = 0;

    rx_fill(1000u);  /* Short poll — ~15ms of silence */

    while (1) {
        plen = extract_frame();
        if (plen == 0) break;

        mesh_fail_count = 0;  /* Got data from device */
        if (frame_buf[0] == PUSH_MSG_WAITING) {
            result |= 0x01;
        }
        /* PUSH_ACK: [0]=0x82 [1-4]=ack_code — buffer for later retrieval */
        if (frame_buf[0] == PUSH_ACK && plen >= 5) {
            uint8_t next = (ack_head + 1) % MAX_ACK_BUF;
            if (next != ack_tail) {  /* not full */
                memcpy(ack_buf[ack_head], frame_buf + 1, 4);
                ack_head = next;
            }
            result |= 0x02;
        }
    }

    return result;
}

uint8_t mesh_next_ack(uint8_t *ack_out) {
    if (ack_tail == ack_head) return 0;  /* empty */
    memcpy(ack_out, ack_buf[ack_tail], 4);
    ack_tail = (ack_tail + 1) % MAX_ACK_BUF;
    return 1;
}

uint8_t mesh_reconnect(const char *ip_port, char *companion_name) {
    uint16_t plen;

    /* Reset state */
    rx_len = 0;
    mesh_fail_count = 0;

    /* Hangup existing connection */
    serial_hangup();

    /* Re-dial */
    if (!serial_dial(ip_port)) {
        return 0;
    }

    serial_start_nmi();

    /* Re-send APP_START handshake */
    tx_buf[0] = CMD_APP_START;
    tx_buf[1] = APP_VERSION;
    memset(tx_buf + 2, 0x20, 6);
    memcpy(tx_buf + 8, "mccli", 5);

    plen = cmd_wait(tx_buf, 13);

    if (plen == 0 || frame_buf[0] != PKT_SELF_INFO) {
        return 0;
    }

    if (plen > 58) {
        copy_ascii_str(companion_name, frame_buf + 58,
                       (uint8_t)(plen - 58 < 20 ? plen - 58 : 19));
    }

    mesh_fail_count = 0;
    return 1;
}
