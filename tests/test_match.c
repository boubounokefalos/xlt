/* Host test for search matching. */
#include <stdio.h>
#include "../src/match.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("OK   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

int main(void)
{
    CHECK(match_score("Persona 3 Portable", "") == 0, "empty query matches nothing");
    CHECK(match_score("Persona 3 Portable", "   ") == 0, "blank query matches nothing");
    CHECK(match_score("Persona 3 Portable", "xyz") == 0, "no match");
    CHECK(match_score("Persona 3 Portable", "PERS") > 0, "case-insensitive");
    CHECK(match_score("Persona 3 Portable", "port pers") > 0, "words in any order");
    CHECK(match_score("Persona 3 Portable", "port zzz") == 0, "every word must match");
    CHECK(match_score("Tekken 6", "tek") > match_score("Ridge Racer 2", "rac"), "title prefix beats word prefix");
    CHECK(match_score("Ridge Racer 2", "rac") > match_score("Burnout Legends", "nout"), "word prefix beats substring");
    CHECK(match_score("Patapon 2", "pon") > 0, "plain substring matches");
    CHECK(match_score("God of War: Chains", "war") > match_score("Starwars", "war"), "word start preferred");
    CHECK(match_score("Monster Hunter", "monster h") > 0, "multi-word prefix");
    CHECK(match_score("Lumines", "lumines ") > 0, "trailing space ignored");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
