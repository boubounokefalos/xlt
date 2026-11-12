/* Host stand-ins for the PSP SDK headers, for tests/saver_preview.c only. */
#ifndef HOST_PSPTYPES_H
#define HOST_PSPTYPES_H
#include <stdint.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef uint64_t u64;
typedef int32_t s32;
typedef struct { int x, y, z, w; } ScePspIVector4;
typedef struct { ScePspIVector4 x, y, z, w; } ScePspIMatrix4;
#endif
