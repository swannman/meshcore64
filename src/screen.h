#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

/* Screen dimensions */
#define SCR_W       40
#define SCR_H       25
#define MSG_TOP      2
#define MSG_BOTTOM  22
#define MSG_LINES   21
#define INPUT_ROW   24
#define SEP_ROW2    23

/* Max channels with screen buffers (8 channels + 8 DM contact buffers) */
#define MAX_SCREEN_CHANNELS 16
#define DM_BUFFER_BASE       8   /* DM buffer for contact i = DM_BUFFER_BASE + i */
#define MAX_DM_BUFFERS       8   /* contacts 0..7 get dedicated buffers */

/* Initialize screen: colors, charset, separators */
void screen_init(void);

/* Set the local device name for highlighting @-mentions */
void screen_set_my_name(const char *name);

/* Initialize per-channel message buffers */
void screen_init_channel_bufs(void);

/* Set the currently displayed channel.
   Copies channel buffer to screen RAM. */
void screen_set_channel(uint8_t ch);

/* Update status bar (row 0). dm_mode: 0=show F1/F3 hint, 1=show F5/F7 hint */
void screen_status(const char *companion, const char *channel, uint8_t dm_mode);

/* Add a message to a specific channel's buffer with explicit colors.
   If ch is the current display channel, also updates screen. */
void screen_add_message_ch(uint8_t ch, const char *sender, const char *text,
                           uint8_t name_color, uint8_t text_color);

/* Add a message to the current channel (convenience wrapper) */
void screen_add_message(const char *sender, const char *text);

/* Clear message area of current channel */
void screen_clear_messages(void);

/* Draw input line.
   mention: if non-NULL, shown as "@[mention] " prefix before "> " prompt */
void screen_input_draw(const char *buf, uint8_t cursor_pos,
                       const char *mention);

/* Show a system message on current channel (yellow) */
void screen_system_msg(const char *text);

/* Confirm a pending message in a channel buffer:
   scan forward for sender lines with old_color, skip `skip` matches,
   then replace that sender portion with new_color.
   skip=0 confirms the oldest matching line. */
void screen_confirm_line(uint8_t ch, uint8_t skip,
                         uint8_t old_color, uint8_t new_color);

#endif
