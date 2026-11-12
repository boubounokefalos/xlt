/* XLT - system registry access. See psreg.h. */
#include <pspkernel.h>
#include <pspreg.h>
#include <kubridge.h>
#include <systemctrl.h>
#include <stdio.h>
#include <string.h>
#include "psreg.h"
#include "ark.h"

/* User-library NIDs of "sceReg" (from the SDK's libpspreg stubs); the
 * functions themselves run in the kernel, so they are called through kubridge. */
#define NID_OPEN_REG   0x92E41280
#define NID_CLOSE_REG  0xFA8A5739
#define NID_FLUSH_REG  0x39461B4D
#define NID_OPEN_CAT   0x1D8A762E
#define NID_CLOSE_CAT  0x0CAE832B
#define NID_FLUSH_CAT  0x0D69BF40
#define NID_SET_VALUE  0x17768E14
#define NID_INFO_NAME  0xC5768D02
#define NID_VALUE_NAME 0x30BE0259

static void *f_open_reg, *f_close_reg, *f_flush_reg, *f_open_cat, *f_close_cat, *f_flush_cat;
static void *f_set, *f_info, *f_get;

static void *find(unsigned nid)
{
    void *f = (void *)sctrlHENFindFunction("sceRegistry_Service", "sceReg", nid);
    if (!f)
        f = (void *)sctrlHENFindFunction("sceRegistry_Service", "sceReg_driver", nid);
    return f;
}

void psreg_init(void)
{
    if (!ark_version())
        return;
    f_open_reg = find(NID_OPEN_REG);
    f_close_reg = find(NID_CLOSE_REG);
    f_flush_reg = find(NID_FLUSH_REG);
    f_open_cat = find(NID_OPEN_CAT);
    f_close_cat = find(NID_CLOSE_CAT);
    f_flush_cat = find(NID_FLUSH_CAT);
    f_set = find(NID_SET_VALUE);
    f_info = find(NID_INFO_NAME);
    f_get = find(NID_VALUE_NAME);
}

int psreg_available(void)
{
    return f_open_reg && f_close_reg && f_open_cat && f_close_cat && f_info && f_get;
}

static int call(void *f, u32 a1, u32 a2, u32 a3, u32 a4)
{
    KernelCallArg a;
    memset(&a, 0, sizeof(a));
    a.arg1 = a1;
    a.arg2 = a2;
    a.arg3 = a3;
    a.arg4 = a4;
    if (!f || kuKernelCall(f, &a) < 0)
        return -1;
    return (int)a.ret1;
}

/* Opens dir; returns 0 and both handles. */
static int open_dir(const char *dir, REGHANDLE *h, REGHANDLE *hd)
{
    static struct RegParam reg; /* static: the kernel reads it through the pointer */
    memset(&reg, 0, sizeof(reg));
    reg.regtype = 1;
    strcpy(reg.name, "/system");
    reg.namelen = strlen(reg.name);
    reg.unk2 = reg.unk3 = 1;
    if (call(f_open_reg, (u32)&reg, 2, (u32)h, 0) < 0)
        return -1;
    if (call(f_open_cat, *h, (u32)dir, 2, (u32)hd) < 0) {
        call(f_close_reg, *h, 0, 0, 0);
        return -1;
    }
    return 0;
}

static void close_dir(REGHANDLE h, REGHANDLE hd, int flush)
{
    if (flush) call(f_flush_cat, hd, 0, 0, 0);
    call(f_close_cat, hd, 0, 0, 0);
    if (flush) call(f_flush_reg, h, 0, 0, 0);
    call(f_close_reg, h, 0, 0, 0);
}

static int get_key(const char *dir, const char *key, unsigned want_type, void *buf, unsigned size)
{
    if (!psreg_available())
        return -1;
    REGHANDLE h, hd;
    if (open_dir(dir, &h, &hd) < 0)
        return -1;
    static unsigned type;
    static SceSize len;
    int ok = call(f_info, hd, (u32)key, (u32)&type, (u32)&len) >= 0 && type == want_type && len <= size &&
             call(f_get, hd, (u32)key, (u32)buf, len) >= 0;
    close_dir(h, hd, 0);
    return ok ? 0 : -1;
}

int psreg_get_int(const char *dir, const char *key, int *value)
{
    static int v;
    if (get_key(dir, key, REG_TYPE_INT, &v, sizeof(v)) < 0)
        return -1;
    *value = v;
    return 0;
}

int psreg_get_str(const char *dir, const char *key, char *out, int n)
{
    static char buf[128];
    memset(buf, 0, sizeof(buf));
    if (get_key(dir, key, REG_TYPE_STR, buf, sizeof(buf) - 1) < 0)
        return -1;
    snprintf(out, n, "%s", buf);
    return 0;
}

int psreg_set_int(const char *dir, const char *key, int value)
{
    int cur;
    if (!f_set || !f_flush_cat || !f_flush_reg || psreg_get_int(dir, key, &cur) < 0)
        return -1; /* only keys that already exist, with an int value */
    if (cur == value)
        return 0;
    REGHANDLE h, hd;
    if (open_dir(dir, &h, &hd) < 0)
        return -1;
    static int v;
    v = value;
    int r = call(f_set, hd, (u32)key, (u32)&v, sizeof(v));
    close_dir(h, hd, 1);
    return r >= 0 && psreg_get_int(dir, key, &cur) == 0 && cur == value ? 0 : -1;
}
