/* Renders the Save data screen on the PC: the real savedata.c (its drawing;
 * the scan finds nothing here) through the software GE of saver_preview.c,
 * over made-up saves and backups. Writes PPM frames.
 *
 *   sd_preview OUTDIR */
#define main saver_main
#include "saver_preview.c"
#undef main

#include "../src/savedata.c"

/* ------------------------------------------------------------ stand-ins */
int fop_start(int op, const char (*p)[FOP_PATH], int n, const char *d, const char **why) { (void)op; (void)p; (void)n; (void)d; *why = "no jobs on the PC"; return -1; }
int fop_busy(void) { return 0; }
void fop_status(FopStatus *o) { memset(o, 0, sizeof(*o)); o->bytes_total = 100; o->bytes_done = 45; }
int fop_take_result(FopStatus *o) { (void)o; return 0; }
void power_boost(void) {}
void power_idle(void) {}
int sysctl_usb_active(void) { return 0; }
const void *thumbs_get(const Entry *e) { (void)e; return NULL; }
uint32_t entry_key(const Entry *e) { (void)e; return 1; }
void sfx_play(int id) { (void)id; }
void ui_toast(const char *m) { printf("toast: %s\n", m); }
void menu_open(const char *t, const char *s, const MenuItem *it, int n, MenuFn fn) { (void)t; (void)s; (void)it; (void)n; (void)fn; }
static MenuItem s_mi[MENU_MAX];
MenuItem *menu_items(int *n) { if (n) *n = 0; return s_mi; }
void menu_close(void) {}
void format_size(char *b, int n, uint64_t v) { snprintf(b, n, "%.1f MB", v / 1048576.0); }
void format_date(char *b, int n, uint32_t m) { snprintf(b, n, "%02u.%02u.%04u", (m >> 16) & 31, (m >> 21) & 15, 1980 + (m >> 25)); }
void ui_draw_hints(const Hint *hints, int n, int cx, int y)
{
    int total = (n - 1) * 16;
    for (int i = 0; i < n; i++)
        total += (hints[i].icon >= 0 ? 12 : gfx_text_width(FONT_SMALL, hints[i].key)) + 4 + gfx_text_width(FONT_SMALL, hints[i].label);
    int x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        const Hint *h = &hints[i];
        if (h->icon >= 0) { gfx_icon(h->icon, x, y + 3, 12, 12, btn_tint(h->tint)); x += 16; }
        else x = gfx_text(FONT_SMALL, x, y, h->key, C_TEXT) + 4;
        x = gfx_text(FONT_SMALL, x, y, h->label, C_MUTED) + 16;
    }
}
void ui_draw_status_bar(const char *title, const SysStatus *st)
{
    gfx_text(FONT_SMALL, 12, 5, title, C_MUTED);
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", st->hour, st->minute);
    gfx_text(FONT_SMALL, SCREEN_W - 12 - gfx_text_width(FONT_SMALL, b), 5, b, C_TEXT);
}

static void fake(SdItem *it, const char *name, const char *title, const char *sub, uint64_t size, const char *day)
{
    memset(it, 0, sizeof(*it));
    snprintf(it->dir, sizeof(it->dir), "ms0:/PSP/SAVEDATA/%s", name);
    snprintf(it->name, sizeof(it->name), "%s", name);
    snprintf(it->title, sizeof(it->title), "%s", title);
    snprintf(it->sub, sizeof(it->sub), "%s", sub);
    snprintf(it->restore_name, sizeof(it->restore_name), "%s", name);
    snprintf(it->day, sizeof(it->day), "%s", day);
    it->size = size;
    it->mtime = (uint32_t)(46u << 25 | 11u << 21 | 7u << 16);
}

static SysStatus s_st = {21, 41, 76, 0, 1};

static void shot(const char *dir, const char *name)
{
    for (int i = 0; i < 60; i++) sd_step();
    gfx_begin(g_th->bg);
    sd_draw(&s_st);
    gfx_end();
    save(dir, name);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    dm = gfx_dither_matrix;
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, NULL);
    theme_set(argc > 2 ? atoi(argv[2]) : 0); /* a theme by number, to look at each one */
    static SdItem saves[7], backups[2];
    fake(&saves[0], "ULUS10041DATA00", "Crisis Core: Final Fantasy VII", "Chapter 3 - Banora", 1300000, "");
    fake(&saves[1], "UCUS98632SYS", "Patapon 2", "System data", 49000, "");
    fake(&saves[2], "ULES01213MHP2G00", "Monster Hunter Freedom Unite", "HR 6 \xCE\x9A\xCF\x85\xCE\xBD\xCE\xB7\xCE\xB3\xCF\x8C\xCF\x82", 2400000, "");
    fake(&saves[3], "NPUG80086", "Lumines", "", 12000, "");
    fake(&saves[4], "ULUS10336P3P01", "Persona 3 Portable", "Day 47 - Tartarus 54F, a rather long save title", 380000, "");
    fake(&saves[5], "UCUS98645", "Wipeout Pulse", "Career", 90000, "");
    fake(&saves[6], "ULUS10533", "Daxter", "Episode 5", 150000, "");
    fake(&backups[0], "ULUS10041DATA00", "Crisis Core: Final Fantasy VII", "Chapter 2", 1300000, "2026-11-08");
    fake(&backups[1], "UCUS98632SYS", "Patapon 2", "System data", 49000, "2026-11-01");
    D.items[TAB_SAVES] = saves; D.n[TAB_SAVES] = 7;
    D.items[TAB_BACKUPS] = backups; D.n[TAB_BACKUPS] = 2;
    D.open = 1;
    spring_snap(&D.t, FX(1));
    select_row(0, 1);
    shot(argv[1], "sd_1_saves");
    select_row(5, 0);
    shot(argv[1], "sd_2_scrolled");
    D.tab = TAB_BACKUPS; select_row(0, 1);
    J.kind = J_RESTORE_COPY;
    shot(argv[1], "sd_3_backups_restoring");
    J.kind = J_NONE;
    D.n[TAB_BACKUPS] = 0;
    shot(argv[1], "sd_4_no_backups");
    puts("sd_preview: ok");
    return 0;
}
