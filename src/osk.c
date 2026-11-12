/* XLT - on-screen keyboard. See osk.h.
 *
 * Four rows of ten characters and a row of actions (Shift, ABC/ΑΒΓ, #+=,
 * Space, Del, Done). ✕ presses the focused key, □ deletes, △ types a space,
 * L/R move the cursor, SELECT switches layout, START is Done, ○ cancels.
 */
#include <stdio.h>
#include <string.h>
#include "sfx.h"
#include "osk.h"
#include "ui_internal.h"
#include "anim.h"

#define COLS 10
#define ROWS 4
#define KEY_W 37
#define KEY_H 22
#define GAP 3
#define PANEL_W (COLS * KEY_W + (COLS - 1) * GAP + 24)
#define PANEL_X ((SCREEN_W - PANEL_W) / 2)
#define PANEL_Y 22
#define FIELD_Y (PANEL_Y + 34)
#define FIELD_H 24
#define KEYS_Y (FIELD_Y + FIELD_H + 10)
#define PANEL_H (KEYS_Y - PANEL_Y + 5 * KEY_H + 4 * GAP + 12)

enum { LAY_LOWER, LAY_UPPER, LAY_GR_LOWER, LAY_GR_UPPER, LAY_SYM, LAY_COUNT };
enum { ACT_SHIFT, ACT_LANG, ACT_SYM, ACT_SPACE, ACT_DEL, ACT_DONE, ACT_COUNT };

static const char *const LAYOUT[LAY_COUNT][ROWS] = {
    {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.,"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_.,"},
    {"άέήίόύώϊϋΐ", "ςερτυθιοπ-", "ασδφγηξκλ'", "ζχψωβνμ_.,"},
    {"ΆΈΉΊΌΎΏΪΫ.", "ΣΕΡΤΥΘΙΟΠ-", "ΑΣΔΦΓΗΞΚΛ'", "ΖΧΨΩΒΝΜ_.,"},
    {"1234567890", "!@#$%^&()+", "=[]{};'`~,", "€£¥§°±«»…·"},
};
/* OSK_URL (the browser's address bar): / and : on the letter layouts, ? and friends on the symbols. */
static const char *const LAYOUT_URL[LAY_COUNT][ROWS] = {
    {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm/.:"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM/.:"},
    {"άέήίόύώϊϋΐ", "ςερτυθιοπ-", "ασδφγηξκλ'", "ζχψωβνμ/.:"},
    {"ΆΈΉΊΌΎΏΪΫ.", "ΣΕΡΤΥΘΙΟΠ-", "ΑΣΔΦΓΗΞΚΛ'", "ΖΧΨΩΒΝΜ/.:"},
    {"1234567890", "!@#$%^&()+", "=?_[]{};~,", "€£¥§°±«»…·"},
};
static const char *const ACT_LABEL[ACT_COUNT] = {"Shift", "ΑΒΓ", "#+=", "Space", "Del", "Done"};

/* Each key's character, split out of the layout strings once. */
static char s_keys[2][LAY_COUNT][ROWS][COLS][5]; /* [1]: OSK_URL */
#define KEYS (s_keys[K.mode == OSK_URL])
static int s_split;

static struct {
    int open;
    Spring t;
    char title[64];
    char text[OSK_MAX + 1];
    int cursor; /* byte offset */
    int mode, layout, last_letters;
    int row, col;
    OskFn fn;
    const char *err;
} K;

static int utf8_len(unsigned char c) { return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

static void split_layouts(void)
{
    for (int l = 0; l < LAY_COUNT; l++)
        for (int r = 0; r < ROWS; r++) {
            for (int u = 0; u < 2; u++) {
                const char *s = u ? LAYOUT_URL[l][r] : LAYOUT[l][r];
                for (int c = 0; c < COLS && *s; c++) {
                    int n = utf8_len((unsigned char)*s);
                    memcpy(s_keys[u][l][r][c], s, n);
                    s_keys[u][l][r][c][n] = 0;
                    s += n;
                }
            }
        }
    s_split = 1;
}

void osk_open(const char *title, const char *initial, int mode, OskFn fn)
{
    if (!s_split)
        split_layouts();
    snprintf(K.title, sizeof(K.title), "%s", title);
    snprintf(K.text, sizeof(K.text), "%s", initial ? initial : "");
    K.cursor = (int)strlen(K.text);
    K.mode = mode;
    K.fn = fn;
    K.err = NULL;
    K.layout = mode == OSK_NUMBER ? LAY_SYM : K.last_letters;
    K.row = mode == OSK_NUMBER ? 0 : 1;
    K.col = 0;
    K.open = 1;
    spring_to(&K.t, FX(1));
}

int osk_is_open(void) { return K.open || K.t.x > 0; }

static void close_with(const char *text)
{
    K.open = 0;
    spring_to(&K.t, 0);
    if (K.layout != LAY_SYM)
        K.last_letters = K.layout;
    OskFn fn = K.fn;
    K.fn = NULL;
    if (fn)
        fn(text);
}

static int allowed(const char *ch)
{
    if (K.mode == OSK_NUMBER)
        return ch[0] >= '0' && ch[0] <= '9' && !ch[1];
    if (K.mode == OSK_FILENAME && !ch[1] && strchr("\\/:*?\"<>|", ch[0]))
        return 0;
    return 1;
}

static void insert(const char *ch)
{
    K.err = NULL;
    if (!allowed(ch)) {
        sfx_play(SFX_ERROR); /* a character names can't have */
        return;
    }
    int n = (int)strlen(ch), len = (int)strlen(K.text);
    if (len + n > OSK_MAX || (K.mode == OSK_NUMBER && len >= 6)) {
        sfx_play(SFX_ERROR);
        return;
    }
    sfx_play(SFX_KEY); /* a soft click per letter, not the ✕ chime */
    memmove(K.text + K.cursor + n, K.text + K.cursor, len - K.cursor + 1);
    memcpy(K.text + K.cursor, ch, n);
    K.cursor += n;
}

static void backspace(void)
{
    K.err = NULL;
    if (K.cursor == 0)
        return;
    sfx_play(SFX_DEL);
    int p = K.cursor - 1;
    while (p > 0 && ((unsigned char)K.text[p] & 0xC0) == 0x80)
        p--;
    memmove(K.text + p, K.text + K.cursor, strlen(K.text) - K.cursor + 1);
    K.cursor = p;
}

static void move_cursor(int dir)
{
    if ((dir < 0 && K.cursor > 0) || (dir > 0 && K.text[K.cursor]))
        sfx_play(SFX_MOVE);
    if (dir < 0 && K.cursor > 0) {
        K.cursor--;
        while (K.cursor > 0 && ((unsigned char)K.text[K.cursor] & 0xC0) == 0x80)
            K.cursor--;
    } else if (dir > 0 && K.text[K.cursor]) {
        K.cursor += utf8_len((unsigned char)K.text[K.cursor]);
    }
}

static void done(void)
{
    char out[OSK_MAX + 1];
    const char *s = K.text;
    while (*s == ' ')
        s++;
    snprintf(out, sizeof(out), "%s", s);
    int n = (int)strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || (K.mode == OSK_FILENAME && out[n - 1] == '.')))
        out[--n] = 0;
    if (!n) {
        K.err = K.mode == OSK_NUMBER ? "Type a number" : K.mode == OSK_FILENAME ? "Type a name" : "Type something";
        sfx_play(SFX_ERROR);
        return;
    }
    if (K.mode == OSK_FILENAME) {
        for (int i = 0; i < n; i++)
            if (strchr("\\/:*?\"<>|", out[i])) {
                K.err = "A name can't have \\ / : * ? \" < > |";
                sfx_play(SFX_ERROR);
                return;
            }
        if (!strcmp(out, "..")) {
            K.err = "That name is taken by the system";
            sfx_play(SFX_ERROR);
            return;
        }
    }
    sfx_play(SFX_SELECT); /* Done: the chime, once */
    close_with(out);
}

static void action(int a)
{
    switch (a) {
    case ACT_SHIFT:
        if (K.layout == LAY_LOWER) K.layout = LAY_UPPER;
        else if (K.layout == LAY_UPPER) K.layout = LAY_LOWER;
        else if (K.layout == LAY_GR_LOWER) K.layout = LAY_GR_UPPER;
        else if (K.layout == LAY_GR_UPPER) K.layout = LAY_GR_LOWER;
        break;
    case ACT_LANG:
        K.layout = K.layout == LAY_LOWER ? LAY_GR_LOWER : K.layout == LAY_UPPER ? LAY_GR_UPPER
                 : K.layout == LAY_GR_UPPER ? LAY_UPPER : LAY_LOWER;
        break;
    case ACT_SYM:
        K.layout = K.layout == LAY_SYM ? K.last_letters : (K.last_letters = K.layout, LAY_SYM);
        break;
    case ACT_SPACE: insert(" "); break;
    case ACT_DEL: backspace(); break;
    case ACT_DONE: done(); break;
    }
}

int osk_input(unsigned int p)
{
    if (!K.open)
        return 0;
    if (p & PSP_CTRL_CIRCLE) { close_with(NULL); return 1; }
    if (p & PSP_CTRL_START) { done(); return 1; }
    if (p & PSP_CTRL_SQUARE) { backspace(); return 1; }
    if (p & PSP_CTRL_TRIANGLE) { insert(" "); return 1; }
    if (p & PSP_CTRL_LTRIGGER) { move_cursor(-1); return 1; }
    if (p & PSP_CTRL_RTRIGGER) { move_cursor(1); return 1; }
    if (p & PSP_CTRL_SELECT) { action(K.layout == LAY_SYM ? ACT_SYM : ACT_LANG); return 1; }
    if (p & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
        sfx_play(SFX_MOVE); /* the keyboard wraps: every press moves */
    if (p & PSP_CTRL_UP) {
        if (K.row == ROWS) K.col = K.col * COLS / ACT_COUNT + 1;
        K.row = (K.row + ROWS) % (ROWS + 1);
        if (K.row == ROWS) K.col = K.col * ACT_COUNT / COLS;
    } else if (p & PSP_CTRL_DOWN) {
        if (K.row == ROWS) K.col = K.col * COLS / ACT_COUNT + 1;
        K.row = (K.row + 1) % (ROWS + 1);
        if (K.row == ROWS) K.col = K.col * ACT_COUNT / COLS;
    } else if (p & PSP_CTRL_LEFT) {
        int n = K.row == ROWS ? ACT_COUNT : COLS;
        K.col = (K.col + n - 1) % n;
    } else if (p & PSP_CTRL_RIGHT) {
        int n = K.row == ROWS ? ACT_COUNT : COLS;
        K.col = (K.col + 1) % n;
    } else if (p & PSP_CTRL_CROSS) {
        if (K.row == ROWS) action(K.col);
        else insert(KEYS[K.layout][K.row][K.col]);
    }
    if (K.row < ROWS && K.col >= COLS) K.col = COLS - 1;
    if (K.row == ROWS && K.col >= ACT_COUNT) K.col = ACT_COUNT - 1;
    return 1;
}

int osk_step(void)
{
    int old = K.t.x;
    spring_step(&K.t);
    return K.t.x != old;
}

void osk_draw(void)
{
    int t = K.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    ui_dim(t);
    gfx_set_opacity(t);
    int y0 = PANEL_Y + ((256 - t) * 16 >> 8);
    int dy = y0 - PANEL_Y;
    gfx_rrect(PANEL_X, y0, PANEL_W, PANEL_H, 10, C_SURFACE);
    gfx_text_fit(FONT_BOLD, PANEL_X + 12, y0 + 9, PANEL_W - 24, K.title, C_TEXT);

    /* field: scrolled so the cursor stays visible */
    int fx = PANEL_X + 12, fw = PANEL_W - 24, fy = FIELD_Y + dy;
    gfx_rrect(fx, fy, fw, FIELD_H, 5, C_BG);
    const char *start = K.text;
    while (*start) {
        char tmp[OSK_MAX + 1];
        int n = (int)(K.text + K.cursor - start);
        if (n < 0) n = 0;
        memcpy(tmp, start, n);
        tmp[n] = 0;
        if (gfx_text_width(FONT_BODY, tmp) <= fw - 16)
            break;
        start += utf8_len((unsigned char)*start);
    }
    int ty = fy + (FIELD_H - gfx_line_h(FONT_BODY)) / 2;
    int cx = gfx_text_n(FONT_BODY, fx + 6, ty, start, (int)(K.text + K.cursor - start), C_TEXT);
    gfx_text_fit(FONT_BODY, cx, ty, fx + fw - 6 - cx, K.text + K.cursor, C_TEXT);
    gfx_rect(cx, fy + 4, 2, FIELD_H - 8, C_FOCUS);
    if (K.err)
        gfx_text_right(FONT_SMALL, PANEL_X + PANEL_W - 12, y0 + 11, K.err, C_LOW);

    /* keys */
    int ky = KEYS_Y + dy;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int x = PANEL_X + 12 + c * (KEY_W + GAP), y = ky + r * (KEY_H + GAP);
            int on = K.row == r && K.col == c;
            const char *ch = KEYS[K.layout][r][c];
            int ok = allowed(ch);
            gfx_rrect(x, y, KEY_W, KEY_H, 4, on ? C_ACCENT : C_RAISED);
            gfx_text_center(FONT_BODY, x + KEY_W / 2, y + (KEY_H - gfx_line_h(FONT_BODY)) / 2, ch,
                            on ? C_WHITE : ok ? C_TEXT : C_LINE);
        }
    int aw = (COLS * KEY_W + (COLS - 1) * GAP - (ACT_COUNT - 1) * GAP) / ACT_COUNT;
    for (int a = 0; a < ACT_COUNT; a++) {
        int x = PANEL_X + 12 + a * (aw + GAP), y = ky + ROWS * (KEY_H + GAP);
        int on = K.row == ROWS && K.col == a;
        int lit = (a == ACT_SHIFT && (K.layout == LAY_UPPER || K.layout == LAY_GR_UPPER)) ||
                  (a == ACT_SYM && K.layout == LAY_SYM);
        const char *label = ACT_LABEL[a];
        if (a == ACT_LANG)
            label = K.layout == LAY_GR_LOWER || K.layout == LAY_GR_UPPER ? "ABC" : "\xce\x91\xce\x92\xce\x93";
        gfx_rrect(x, y, aw, KEY_H, 4, on ? C_ACCENT : a == ACT_DONE ? C_ACCENT_DK : C_RAISED);
        gfx_text_center(FONT_SMALL, x + aw / 2, y + (KEY_H - gfx_line_h(FONT_SMALL)) / 2, label,
                        on ? C_WHITE : lit ? C_FOCUS : C_TEXT);
    }
    gfx_set_opacity(256);
    static const Hint hints[] = {
        {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Del"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Space"},
        {-1, "L/R", 0, "Cursor"},
        {-1, "SELECT", 0, "Layout"},
        {-1, "START", 0, "Done"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Cancel"},
    };
    ui_draw_hints(hints, COUNT(hints), SCREEN_W / 2, 253);
}
