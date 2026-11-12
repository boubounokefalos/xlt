/* XLT - interface sounds (tools/bake_sounds.py). One hardware audio channel of
 * their own, so they mix over music; off with Settings > Sound > Interface sounds. */
#ifndef SFX_H
#define SFX_H

#include "sfx_ids.h"

void sfx_init(void);
void sfx_shutdown(void);

/* Main thread. Several in one frame: the most important one plays (sfx_commit). */
void sfx_play(int id);
void sfx_commit(void);  /* once per frame, after input: starts what sfx_play picked */
void sfx_now(int id);   /* starts at once (outside the frame loop, e.g. the boot notes) */
void sfx_drain(int max_ms); /* waits until nothing plays (before a game starts) */

#endif
