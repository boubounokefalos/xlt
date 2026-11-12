/* XLT - action sheet. See menu.h. */
#include <stdio.h>
#include <string.h>
#include "sfx.h"
#include "menu.h"
#include "ui_internal.h"
#include "anim.h"

#define SHEET_W 236
#define ROW_Y 52
#define ROW_H 24
#define PAD 14
#define VISIBLE 7

static struct {
    int open;
    Spring t;
    char title[96], sub[96];
    MenuItem items[MENU_MAX];
    int n, focus, top;
    MenuFn fn;
} M;

void menu_open(const char *title, const char *sub, const MenuItem *items, int n, MenuFn fn)
{
    snprintf(M.title, sizeof(M.title), "%s", title);
    snprintf(M.sub, sizeof(M.sub), "%s", sub ? sub : "");
    if (n > MENU_MAX) n = MENU_MAX;
    memcpy(M.items, items, sizeof(MenuItem) * n);
    M.n = n;
    M.focus = M.top = 0;
    M.fn = fn;
    M.open = 1;
    spring_to(&M.t, FX(1));
}

MenuItem *menu_items(int *n)
{
    if (n) *n = M.n;
    return M.items;
}

void menu_set_count(int n)
{
    M.n = n < 0 ? 0 : n > MENU_MAX ? MENU_MAX : n;
    if (M.focus >= M.n) M.focus = M.n ? M.n - 1 : 0;
}

void menu_close(void)
{
    M.open = 0;
    spring_to(&M.t, 0);
}

int menu_is_open(void) { return M.open || M.t.x > 0; }

static void focus(int i)
{
    if (M.n == 0) return;
    if ((i + M.n) % M.n != M.focus) sfx_play(SFX_MOVE);
    M.focus = (i + M.n) % M.n;
    if (M.focus < M.top) M.top = M.focus;
    if (M.focus >= M.top + VISIBLE) M.top = M.focus - VISIBLE + 1;
}

int menu_input(unsigned int p)
{
    if (!M.open)
        return 0;
    if (p & PSP_CTRL_CIRCLE || p & PSP_CTRL_TRIANGLE) {
        menu_close();
        return 1;
    }
    if (p & PSP_CTRL_UP) focus(M.focus - 1);
    else if (p & PSP_CTRL_DOWN) focus(M.focus + 1);
    else if (M.n && p & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        MenuItem *it = &M.items[M.focus];
        if (it->value[0] && M.fn)
            M.fn(it->id, p & (PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER) ? -1 : 1);
    } else if (M.n && p & PSP_CTRL_CROSS && M.fn) {
        M.fn(M.items[M.focus].id, 0);
    }
    return 1;
}

int menu_step(void)
{
    int old = M.t.x;
    spring_step(&M.t);
    return M.t.x != old;
}

void menu_draw(void)
{
    int t = M.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    ui_dim(t);
    const int w = SHEET_W, x = SCREEN_W - ((w * t) >> 8);
    gfx_rect(x, 0, w, SCREEN_H, C_SURFACE);
    gfx_rect(x, 0, 1, SCREEN_H, C_LINE);
    gfx_text_fit(FONT_BOLD, x + PAD, 12, w - 2 * PAD, M.title, C_TEXT);
    if (M.sub[0])
        gfx_text_fit(FONT_SMALL, x + PAD, 31, w - 2 * PAD, M.sub, C_MUTED);
    int y = ROW_Y;
    for (int i = M.top; i < M.n && i < M.top + VISIBLE; i++) {
        MenuItem *it = &M.items[i];
        if (it->gap && i > M.top) {
            gfx_rect(x + PAD, y + 1, w - 2 * PAD, 1, C_LINE);
            y += 6;
        }
        int focused = i == M.focus;
        if (focused)
            gfx_rrect(x + 8, y, w - 16, ROW_H - 2, 5, it->danger ? C_LOW : C_ACCENT);
        int ty = y + (ROW_H - 2 - gfx_line_h(FONT_BODY)) / 2;
        int vw = 0;
        if (it->value[0]) {
            char shown[48];
            snprintf(shown, sizeof(shown), focused ? "< %s >" : "%s", it->value);
            vw = gfx_text_width(FONT_SMALL, shown) + 8;
            gfx_text_right(FONT_SMALL, x + w - PAD - 2, ty + 1, shown, focused ? C_WHITE : C_MUTED);
        }
        gfx_text_fit(FONT_BODY, x + PAD + 2, ty, w - 2 * PAD - 4 - vw, it->label,
                     focused ? C_WHITE : it->danger ? C_LOW : C_TEXT);
        y += ROW_H;
    }
    if (M.top > 0)
        gfx_text_right(FONT_SMALL, x + w - 6, ROW_Y - 14, "^", C_MUTED);
    if (M.top + VISIBLE < M.n)
        gfx_text_right(FONT_SMALL, x + w - 6, 236, "v", C_MUTED);
    static const Hint hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Select"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Close"},
    };
    ui_draw_hints(hints, COUNT(hints), x + w / 2, 253);
}
