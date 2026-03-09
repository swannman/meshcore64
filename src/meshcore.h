#ifndef MESHCORE_H
#define MESHCORE_H

#include <stdint.h>

/* Frame markers (verified against meshcore_py tcp_cx.py) */
#define FRAME_TO_DEVICE   0x3C  /* TX: C64 -> companion */
#define FRAME_FROM_DEVICE 0x3E  /* RX: companion -> C64 */

/* Max frame payload */
#define MAX_FRAME_SIZE  300

/* Command codes (sent by us) */
#define CMD_APP_START           0x01
#define CMD_SEND_TXT_MSG        0x02
#define CMD_SEND_CHAN_TXT_MSG   0x03
#define CMD_GET_CONTACTS        0x04
#define CMD_SYNC_NEXT_MSG       0x0A
#define CMD_DEVICE_QUERY        0x16
#define CMD_GET_CHANNEL         0x1F

/* Packet types (received from device) */
#define PKT_OK                 0x00
#define PKT_ERR                0x01
#define PKT_CONTACTS_START     0x02
#define PKT_CONTACT            0x03
#define PKT_CONTACTS_END       0x04
#define PKT_SELF_INFO          0x05
#define PKT_MSG_SENT           0x06
#define PKT_CONTACT_MSG        0x07
#define PKT_CHANNEL_MSG        0x08
#define PKT_NO_MORE_MSGS       0x0A
#define PKT_DEVICE_INFO        0x0D
#define PKT_CONTACT_MSG_V3     0x10
#define PKT_CHANNEL_MSG_V3     0x11
#define PKT_CHANNEL_INFO       0x12

/* Push notifications (async, unsolicited) */
#define PUSH_ADVERT            0x80
#define PUSH_PATH_UPDATED      0x81
#define PUSH_ACK               0x82
#define PUSH_MSG_WAITING       0x83

/* Limits */
#define MAX_CHANNELS     8
#define MAX_CONTACTS    32
#define CONTACT_NAME_LEN 16
#define CHANNEL_NAME_LEN 32
#define MSG_TEXT_LEN    160

/* Channel info */
typedef struct {
    char name[CHANNEL_NAME_LEN + 1];
    uint8_t valid;
} channel_t;

/* Contact cache entry (compact: 6-byte prefix + short name) */
typedef struct {
    uint8_t prefix[6];
    char    name[CONTACT_NAME_LEN + 1];
} contact_t;

/* Parsed message */
typedef struct {
    char    sender[CONTACT_NAME_LEN + 1];
    char    text[MSG_TEXT_LEN + 1];
    uint8_t channel_idx;
    uint8_t is_channel;  /* 1=channel msg, 0=DM */
} mesh_msg_t;

/* Connect to meshcore device via modem, do APP_START handshake.
   Fills companion_name (up to 20 chars). Returns 1 on success. */
uint8_t mesh_connect(const char *ip_port, char *companion_name);

/* Query device info, populate mesh_num_channels. Returns max_channels. */
uint8_t mesh_query_device(void);

/* Fetch channel list (calls GET_CHANNEL for each index). */
uint8_t mesh_get_channels(void);

/* Fetch contacts into mesh_contacts[]. Returns count. */
uint8_t mesh_get_contacts(void);

/* Poll for next message. Returns 1 if message available in msg. */
uint8_t mesh_poll(mesh_msg_t *msg);

/* Send channel text message. If ack_out is non-NULL, copies 4-byte
   expected_ack code from MSG_SENT response. Returns 1 on success. */
uint8_t mesh_send(uint8_t channel_idx, const char *text, uint8_t *ack_out);

/* Send direct message to a contact. If ack_out is non-NULL, copies 4-byte
   expected_ack code from MSG_SENT response. Returns 1 on success. */
uint8_t mesh_send_dm(uint8_t contact_idx, const char *text, uint8_t *ack_out);

/* Process incoming serial data, handle push notifications.
   Call from main loop. Returns bitmask:
   bit 0 = PUSH_MSG_WAITING received
   bit 1 = PUSH_ACK(s) received (retrieve with mesh_next_ack) */
uint8_t mesh_process_incoming(void);

/* Retrieve next buffered PUSH_ACK code. Returns 1 and copies 4-byte
   ack code to ack_out if available, 0 if no more acks. */
uint8_t mesh_next_ack(uint8_t *ack_out);

/* Reconnect to device (re-dial + APP_START). Returns 1 on success. */
uint8_t mesh_reconnect(const char *ip_port, char *companion_name);

/* Number of consecutive failed commands (no response). */
extern uint8_t mesh_fail_count;

/* Threshold for considering connection lost */
#define MESH_FAIL_THRESHOLD 5

/* Global data */
extern channel_t  mesh_channels[MAX_CHANNELS];
extern uint8_t    mesh_num_channels;
extern contact_t  mesh_contacts[MAX_CONTACTS];
extern uint8_t    mesh_num_contacts;

#endif
