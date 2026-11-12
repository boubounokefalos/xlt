/* XLT - a side sheet of actions (the file manager's and the reader's △ menu),
 * drawn like the options sheet. The owner fills the rows and gets a callback. */
#ifndef MENU_H
#define MENU_H

#define MENU_MAX 40

typedef struct {
    char label[48];
    char value[40]; /* shown on the right; rows with a value change with ←/→ */
    int id;
    int danger;     /* drawn red (Delete) */
    int gap;        /* a separator line above */
} MenuItem;

/* dir: 0 for ✕, -1/+1 for ←/→ (only on rows with a value).
 * The callback may change the rows (menu_items()) or call menu_close(). */
typedef void (*MenuFn)(int id, int dir);

void menu_open(const char *title, const char *sub, const MenuItem *items, int n, MenuFn fn);
MenuItem *menu_items(int *n);  /* the rows, to update values or labels from the callback */
void menu_set_count(int n);    /* after rows were added or removed */
void menu_close(void);
int menu_is_open(void);        /* open or still sliding out */
int menu_input(unsigned int pressed);
int menu_step(void);
void menu_draw(void);

#endif
