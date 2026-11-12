/* XLT - fixed-point spring easing. No floats, no allocation.
 *
 * Values are Q8 (256 = 1.0 / one pixel). One step per displayed frame:
 *   v += k*(target - x) - c*v ;  x += v
 * k = 0.19, c = 0.47 gives a quick settle with a slight overshoot.
 */
#ifndef ANIM_H
#define ANIM_H

#define FX(n) ((n) << 8)
#define FX_INT(x) (((x) + 128) >> 8)

typedef struct {
    int x, v, target;
} Spring;

#define SPRING_K 49
#define SPRING_C 120

static inline void spring_snap(Spring *s, int target)
{
    s->x = s->target = target;
    s->v = 0;
}

static inline void spring_to(Spring *s, int target)
{
    s->target = target;
}

/* Advances one frame with stiffness k and damping c (Q8); returns 1 while it moves. */
static inline int spring_step_kc(Spring *s, int k, int c)
{
    int d = s->target - s->x;
    if (d == 0 && s->v == 0)
        return 0;
    s->v += (d * k - s->v * c) >> 8;
    s->x += s->v;
    d = s->target - s->x;
    if (d < 32 && d > -32 && s->v < 24 && s->v > -24) {
        s->x = s->target;
        s->v = 0;
        return 0;
    }
    return 1;
}

/* Advances one frame (the launcher's usual feel); returns 1 while the spring is still moving. */
static inline int spring_step(Spring *s) { return spring_step_kc(s, SPRING_K, SPRING_C); }

static inline int spring_int(const Spring *s)
{
    return FX_INT(s->x);
}

#endif
