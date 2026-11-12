/* Host test for the portable parts of src/fileops.c. */
#include <stdio.h>
#include <string.h>
#include "../src/fileops.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *s_taken[8];
static int exists(const char *p)
{
    for (int i = 0; s_taken[i]; i++)
        if (!strcmp(s_taken[i], p))
            return 1;
    return 0;
}

int main(void)
{
    CHECK(!strcmp(fop_basename("ms0:/PICTURE/a.jpg"), "a.jpg"), "basename");
    CHECK(!strcmp(fop_basename("ms0:/a"), "a"), "basename at the root");
    CHECK(fop_is_inside("ms0:/A/B", "ms0:/A"), "inside");
    CHECK(fop_is_inside("ms0:/A", "ms0:/A/"), "itself");
    CHECK(fop_is_inside("ms0:/a/b", "ms0:/A"), "FAT ignores case");
    CHECK(!fop_is_inside("ms0:/AB", "ms0:/A"), "a sibling with the same prefix");
    char out[256];
    s_taken[0] = NULL;
    fop_unique_name("ms0:/X", "a.txt", out, sizeof(out), exists);
    CHECK(!strcmp(out, "a.txt"), "free name kept: %s", out);
    s_taken[0] = "ms0:/X/a.txt";
    s_taken[1] = "ms0:/X/a (2).txt";
    s_taken[2] = NULL;
    fop_unique_name("ms0:/X/", "a.txt", out, sizeof(out), exists);
    CHECK(!strcmp(out, "a (3).txt"), "numbered: %s", out);
    s_taken[0] = "ms0:/X/Folder";
    s_taken[1] = NULL;
    fop_unique_name("ms0:/X", "Folder", out, sizeof(out), exists);
    CHECK(!strcmp(out, "Folder (2)"), "folder: %s", out);
    s_taken[0] = "ms0:/.hidden";
    s_taken[1] = NULL;
    fop_unique_name("ms0:/", ".hidden", out, sizeof(out), exists);
    CHECK(!strcmp(out, ".hidden (2)"), "dot file: %s", out);
    if (fails)
        return 1;
    printf("test_fileops: all passed\n");
    return 0;
}
