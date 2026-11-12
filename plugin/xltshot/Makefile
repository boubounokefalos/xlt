# xltshot: the in-game screenshot plugin (a kernel PRX ARK loads at the "game"
# runlevel). XLT's own Makefile builds it and embeds it; XLT installs it.
TARGET = xltshot
OBJS = main.o pngout.o

BUILD_PRX = 1
PSP_FW_VERSION = 660
USE_KERNEL_LIBS = 1

CFLAGS = -Os -G0 -Wall -Wextra -Wno-unused-parameter
ASFLAGS = $(CFLAGS)
# the 6.60 versions of the kernel libraries (their function IDs changed in 6.60), ahead of build.mak's -lpspkernel
LIBS = -lz -lpspsystemctrl_kernel -lpspsysclib_kernel_660 -lpspthreadman_kernel_660 -lpspsysmem_kernel_660 -lpsploadcore_kernel_660

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
