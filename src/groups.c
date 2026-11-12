/* XLT - tab overrides and folders for Games / Homebrew. See groups.h. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "groups.h"

#define PATH_LEN 200

typedef struct {
    char path[PATH_LEN];
    unsigned char tab;
    char folder[GROUP_NAME];
} Item;

static Item s_items[GROUP_ITEMS];
static int s_nitems;
static char s_folders[GROUPS_TABS][GROUP_FOLDERS][GROUP_NAME];
static int s_nfolders[GROUPS_TABS];

static int valid_tab(int t) { return t >= 0 && t < GROUPS_TABS; }

void groups_reset(void)
{
    s_nitems = 0;
    memset(s_nfolders, 0, sizeof(s_nfolders));
}

static Item *find_item(const char *path)
{
    for (int i = 0; i < s_nitems; i++)
        if (!strcmp(s_items[i].path, path))
            return &s_items[i];
    return NULL;
}

static void drop_item(Item *it)
{
    int i = (int)(it - s_items);
    memmove(&s_items[i], &s_items[i + 1], sizeof(Item) * (s_nitems - i - 1));
    s_nitems--;
}

/* Bytes in the UTF-8 character that starts with lead. */
static int utf8_len(unsigned char lead)
{
    return lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
}

int groups_clean_name(const char *in, char *out)
{
    while (*in == ' ')
        in++;
    int n = 0;
    for (; *in && n < GROUP_NAME - 1; in++)
        if (*in != '\t' && *in != '\n' && *in != '\r')
            out[n++] = *in;
    if (n > 0) { /* cut at the size limit: don't leave half a character */
        int k = n - 1;
        while (k > 0 && ((unsigned char)out[k] & 0xC0) == 0x80)
            k--;
        if (n - k < utf8_len((unsigned char)out[k]))
            n = k;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    return n > 0;
}

int groups_folder_count(int tab) { return valid_tab(tab) ? s_nfolders[tab] : 0; }

const char *groups_folder_name(int tab, int i)
{
    return valid_tab(tab) && i >= 0 && i < s_nfolders[tab] ? s_folders[tab][i] : "";
}

int groups_folder_find(int tab, const char *name)
{
    if (!valid_tab(tab) || !name || !name[0])
        return -1;
    for (int i = 0; i < s_nfolders[tab]; i++)
        if (!strcasecmp(s_folders[tab][i], name))
            return i;
    return -1;
}

int groups_folder_add(int tab, const char *name)
{
    char clean[GROUP_NAME];
    if (!valid_tab(tab) || !groups_clean_name(name, clean))
        return -3;
    if (groups_folder_find(tab, clean) >= 0)
        return -2;
    if (s_nfolders[tab] >= GROUP_FOLDERS)
        return -1;
    int at = s_nfolders[tab];
    while (at > 0 && strcasecmp(s_folders[tab][at - 1], clean) > 0) {
        memcpy(s_folders[tab][at], s_folders[tab][at - 1], GROUP_NAME);
        at--;
    }
    memcpy(s_folders[tab][at], clean, GROUP_NAME);
    s_nfolders[tab]++;
    return 0;
}

static void remove_folder_at(int tab, int i)
{
    memmove(s_folders[tab][i], s_folders[tab][i + 1], (size_t)GROUP_NAME * (s_nfolders[tab] - i - 1));
    s_nfolders[tab]--;
}

int groups_folder_rename(int tab, const char *old_name, const char *new_name)
{
    int i = groups_folder_find(tab, old_name);
    char clean[GROUP_NAME], old[GROUP_NAME];
    if (i < 0 || !groups_clean_name(new_name, clean))
        return -3;
    int j = groups_folder_find(tab, clean);
    if (j >= 0 && j != i)
        return -2;
    memcpy(old, s_folders[tab][i], GROUP_NAME);
    remove_folder_at(tab, i);
    groups_folder_add(tab, clean); /* re-sorted */
    for (int k = 0; k < s_nitems; k++)
        if (s_items[k].tab == tab && !strcasecmp(s_items[k].folder, old))
            memcpy(s_items[k].folder, clean, GROUP_NAME);
    return 0;
}

void groups_folder_delete(int tab, const char *name)
{
    int i = groups_folder_find(tab, name);
    if (i < 0)
        return;
    char old[GROUP_NAME];
    memcpy(old, s_folders[tab][i], GROUP_NAME);
    remove_folder_at(tab, i);
    for (int k = 0; k < s_nitems; k++)
        if (s_items[k].tab == tab && !strcasecmp(s_items[k].folder, old))
            s_items[k].folder[0] = 0; /* back to All, still in this tab */
}

int groups_tab(const char *path, int def)
{
    const Item *it = find_item(path);
    return it ? it->tab : def;
}

const char *groups_folder(const char *path)
{
    const Item *it = find_item(path);
    if (!it || !it->folder[0] || groups_folder_find(it->tab, it->folder) < 0)
        return "";
    return s_folders[it->tab][groups_folder_find(it->tab, it->folder)];
}

int groups_set(const char *path, int def_tab, int tab, const char *folder)
{
    if (!valid_tab(tab) || !path || strlen(path) >= PATH_LEN)
        return -1;
    if (!folder) folder = "";
    int f = -1;
    if (folder[0] && (f = groups_folder_find(tab, folder)) < 0)
        return -2;
    Item *it = find_item(path);
    if (tab == def_tab && f < 0) { /* nothing to remember: its type puts it there */
        if (it) drop_item(it);
        return 0;
    }
    if (!it) {
        if (s_nitems >= GROUP_ITEMS)
            return -1;
        it = &s_items[s_nitems++];
        snprintf(it->path, sizeof(it->path), "%s", path);
    }
    it->tab = (unsigned char)tab;
    snprintf(it->folder, sizeof(it->folder), "%s", f >= 0 ? s_folders[tab][f] : "");
    return 0;
}

/* ------------------------------------------------------------- file text */

static char tab_char(int t) { return t == 1 ? 'H' : 'G'; }
static int tab_of(char c) { return c == 'H' ? 1 : c == 'G' ? 0 : -1; }

/* Copies the field at *p (up to a tab or the end of the line) into out. */
static int field(const char **p, char *out, int cap)
{
    int n = 0;
    while (**p && **p != '\t' && **p != '\n' && **p != '\r') {
        if (n < cap - 1) out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    if (**p == '\t') {
        (*p)++;
        return 1; /* more follows */
    }
    return 0;
}

int groups_parse(const char *text)
{
    groups_reset();
    int used = 0;
    const char *p = text;
    while (p && *p) {
        char kind[4], tab[4], folder[GROUP_NAME + 8], path[PATH_LEN + 8];
        folder[0] = path[0] = 0;
        int more = field(&p, kind, sizeof(kind));
        if (more) more = field(&p, tab, sizeof(tab));
        else tab[0] = 0;
        if (more) more = field(&p, folder, sizeof(folder));
        if (more) field(&p, path, sizeof(path));
        while (*p && *p != '\n') p++; /* the rest of a line we don't know */
        if (*p == '\n') p++;
        int t = tab_of(tab[0]);
        if (t < 0)
            continue;
        if (!strcmp(kind, "F") && groups_folder_add(t, folder) == 0) {
            used++;
        } else if (!strcmp(kind, "I") && path[0] && strlen(path) < PATH_LEN && !find_item(path) &&
                   s_nitems < GROUP_ITEMS) {
            Item *it = &s_items[s_nitems++];
            memcpy(it->path, path, strlen(path) + 1); /* checked to fit above */
            it->tab = (unsigned char)t;
            groups_clean_name(folder, it->folder); /* "" stays "" */
            used++;
        }
    }
    return used;
}

int groups_serialized_max(void)
{
    return GROUPS_TABS * GROUP_FOLDERS * (GROUP_NAME + 6) + GROUP_ITEMS * (PATH_LEN + GROUP_NAME + 8) + 1;
}

int groups_serialize(char *out, int cap)
{
    int n = 0, w;
    for (int t = 0; t < GROUPS_TABS; t++)
        for (int i = 0; i < s_nfolders[t]; i++) {
            w = snprintf(out + n, cap - n, "F\t%c\t%s\n", tab_char(t), s_folders[t][i]);
            if (w < 0 || w >= cap - n) return -1;
            n += w;
        }
    for (int i = 0; i < s_nitems; i++) {
        const Item *it = &s_items[i];
        w = snprintf(out + n, cap - n, "I\t%c\t%s\t%s\n", tab_char(it->tab), it->folder, it->path);
        if (w < 0 || w >= cap - n) return -1;
        n += w;
    }
    return n;
}
