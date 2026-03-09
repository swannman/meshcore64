#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define CONFIG_MAX_HOST 32

/* Load companion host from disk. Returns 1 if loaded, 0 if not found. */
uint8_t config_load(char *host);

/* Save companion host to disk. Returns 1 on success. */
uint8_t config_save(const char *host);

#endif
