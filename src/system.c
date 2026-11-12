/* XLT - clock, battery, WLAN switch and CPU profiles. */
#include <psppower.h>
#include <psprtc.h>
#include <pspwlan.h>
#include <pspkernel.h>
#include <stdio.h>
#include "system.h"
#include "bg.h"
#include "psreg.h"

static u64 s_last_tick;
static volatile int s_h12, s_date_fmt = 2, s_light_off, s_fmt_gen;
static int s_fmt_seen;
static int s_have_status;
static int s_profile;
static int s_wanted = 111; /* what idle/boost asked for last */
static volatile int s_hold;

int system_tick(SysStatus *st)
{
    u64 now;
    sceRtcGetCurrentTick(&now);
    int fmt = s_fmt_gen != s_fmt_seen;
    s_fmt_seen = s_fmt_gen;
    if (s_have_status && now - s_last_tick < 1000000ull)
        return fmt;
    s_last_tick = now;

    SysStatus n;
    ScePspDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    n.hour = t.hour;
    n.minute = t.minute;
    n.battery = scePowerIsBatteryExist() ? scePowerGetBatteryLifePercent() : -1;
    n.charging = n.battery >= 0 && scePowerIsBatteryCharging();
    n.wifi_on = sceWlanGetSwitchState() == 1;

    int changed = fmt || !s_have_status || n.hour != st->hour || n.minute != st->minute ||
                  n.battery != st->battery || n.charging != st->charging || n.wifi_on != st->wifi_on;
    *st = n;
    s_have_status = 1;
    return changed;
}


static void job_formats(void *arg)
{
    int v;
    if (psreg_get_int("/CONFIG/DATE", "time_format", &v) == 0)
        s_h12 = v == 1;
    if (psreg_get_int("/CONFIG/DATE", "date_format", &v) == 0 && v >= 0 && v <= 2)
        s_date_fmt = v;
    if (psreg_get_int("/CONFIG/SYSTEM/POWER_SAVING", "backlight_off_interval", &v) == 0 && v >= 0)
        s_light_off = v;
    s_fmt_gen++; /* system_tick redraws the clock in the new format */
}

void system_read_formats(void) { bg_post(job_formats, NULL, 0); }
int system_h12(void) { return s_h12; }
int system_date_fmt(void) { return s_date_fmt; }
int system_backlight_off(void) { return s_light_off; }

void format_clock(char *b, int n, int hour, int minute)
{
    if (!s_h12) {
        snprintf(b, n, "%02d:%02d", hour, minute);
        return;
    }
    int h = hour % 12;
    snprintf(b, n, "%d:%02d %s", h ? h : 12, minute, hour < 12 ? "AM" : "PM");
}

static void set_profile(int mhz)
{
    if (s_profile == mhz)
        return;
    int r;
    switch (mhz) {
    case 111: r = scePowerSetClockFrequency(111, 111, 55); break;
    case 333: r = scePowerSetClockFrequency(333, 333, 166); break;
    default:  r = scePowerSetClockFrequency(222, 222, 111); break;
    }
    if (r >= 0)
        s_profile = mhz;
}

static void want(int mhz)
{
    s_wanted = mhz;
    if (!s_hold)
        set_profile(mhz);
}

/* One steady clock for the whole interface: every change makes the screen jump. */
void power_idle(void) { want(222); }
void power_boost(void) { want(222); }
void power_restore(void) { s_hold = 0; want(222); }

void power_hold(int mhz)
{
    s_hold = mhz;
    set_profile(mhz ? mhz : s_wanted);
}
