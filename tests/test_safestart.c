/* Safe start's decisions: when it happens and what the guard file says next. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "safestart.h"

int main(void)
{
    assert(safestart_parse("0\n", 2) == 0);
    assert(safestart_parse("2\n", 2) == 2);
    assert(safestart_parse("", 0) == 0);
    assert(safestart_parse("x", 1) == 0); /* a damaged file is not a reason for a safe start */

    assert(safestart_decide(0, 0) == SAFE_NO);
    assert(safestart_decide(1, 0) == SAFE_NO);   /* one bad start can be a pulled battery */
    assert(safestart_decide(2, 0) == SAFE_CRASHED);
    assert(safestart_decide(9, 0) == SAFE_CRASHED);
    assert(safestart_decide(0, 1) == SAFE_HELD); /* L wins, whatever the count */
    assert(safestart_decide(5, 1) == SAFE_HELD);

    assert(safestart_next(0, SAFE_NO) == 1);
    assert(safestart_next(1, SAFE_NO) == 2);
    assert(safestart_next(9, SAFE_NO) == 9);     /* stays one digit */
    assert(safestart_next(2, SAFE_CRASHED) == 0); /* after a safe start, normal again */
    assert(safestart_next(1, SAFE_HELD) == 0);

    /* crash, crash, safe, then a normal try */
    int f = 0, m;
    m = safestart_decide(f, 0); assert(m == SAFE_NO); f = safestart_next(f, m);
    m = safestart_decide(f, 0); assert(m == SAFE_NO); f = safestart_next(f, m);
    m = safestart_decide(f, 0); assert(m == SAFE_CRASHED); f = safestart_next(f, m);
    m = safestart_decide(f, 0); assert(m == SAFE_NO);

    assert(safestart_message(SAFE_NO) == NULL);
    assert(strlen(safestart_message(SAFE_HELD)) < 96);
    assert(strlen(safestart_message(SAFE_CRASHED)) < 96);
    puts("test_safestart: ok");
    return 0;
}
