# XLTNET: XLT's Wi-Fi start on its own (see main.c). make, then copy EBOOT.PBP to PSP/GAME/XLTNET/.
TARGET = xltnet
OBJS = main.o net.o
INCDIR = ../../src
CFLAGS = -O2 -G0 -Wall
LIBS = -lpspwlan # the rest comes from psp-gcc's specs and build.mak: naming them splits the stubs
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = XLTNET test
BUILD_PRX = 1
PSP_FW_VERSION = 660
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
net.o: ../../src/net.c ../../src/net.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -I../../src -I$(PSPSDK)/include -c -o $@ $<
