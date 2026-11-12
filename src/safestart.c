/* XLT - safe start: the decisions. See safestart.h. */
#include <stddef.h>
#include "safestart.h"

int g_safe_start;

int safestart_parse(const char *text, int len)
{
    if (len < 1 || text[0] < '0' || text[0] > '9')
        return 0;
    return text[0] - '0';
}

int safestart_decide(int failed, int held)
{
    if (held)
        return SAFE_HELD;
    return failed >= SAFE_AFTER_FAILS ? SAFE_CRASHED : SAFE_NO;
}

int safestart_next(int failed, int mode)
{
    if (mode != SAFE_NO)
        return 0; /* the next start tries normally again */
    return failed < 9 ? failed + 1 : 9; /* one digit: the file is rewritten in place */
}

const char *safestart_message(int mode)
{
    switch (mode) {
    case SAFE_HELD: return "Safe start: everything is back next time";
    case SAFE_CRASHED: return "XLT didn't start properly twice: safe start";
    default: return NULL;
    }
}
