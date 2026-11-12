/* XLT - the C library's clock. pspdev's libcglue takes gettimeofday() (and so time())
 * from sceKernelLibcGettimeofday, which on a PSP with ARK returned ~8 hours after
 * 1970: mbedTLS then took every certificate as "valid from the future". This one
 * takes the real-time clock, the date set in the PSP's settings, in UTC. Linked
 * before libcglue, it replaces that library's _gettimeofday. */
#include <psprtc.h>
#include <sys/time.h>
#include <errno.h>

#define UNIX_EPOCH_S 62135596800ULL /* 0001-01-01 to 1970-01-01, in seconds */

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    u64 tick;
    if (sceRtcGetCurrentTick(&tick) < 0) { /* microseconds since 0001-01-01, UTC */
        errno = EIO;
        return -1;
    }
    if (tv) {
        u64 s = tick / 1000000ULL;
        tv->tv_sec = (time_t)(s > UNIX_EPOCH_S ? s - UNIX_EPOCH_S : 0);
        tv->tv_usec = (suseconds_t)(tick % 1000000ULL);
    }
    return 0;
}
