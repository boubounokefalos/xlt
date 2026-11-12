/* XLT - Wi-Fi for the web browser: the network modules, the PSP's saved
 * connections and joining one. The blocking calls run on the browser's
 * network thread (webfetch.c), never on the UI thread. */
#ifndef NET_H
#define NET_H

#define NET_PROFILES 10

enum {
    NET_IDLE,       /* nothing started yet */
    NET_STARTING,   /* loading the network modules */
    NET_JOINING,    /* associating with the access point */
    NET_GETTING_IP,
    NET_READY,
    NET_FAILED,
};

/* Where the start-up steps are logged (webfetch.c's web.log). */
void net_set_log(void (*fn)(const char *fmt, ...));

/* Loads the network modules only (net_start loads them too). 0 or -1. */
int net_load_modules(void);

/* Loads the modules (if not yet) and starts the stack (once). Returns 0 or -1. */
int net_start(void);

/* The saved connections (Settings -> Network Settings), 1-based as the PSP
 * numbers them: ids[i] is the number of names[i]. Needs net_start(). */
int net_profiles(int *ids, char (*names)[64], int max);

/* Joins saved connection `id` (0: the one used last, else the first),
 * waiting up to 30 s. *cancel != 0 gives up. Returns 0 when connected. */
int net_connect(int id, volatile int *cancel);

int net_state(void);           /* NET_*, safe from any thread */
const char *net_step(void);    /* the start-up call running now (shown while starting) */
const char *net_error(void);   /* why the last net_connect failed */
const char *net_profile_name(void); /* the connection being used / last used */
int net_is_up(void);           /* connected right now (asks the PSP) */
void net_disconnect(void);
/* Before quitting or starting a game: drops the connection. */
void net_shutdown(void);

/* The connection to use first, remembered across starts (cache/web.txt). */
void net_set_preferred(int id);
int net_preferred(void);

#endif
