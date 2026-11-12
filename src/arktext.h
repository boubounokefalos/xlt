#ifndef ARKTEXT_H
#define ARKTEXT_H

/* Clock that applies to `id` (NULL = all games) in SETTINGS.TXT text. */
int arktext_get_clock(const char *text, const char *id);

/* Writes `text` with the clock lines for `id` (NULL = "game") replaced by
 * `clock` (CLOCK_AUTO removes them) into out. Returns length or -1. */
int arktext_set_clock(const char *text, const char *id, int clock, int ark4, char *out, int cap);

#endif
