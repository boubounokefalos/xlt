/* groups.c: folders, tab overrides, the text file round trip. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "groups.h"

int main(void)
{
    groups_reset();
    assert(groups_tab("ms0:/ISO/a.iso", 0) == 0);
    assert(groups_folder("ms0:/ISO/a.iso")[0] == 0);

    /* folders: sorted, unique (case ignored), cleaned names */
    assert(groups_folder_add(0, "RPG") == 0);
    assert(groups_folder_add(0, "  Action  ") == 0);
    assert(groups_folder_add(0, "rpg") == -2);
    assert(groups_folder_add(0, "   ") == -3);
    assert(groups_folder_count(0) == 2);
    assert(!strcmp(groups_folder_name(0, 0), "Action"));
    assert(!strcmp(groups_folder_name(0, 1), "RPG"));
    assert(groups_folder_add(1, "RPG") == 0); /* per tab */
    assert(groups_folder_add(1, "Emulators") == 0);

    /* entries */
    assert(groups_set("ms0:/ISO/a.iso", 0, 0, "rpg") == 0);
    assert(!strcmp(groups_folder("ms0:/ISO/a.iso"), "RPG"));
    assert(groups_set("ms0:/ISO/b.iso", 0, 0, "Nope") == -2);
    assert(groups_set("ms0:/PSP/GAME/EMU/EBOOT.PBP", 1, 0, "") == 0); /* homebrew shown in Games */
    assert(groups_tab("ms0:/PSP/GAME/EMU/EBOOT.PBP", 1) == 0);
    assert(groups_set("ms0:/PSP/GAME/EMU/EBOOT.PBP", 1, 1, "") == 0); /* back home: forgotten */
    assert(groups_tab("ms0:/PSP/GAME/EMU/EBOOT.PBP", 1) == 1);
    assert(groups_set("ms0:/PSP/GAME/EMU/EBOOT.PBP", 1, 1, "Emulators") == 0);

    /* rename follows the entries, delete sends them to All */
    assert(groups_folder_rename(0, "RPG", "Role-playing") == 0);
    assert(!strcmp(groups_folder("ms0:/ISO/a.iso"), "Role-playing"));
    assert(groups_folder_rename(0, "Role-playing", "action") == -2);
    char text[8192];
    int n = groups_serialize(text, sizeof(text));
    assert(n > 0 && n < (int)sizeof(text));

    groups_folder_delete(0, "Role-playing");
    assert(groups_folder("ms0:/ISO/a.iso")[0] == 0);
    assert(groups_tab("ms0:/ISO/a.iso", 0) == 0);

    /* round trip of the text saved before the delete */
    assert(groups_parse(text) == 6);
    assert(!strcmp(groups_folder("ms0:/ISO/a.iso"), "Role-playing"));
    assert(!strcmp(groups_folder("ms0:/PSP/GAME/EMU/EBOOT.PBP"), "Emulators"));
    assert(groups_folder_count(1) == 2);
    char again[8192];
    assert(groups_serialize(again, sizeof(again)) == n && !memcmp(text, again, n));
    assert(groups_serialize(again, 10) == -1);

    /* junk lines are skipped */
    assert(groups_parse("garbage\nF\tX\tBad\nF\tG\tOk\nI\tG\t\tms0:/ISO/c.iso\n\nI\tH\n") == 2);
    assert(groups_folder_count(0) == 1 && groups_tab("ms0:/ISO/c.iso", 1) == 0);

    /* UTF-8 names cut at a character boundary */
    char out[GROUP_NAME];
    char longname[80] = "";
    for (int i = 0; i < 20; i++) strcat(longname, "\xCE\xB1"); /* 20 x Greek alpha, 40 bytes */
    assert(groups_clean_name(longname, out) && strlen(out) == 30);
    assert(groups_clean_name("a\tb\nc", out) && !strcmp(out, "abc"));

    /* limits */
    groups_reset();
    for (int i = 0; i < GROUP_FOLDERS; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "F%02d", i);
        assert(groups_folder_add(0, nm) == 0);
    }
    assert(groups_folder_add(0, "more") == -1);
    for (int i = 0; i < GROUP_ITEMS; i++) {
        char p[32];
        snprintf(p, sizeof(p), "ms0:/ISO/%d.iso", i);
        assert(groups_set(p, 0, 1, "") == 0);
    }
    assert(groups_set("ms0:/ISO/x.iso", 0, 1, "") == -1);
    char *big = malloc(groups_serialized_max());
    assert(groups_serialize(big, groups_serialized_max()) > 0);
    free(big);
    printf("test_groups: all passed\n");
    return 0;
}
