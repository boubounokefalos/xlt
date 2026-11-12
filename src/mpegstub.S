# XLT - sceMpeg functions that pspdev's libpspmpeg has no stubs for.
# NIDs from PPSSPP's sceMpeg table; exported by flash0:/kd/mpeg_vsh.prx,
# which video.c loads before using them.
	.set noreorder

#include "pspstub.s"

	STUB_START "sceMpeg",0x00090000,0x00010005
	STUB_FUNC  0x11F95CF1,sceMpegGetAvcNalAu
	STUB_END
