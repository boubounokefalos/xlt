/* Stand-ins for everything ui.c, opts.c and menu.c call that the PC previews
 * (tests/ui_preview.c) leave out: nothing open, nothing found, 0 everywhere.
 * Made from the prototypes in the headers in src; add a line when ui.c calls something new. */
#include <stddef.h>
#include <stdint.h>
#include "safestart.h"
int g_safe_start;
int g_rec_preview; /* ui_preview sets it to draw the recording light */
int rec_active(void) { return g_rec_preview; }
int rec_seconds(void) { return 83; }
#include "ark.h"
#include "boot.h"
#include "cc.h"
#include "delpath.h"
#include "fm.h"
#include "fsdel.h"
#include "gamecfg.h"
#include "launch.h"
#include "menu.h"
#include "nowplaying.h"
#include "osd.h"
#include "osk.h"
#include "plugins.h"
#include "preview.h"
#include "reader.h"
#include "saver.h"
#include "search.h"
#include "settings.h"
#include "sysctl.h"
#include "system.h"
#include "thumbs.h"
#include "umd.h"
#include "video.h"
#include "viewer.h"
#include "web.h"
#include "groups.h"

const char * ark_dir(void) { return NULL; }
int ark_get_clock(const char *id) { return 0; }
int ark_set_clock(const char *id, int clock) { return 0; }
int ark_version(void) { return 0; }
int boot_covers(void) { return 0; }
void boot_draw(void) { }
void cc_draw(const SysStatus *st) { }
int cc_input(unsigned int pressed) { return 0; }
int cc_is_open(void) { return 0; }
int cc_step(void) { return 0; }
void cc_toggle(void) { }
int cc_usb_covers(void) { return 0; }
void cc_usb_draw(const SysStatus *st) { }
int cc_usb_input(unsigned int pressed) { return 0; }
int cc_usb_screen(void) { return 0; }
const char * clock_label(int clock) { return "Default"; }
int del_target(const char *path, int is_eboot, const char *self_path, char *out, int n) { return 0; }
int fm_busy(void) { return 0; }
int fm_covers(void) { return 0; }
void fm_draw(const SysStatus *st) { }
int fm_input(unsigned int pressed) { return 0; }
int fm_is_open(void) { return 0; }
void fm_open(const char *path) { }
int fm_place_count(void) { return 0; }
int fm_place_icon(int i) { return 0; }
const char * fm_place_name(int i) { return NULL; }
const char * fm_place_path(int i) { return NULL; }
const Entry * fm_preview_entry(void) { return NULL; }
int fm_step(void) { return 0; }
int fs_delete(const char *path, int is_dir) { return 0; }
void gamecfg_forget(const Entry *e) { }
void gamecfg_load(const Entry *e, GameCfg *out) { }
void gamecfg_save(const Entry *e, const GameCfg *cfg) { }
void globalcfg_save(void) { }
void groups_save(void) { }
int np_covers(void) { return 0; }
void np_draw(const SysStatus *st) { }
int np_input(unsigned int pressed) { return 0; }
int np_is_open(void) { return 0; }
void np_open(void) { }
int np_step(void) { return 0; }
void osd_draw(void) { }
int osd_step(void) { return 0; }
void osk_draw(void) { }
int osk_input(unsigned int pressed) { return 0; }
int osk_is_open(void) { return 0; }
void osk_open(const char *title, const char *initial, int mode, OskFn fn) { }
int osk_step(void) { return 0; }
int plugins_for(const Entry *e, PlugState *out, int max) { return 0; }
int plugins_set(const Entry *e, const PlugState *p, int override) { return 0; }
void power_hold(int mhz) { }
int preview_draw(const Entry *e, int x, int y, int w, int h) { return 0; }
int preview_step(void) { return 0; }
void preview_target(const Entry *e) { }
void preview_want_thumb(const Entry *e) { }
int reader_covers(void) { return 0; }
void reader_draw(const SysStatus *st) { }
int reader_input(unsigned int pressed) { return 0; }
int reader_is_open(void) { return 0; }
int reader_step(void) { return 0; }
int saver_covers(void) { return 0; }
void saver_draw(const SysStatus *st) { }
int saver_is_open(void) { return 0; }
int search_covers(void) { return 0; }
void search_draw(const SysStatus *st) { }
int search_input(unsigned int pressed) { return 0; }
int search_is_open(void) { return 0; }
void search_open(void) { }
void search_refresh(void) { }
int search_step(void) { return 0; }
int settings_covers(void) { return 0; }
void settings_draw(const SysStatus *st) { }
int settings_input(unsigned int pressed) { return 0; }
int settings_is_open(void) { return 0; }
void settings_open(void) { }
void settings_open_section(int s) { }
int settings_recent(int i) { return 0; }
int settings_section_icon(int s) { return 0; }
const char * settings_section_name(int s) { return NULL; }
const char * settings_section_sub(int s) { return NULL; }
int settings_step(void) { return 0; }
int sysctl_usb_active(void) { return 0; }
int thumbs_missing(const Entry *e) { return 0; }
int thumbs_pump(void) { return 0; }
const Entry * umd_entry(void) { return NULL; }
int umd_poll(void) { return 0; }
int umd_state(void) { return 0; }
int video_covers(void) { return 0; }
void video_draw(void) { }
int video_input(unsigned int pressed) { return 0; }
int video_is_open(void) { return 0; }
void video_open(const Entry *e) { }
int video_step(void) { return 0; }
int viewer_covers(void) { return 0; }
void viewer_draw(void) { }
int viewer_input(unsigned int pressed) { return 0; }
int viewer_is_open(void) { return 0; }
void viewer_open(const Entry *e) { }
void viewer_refresh(void) { }
int viewer_step(void) { return 0; }
int web_covers(void) { return 0; }
void web_draw(const SysStatus *st) { }
#include "gfx.h"
#include <pspgu.h>
#include "webicon_data.h"
void web_draw_icon(int id, int x, int y, int w, int h, unsigned int color) /* as web.c: the home tile's globe */
{
    const short *r = webicon_rects[id];
    gfx_bind(webicon_pixels, GU_PSM_4444, WEBICON_W, WEBICON_H, WEBICON_W, 1);
    gfx_sprite(x, y, x + w, y + h, r[0], r[1], r[0] + r[2], r[1] + r[2], color);
    gfx_bind_atlas();
}
int web_input(unsigned int pressed) { return 0; }
int web_is_open(void) { return 0; }
const char * web_last_title(void) { return NULL; }
int web_step(void) { return 0; }
int web_tile_count(void) { return 0; }
const char * web_tile_name(int i) { return NULL; }
void web_tile_open(int i) { }
const char * web_tile_sub(int i) { return NULL; }

/* Added by hand: Save data and video positions (the previews have none). */
#include "savedata.h"
#include "video.h"
void sd_open(void) { }
int sd_is_open(void) { return 0; }
int sd_covers(void) { return 0; }
int sd_input(unsigned int pressed) { return 0; }
int sd_step(void) { return 0; }
void sd_draw(const SysStatus *st) { }
int video_place(const char *path, int *dur_ms) { if (dur_ms) *dur_ms = 0; return 0; }
void viewer_hold(unsigned int held) { }
