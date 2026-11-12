#ifndef SYSTEM_H
#define SYSTEM_H

typedef struct {
    int hour, minute;
    int battery;  /* percent, -1 if no battery */
    int charging;
    int wifi_on;  /* WLAN switch */
} SysStatus;

/* Refreshes the status at most once per second. Returns 1 if anything shown changed. */
int system_tick(SysStatus *st);

/* The PSP's own time and date formats (System settings, in the registry).
 * system_read_formats() reads them on the background worker; until they
 * arrive the defaults are 24-hour and DD/MM/YYYY. */
void system_read_formats(void);
int system_h12(void);          /* 1: 12-hour clock */
int system_date_fmt(void);     /* registry date_format: 0 YYYY/MM/DD, 1 MM/DD/YYYY, 2 DD/MM/YYYY */
int system_backlight_off(void); /* firmware backlight auto-off in seconds, 0 = never */
/* "21:41", or "9:41 PM" with the 12-hour clock. */
void format_clock(char *b, int n, int hour, int minute);

/* CPU profiles. The whole interface stays at 222 MHz: idle and boost are the same
 * now, because every clock change makes the screen jump. */
void power_idle(void);
void power_boost(void);
void power_restore(void); /* 222 MHz, the firmware default, on exit */

/* Holds the clock at mhz (0 = release) and ignores idle/boost until released.
 * Used by the full-screen video player (333 MHz) and while music plays (222 MHz). */
void power_hold(int mhz);

#endif
