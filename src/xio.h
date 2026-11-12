/* XLT - tiny file I/O layer so the parsers also build on a PC for tests. */
#ifndef XIO_H
#define XIO_H

#include <stdint.h>

#ifdef __PSP__
#include <pspiofilemgr.h>
static inline int xio_open(const char *path) { return sceIoOpen(path, PSP_O_RDONLY, 0); }
static inline int xio_read_at(int fd, void *buf, uint32_t size, uint64_t off)
{
    if (sceIoLseek(fd, (SceOff)off, PSP_SEEK_SET) != (SceOff)off)
        return -1;
    return sceIoRead(fd, buf, size);
}
static inline void xio_close(int fd) { sceIoClose(fd); }
static inline int64_t xio_size(int fd) { return (int64_t)sceIoLseek(fd, 0, PSP_SEEK_END); }
#else
#include <fcntl.h>
#include <unistd.h>
static inline int xio_open(const char *path) { return open(path, O_RDONLY); }
static inline int xio_read_at(int fd, void *buf, uint32_t size, uint64_t off)
{
    return (int)pread(fd, buf, size, (off_t)off);
}
static inline void xio_close(int fd) { close(fd); }
static inline int64_t xio_size(int fd) { return (int64_t)lseek(fd, 0, SEEK_END); }
#endif

#endif
