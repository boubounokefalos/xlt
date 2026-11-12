/* Host test for what "Delete from library" may remove. */
#include <stdio.h>
#include <string.h>
#include "../src/delpath.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("OK   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static const char *SELF = "ms0:/PSP/GAME/XLT/EBOOT.PBP";

static int t(const char *path, int eboot, const char *want)
{
    char out[200] = "";
    int r = del_target(path, eboot, SELF, out, sizeof(out));
    if (!want)
        return r == -1;
    return r == (eboot ? 1 : 0) && !strcmp(out, want);
}

int main(void)
{
    CHECK(t("ms0:/ISO/game.iso", 0, "ms0:/ISO/game.iso"), "ISO file");
    CHECK(t("ef0:/ISO/CAT_RPG/sub/game.CSO", 0, "ef0:/ISO/CAT_RPG/sub/game.CSO"), "nested CSO on ef0");
    CHECK(t("ms0:/iso/game.zso", 0, "ms0:/iso/game.zso"), "case-insensitive root");
    CHECK(t("ms0:/PSP/GAME/APP/EBOOT.PBP", 1, "ms0:/PSP/GAME/APP"), "homebrew folder");
    CHECK(t("ms0:/PSP/GAME/CAT_Emu/APP/EBOOT.PBP", 1, "ms0:/PSP/GAME/CAT_Emu/APP"), "category app folder");
    CHECK(t("ms0:/PSP/GAME/EBOOT.PBP", 1, NULL), "never the GAME folder itself");
    CHECK(t("ms0:/PSP/GAME/XLT/EBOOT.PBP", 1, NULL), "never XLT itself");
    CHECK(t("ms0:/PSP/GAME/XLT/EBOOT.PBP", 1, NULL), "never XLT (case)");
    CHECK(t("ms0:/PSP/GAME/XLT2/EBOOT.PBP", 1, "ms0:/PSP/GAME/XLT2"), "similar name is fine");
    CHECK(t("ms0:/PSP/GAME/../SAVEDATA/EBOOT.PBP", 1, NULL), "no ..");
    CHECK(t("ms0:/PSP/GAME/APP/other.pbp", 1, NULL), "only EBOOT.PBP");
    CHECK(t("ms0:/PSP/SAVEDATA/X/EBOOT.PBP", 1, NULL), "not outside scan folders");
    CHECK(t("ms0:/ISO/game.iso", 1, NULL), "disc path not as eboot");
    CHECK(t("ms0:/PSP/GAME/APP/EBOOT.PBP", 0, NULL), "eboot path not as disc");
    CHECK(t("ms0:/MUSIC/song.mp3", 0, NULL), "not media");
    CHECK(t("ms0:/ISO/readme.txt", 0, NULL), "only disc images");
    CHECK(t("ms0:/ISOX/game.iso", 0, NULL), "root must match whole name");
    CHECK(t("host0:/ISO/game.iso", 0, NULL), "only ms0/ef0");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
