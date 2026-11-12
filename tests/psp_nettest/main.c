/* XLTNET: XLT's Wi-Fi start (src/net.c, unchanged) alone, with nothing else of XLT
 * running. Every step is printed on the screen and appended to net.log next to the
 * EBOOT. Then it joins the saved connection used last (or the first) and looks up
 * a host name. To tell whether XLT's network code or the rest of XLT is at fault. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "net.h"

PSP_MODULE_INFO("XLTNET", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(4 * 1024);

static char s_log[256];

static void say(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    pspDebugScreenPrintf("%s", line);
    SceUID fd = sceIoOpen(s_log, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}

static int exit_cb(int a, int b, void *c) { sceKernelExitGame(); return 0; }
static int cb_thread(SceSize a, void *b)
{
    int id = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

static int run(SceSize a, void *b)
{
    say("XLTNET: XLT's Wi-Fi code alone. %d KB free", (int)(sceKernelTotalFreeMemSize() >> 10));
    if (net_start() < 0) { say("net_start failed: %s", net_error()); return 0; }
    int ids[NET_PROFILES];
    char names[NET_PROFILES][64];
    int n = net_profiles(ids, names, NET_PROFILES);
    for (int i = 0; i < n; i++) say("  connection %d: %s", ids[i], names[i]);
    say("joining...");
    if (net_connect(0, NULL) < 0) { say("join failed: %s", net_error()); return 0; }
    say("connected to %s", net_profile_name());
    struct hostent *h = gethostbyname("example.com");
    say("example.com %s", h ? "found" : "NOT found");
    say("done. HOME to quit");
    return 0;
}

int main(int argc, char **argv)
{
    pspDebugScreenInit();
    snprintf(s_log, sizeof(s_log), "%s", argv[0]);
    char *slash = strrchr(s_log, '/');
    if (slash) strcpy(slash + 1, "net.log");
    sceIoRemove(s_log);
    int t = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    net_set_log(say);
    /* the same kind of thread as XLT's network thread (webfetch.c) */
    t = sceKernelCreateThread("xl_webnet", run, 0x2A, 0x40000, PSP_THREAD_ATTR_USER, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    sceKernelSleepThreadCB();
    return 0;
}
