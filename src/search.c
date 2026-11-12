/* XLT - search.
 *
 * Left: a 7x6 key grid (✕ types, □ deletes, △ adds a space). Right: games and
 * homebrew whose title matches, best first, updated on every key. → past the
 * grid (or R / START) moves to the results; ✕ there starts the game and △
 * opens its options. ○ closes. Like everything else, it only redraws on input.
 */
#include <stdio.h>
#include <string.h>
#include "sfx.h"
#include "search.h"
#include "ui_internal.h"
#include "anim.h"
#include "library.h"
#include "match.h"
#include "opts.h"

#define COLS 7
#define ROWS 6
#define KEY_W 28
#define KEY_H 24
#define KEY_GAP 3
#define GRID_X 12
#define GRID_Y 62
#define FIELD_Y 30
#define RES_X 240
#define RES_W (SCREEN_W - 12 - RES_X)
#define RES_Y 50
#define RES_ROW 22
#define RES_ROWS 8
#define MAX_RESULTS 64
#define MAX_QUERY 32

static const char *const s_keys[COLS * ROWS] = {
    "A", "B", "C", "D", "E", "F", "G",
    "H", "I", "J", "K", "L", "M", "N",
    "O", "P", "Q", "R", "S", "T", "U",
    "V", "W", "X", "Y", "Z", "1", "2",
    "3", "4", "5", "6", "7", "8", "9",
    "0", "-", ":", "'", "&", "Spc", "Del",
};

typedef struct { int view, idx, score; } Hit;

static struct {
    int open;
    Spring t;
    char q[MAX_QUERY + 1];
    int row, col;
    int in_results;
    Hit hits[MAX_RESULTS];
    int n, total;
    int sel, top;
} F;

static void run_query(void)
{
    F.n = F.total = 0;
    static const int views[] = {VIEW_GAMES, VIEW_HOMEBREW, VIEW_MUSIC, VIEW_VIDEO}; /* not pictures or removed */
    for (int vi = 0; vi < 4; vi++) {
        int v = views[vi];
        for (int i = 0; i < library_count(v); i++) {
            int s = match_score(library_get(v, i)->title, F.q);
            if (!s)
                continue;
            F.total++;
            /* insert into the top MAX_RESULTS, best score first (views are already by title) */
            int pos = F.n;
            while (pos > 0 && F.hits[pos - 1].score < s)
                pos--;
            if (pos >= MAX_RESULTS)
                continue;
            int last = F.n < MAX_RESULTS ? F.n : MAX_RESULTS - 1;
            for (int j = last; j > pos; j--)
                F.hits[j] = F.hits[j - 1];
            F.hits[pos] = (Hit){v, i, s};
            if (F.n < MAX_RESULTS)
                F.n++;
        }
    }
    F.sel = F.top = 0;
    if (F.n == 0)
        F.in_results = 0;
}

void search_refresh(void)
{
    if (F.open)
        run_query();
}

void search_open(void)
{
    F.open = 1;
    F.in_results = 0;
    F.row = F.col = 0;
    spring_to(&F.t, FX(1));
    run_query(); /* keeps the last query: reopening shows the same results */
}

int search_is_open(void) { return F.open || F.t.x > 0; }
int search_covers(void) { return F.open && F.t.x >= FX(1); }

static void type(const char *k)
{
    sfx_play(!strcmp(k, "Del") ? SFX_DEL : SFX_KEY); /* a soft click per letter, not the ✕ chime */
    int n = strlen(F.q);
    if (!strcmp(k, "Del")) {
        if (n) F.q[n - 1] = 0;
    } else if (n < MAX_QUERY) {
        const char *c = !strcmp(k, "Spc") ? " " : k;
        if (*c == ' ' && (n == 0 || F.q[n - 1] == ' '))
            return; /* no leading or double spaces */
        F.q[n] = *c;
        F.q[n + 1] = 0;
    }
    run_query();
}

static void select_hit(int i)
{
    if (i < 0) i = 0;
    if (i >= F.n) i = F.n - 1;
    if (i != F.sel) sfx_play(SFX_MOVE);
    F.sel = i;
    if (F.sel < F.top) F.top = F.sel;
    if (F.sel >= F.top + RES_ROWS) F.top = F.sel - RES_ROWS + 1;
}

static const Entry *current(void)
{
    return F.n ? library_get(F.hits[F.sel].view, F.hits[F.sel].idx) : NULL;
}

int search_input(unsigned int p)
{
    if (!F.open)
        return 0;
    if (p & PSP_CTRL_CIRCLE) {
        F.open = 0;
        spring_to(&F.t, 0);
        return 1;
    }
    if (p & PSP_CTRL_SQUARE) { /* delete works from anywhere */
        type("Del");
        return 1;
    }
    if (F.in_results) {
        if (p & PSP_CTRL_UP) select_hit(F.sel - 1);
        else if (p & PSP_CTRL_DOWN) select_hit(F.sel + 1);
        else if (p & (PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER)) F.in_results = 0;
        else if (p & PSP_CTRL_CROSS && current()) ui_request_launch(current());
        else if (p & PSP_CTRL_TRIANGLE && current()) opts_open(current());
        return 1;
    }
    if (p & PSP_CTRL_UP && F.row > 0) { F.row--; sfx_play(SFX_MOVE); }
    else if (p & PSP_CTRL_DOWN && F.row < ROWS - 1) { F.row++; sfx_play(SFX_MOVE); }
    else if (p & PSP_CTRL_LEFT && F.col > 0) { F.col--; sfx_play(SFX_MOVE); }
    else if (p & PSP_CTRL_RIGHT) {
        if (F.col < COLS - 1) F.col++;
        else if (F.n) F.in_results = 1;
    } else if (p & (PSP_CTRL_RTRIGGER | PSP_CTRL_START) && F.n) {
        F.in_results = 1;
    } else if (p & PSP_CTRL_CROSS) {
        type(s_keys[F.row * COLS + F.col]);
    } else if (p & PSP_CTRL_TRIANGLE) {
        type("Spc");
    }
    return 1;
}

int search_step(void)
{
    int old = F.t.x;
    spring_step(&F.t);
    return F.t.x != old;
}

void search_draw(const SysStatus *st)
{
    int t = F.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    ui_draw_status_bar("Search", st);

    /* query field */
    gfx_rrect(GRID_X, FIELD_Y - 4, COLS * (KEY_W + KEY_GAP) - KEY_GAP, 26, 6, C_RAISED);
    int fx = GRID_X + 8, fy = FIELD_Y + (18 - gfx_line_h(FONT_BODY)) / 2;
    int end = F.q[0] ? gfx_text(FONT_BODY, fx, fy, F.q, C_TEXT) : fx;
    if (!F.q[0])
        gfx_text(FONT_BODY, fx + 4, fy, "Title", C_MUTED);
    gfx_rect(end + 1, FIELD_Y, 2, 16, C_FOCUS); /* caret */

    /* key grid */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const char *k = s_keys[r * COLS + c];
            int x = GRID_X + c * (KEY_W + KEY_GAP), y = GRID_Y + r * (KEY_H + KEY_GAP);
            int focus = !F.in_results && F.row == r && F.col == c;
            gfx_rrect(x, y, KEY_W, KEY_H, 5, focus ? C_ACCENT : C_SURFACE);
            int font = strlen(k) > 1 ? FONT_SMALL : FONT_BODY;
            gfx_text_center(font, x + KEY_W / 2, y + (KEY_H - gfx_line_h(font)) / 2, k,
                            focus ? C_WHITE : strlen(k) > 1 ? C_MUTED : C_TEXT);
        }

    /* results */
    char buf[48];
    if (!F.q[0]) {
        snprintf(buf, sizeof(buf), "%d games, apps, songs and videos",
                 library_count(VIEW_GAMES) + library_count(VIEW_HOMEBREW) + library_count(VIEW_MUSIC) +
                 library_count(VIEW_VIDEO));
        gfx_text(FONT_SMALL, RES_X, FIELD_Y, buf, C_MUTED);
    } else if (F.total == 0) {
        gfx_text(FONT_SMALL, RES_X, FIELD_Y, "No matches", C_MUTED);
    } else {
        if (F.total > F.n)
            snprintf(buf, sizeof(buf), "%d matches (best %d shown)", F.total, F.n);
        else
            snprintf(buf, sizeof(buf), F.total == 1 ? "1 match" : "%d matches", F.total);
        gfx_text(FONT_SMALL, RES_X, FIELD_Y, buf, C_MUTED);
    }
    for (int i = F.top; i < F.n && i < F.top + RES_ROWS; i++) {
        const Entry *e = library_get(F.hits[i].view, F.hits[i].idx);
        int y = RES_Y + (i - F.top) * RES_ROW;
        int focus = F.in_results && i == F.sel;
        if (focus)
            gfx_rrect(RES_X - 6, y, RES_W + 6, RES_ROW, 5, C_ACCENT);
        const char *kind = kind_name(e->kind);
        int kw = gfx_text_width(FONT_SMALL, kind);
        int ty = y + (RES_ROW - gfx_line_h(FONT_BODY)) / 2;
        gfx_text_fit(focus ? FONT_BOLD : FONT_BODY, RES_X, ty, RES_W - kw - 12, e->title,
                     focus ? C_WHITE : C_TEXT);
        gfx_text_right(FONT_SMALL, RES_X + RES_W - 4, ty + 1, kind, focus ? C_WHITE : C_MUTED);
    }

    static const Hint keys_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Type"},
        {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Delete"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Space"},
        {-1, "R", 0, "Results"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    static const Hint res_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Play"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Options"},
        {-1, "L", 0, "Keys"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    gfx_rect(0, 248, SCREEN_W, 1, C_LINE);
    if (F.in_results)
        ui_draw_hints(res_hints, COUNT(res_hints), SCREEN_W / 2, 253);
    else
        ui_draw_hints(keys_hints, COUNT(keys_hints), SCREEN_W / 2, 253);
    gfx_set_opacity(256);
}
