/* XLT - Wi-Fi. See net.h.
 *
 * The modules and the start-up order are psp5's (wifi.c): utility net
 * modules COMMON and INET, sceNetInit, sceNetInetInit, sceNetApctlInit; plus
 * the resolver, which curl needs for host names. Instead of the system's
 * connection dialog, XLT joins a saved connection itself with
 * sceNetApctlConnect and watches the state, so nothing but XLT's own screen
 * is ever shown. */
#include <pspkernel.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputility_modules.h>
#include <psputility_netparam.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspwlan.h>
#include <stdio.h>
#include <string.h>
#include "net.h"

#define MODULE_ALREADY_LOADED 0x80111102
#define MODULE_ALREADY_LOADED2 0x80020139 /* as music.c accepts */

static int s_started;
static volatile int s_state = NET_IDLE;
static char s_err[96];
static char s_name[64];
static int s_preferred;

int net_state(void) { return s_state; }
const char *net_error(void) { return s_err; }
const char *net_profile_name(void) { return s_name; }
void net_set_preferred(int id) { s_preferred = id; }
int net_preferred(void) { return s_preferred; }

static void (*s_log)(const char *fmt, ...);
static volatile const char *s_step = "";
const char *net_step(void) { return (const char *)s_step; }
void net_set_log(void (*fn)(const char *fmt, ...)) { s_log = fn; }
#define NLOG(...) do { if (s_log) s_log(__VA_ARGS__); } while (0)
#define STEP(name) do { step = name; s_step = name; NLOG("wifi: %s", name); } while (0)

static int s_modules;

/* Loads the two modules alone (net_start does it too, when the browser first needs the
 * network). Loading NET_INET once hung the PSP: the cause was the import tables split by
 * stub libraries named twice in the Makefile, not when the modules are loaded. */
int net_load_modules(void)
{
    if (s_modules)
        return 0;
    int r = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (r < 0 && r != (int)MODULE_ALREADY_LOADED && r != (int)MODULE_ALREADY_LOADED2) {
        NLOG("wifi: load NET_COMMON at start: %08X", (unsigned)r);
        return -1;
    }
    r = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (r < 0 && r != (int)MODULE_ALREADY_LOADED && r != (int)MODULE_ALREADY_LOADED2) {
        NLOG("wifi: load NET_INET at start: %08X", (unsigned)r);
        sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
        return -1;
    }
    s_modules = 1;
    return 0;
}

/* Each step is logged before it runs (the log file is closed after every line),
 * so if one never returns, web.log says which. */
int net_start(void)
{
    if (s_started)
        return 0;
    s_state = NET_STARTING;
    int r;
    NLOG("wifi: starting, %d KB free", (int)(sceKernelTotalFreeMemSize() >> 10));
    /* The older loader, as PSPDX and the SDK samples use: loaded with sceUtilityLoadModule
     * the libraries were never linked to the application (sceNetInit: 0x8002013A). */
    const char *step;
    if (!s_modules) {
        STEP("load NET_COMMON");
        r = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
        if (r < 0 && r != (int)MODULE_ALREADY_LOADED && r != (int)MODULE_ALREADY_LOADED2) goto fail;
        STEP("load NET_INET");
        r = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
        if (r < 0 && r != (int)MODULE_ALREADY_LOADED && r != (int)MODULE_ALREADY_LOADED2) goto fail;
        s_modules = 1;
    }
    STEP("sceNetInit");
    if ((r = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024)) < 0) goto fail; /* as PSPDX (pspkit-https) */
    STEP("sceNetInetInit");
    if ((r = sceNetInetInit()) < 0) goto fail;
    STEP("sceNetResolverInit");
    if ((r = sceNetResolverInit()) < 0) goto fail;
    STEP("sceNetApctlInit");
    if ((r = sceNetApctlInit(10 * 1024, 48)) < 0) goto fail; /* as arkMenu */
    s_step = "";
    NLOG("wifi: started, %d KB free", (int)(sceKernelTotalFreeMemSize() >> 10));
    s_started = 1;
    s_state = NET_IDLE;
    return 0;
fail:
    NLOG("wifi: %s failed: %08X", step, (unsigned)r);
    snprintf(s_err, sizeof(s_err), "The network could not be started (%s %08X, %d KB free)", step, (unsigned)r,
             (int)(sceKernelTotalFreeMemSize() >> 10));
    s_state = NET_FAILED;
    return -1;
}

int net_profiles(int *ids, char (*names)[64], int max)
{
    int n = 0;
    for (int i = 1; i <= NET_PROFILES && n < max; i++) {
        if (sceUtilityCheckNetParam(i) != 0)
            continue;
        netData d;
        memset(&d, 0, sizeof(d));
        if (sceUtilityGetNetParam(i, PSP_NETPARAM_NAME, &d) == 0 && d.asString[0])
            snprintf(names[n], 64, "%s", d.asString);
        else
            snprintf(names[n], 64, "Connection %d", i);
        ids[n++] = i;
    }
    return n;
}

int net_is_up(void)
{
    int st = 0;
    return s_started && sceNetApctlGetState(&st) >= 0 && st == PSP_NET_APCTL_STATE_GOT_IP;
}

int net_connect(int id, volatile int *cancel)
{
    s_err[0] = 0;
    if (sceWlanGetSwitchState() != 1) {
        snprintf(s_err, sizeof(s_err), "Turn on the Wi-Fi switch");
        s_state = NET_FAILED;
        return -1;
    }
    if (net_start() < 0)
        return -1;
    if (net_is_up()) {
        s_state = NET_READY;
        return 0;
    }
    int ids[NET_PROFILES];
    char names[NET_PROFILES][64];
    int n = net_profiles(ids, names, NET_PROFILES);
    if (n == 0) {
        snprintf(s_err, sizeof(s_err), "No saved Wi-Fi connection: add one in Settings > Network Settings");
        s_state = NET_FAILED;
        return -1;
    }
    if (id <= 0) id = s_preferred;
    int k = 0;
    for (int i = 0; i < n; i++)
        if (ids[i] == id) k = i;
    id = ids[k];
    snprintf(s_name, sizeof(s_name), "%s", names[k]);

    sceNetApctlDisconnect();
    s_state = NET_JOINING;
    NLOG("wifi: joining connection %d (%s)", id, s_name);
    int r = sceNetApctlConnect(id);
    if (r < 0) {
        snprintf(s_err, sizeof(s_err), "Couldn't connect to %s (%08X)", s_name, (unsigned)r);
        s_state = NET_FAILED;
        return -1;
    }
    int last = -1, seen_joining = 0;
    for (int t = 0; t < 30000 / 50; t++) {
        if (cancel && *cancel) {
            sceNetApctlDisconnect();
            s_state = NET_IDLE;
            snprintf(s_err, sizeof(s_err), "Stopped");
            return -1;
        }
        int st = 0;
        if (sceNetApctlGetState(&st) < 0)
            break;
        if (st != last) {
            NLOG("wifi: state %d", st);
            last = st;
            if (st == PSP_NET_APCTL_STATE_GETTING_IP) s_state = NET_GETTING_IP;
            else if (st != PSP_NET_APCTL_STATE_DISCONNECTED) s_state = NET_JOINING;
        }
        if (st == PSP_NET_APCTL_STATE_GOT_IP) {
            s_state = NET_READY;
            s_preferred = id;
            return 0;
        }
        if (st != PSP_NET_APCTL_STATE_DISCONNECTED) seen_joining = 1;
        else if (seen_joining) break; /* it went back to disconnected: refused */
        sceKernelDelayThread(50000);
    }
    sceNetApctlDisconnect();
    snprintf(s_err, sizeof(s_err), "Couldn't connect to %s", s_name);
    s_state = NET_FAILED;
    return -1;
}

void net_disconnect(void)
{
    if (s_started)
        sceNetApctlDisconnect();
    s_state = NET_IDLE;
}

void net_shutdown(void)
{
    if (!s_started)
        return;
    sceNetApctlDisconnect();
    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    s_modules = 0;
    s_started = 0;
    s_state = NET_IDLE;
}
