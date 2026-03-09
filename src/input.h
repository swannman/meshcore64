#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* Input actions */
#define INPUT_NONE      0
#define INPUT_SUBMIT    1
#define INPUT_NEXT_CH   2
#define INPUT_PREV_CH   3
#define INPUT_NEXT_CONTACT 4
#define INPUT_PREV_CONTACT 5
#define INPUT_SETTINGS     6
#define INPUT_UP           7
#define INPUT_DOWN         8

/* Max input length (40 - "> " - cursor) */
#define INPUT_MAX_LEN   37

/* Initialize input system */
void input_init(void);

/* Non-blocking poll. Returns action code. */
uint8_t input_poll(void);

/* Get current input buffer */
const char *input_buf(void);

/* Clear input buffer */
void input_clear(void);

/* Set @-mention prefix displayed before the prompt.
   Pass NULL to clear. Redraws the input line. */
void input_set_mention(const char *name);

/* Get current mention (NULL if none) */
const char *input_mention(void);

#endif
