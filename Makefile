# XLT - PSP launcher (PSPSDK / pspdev)
TARGET = xlt
OBJS = src/main.o src/fatcount.o src/gfx.o src/ui.o src/input.o src/system.o src/library.o \
       src/disc.o src/probe.o src/thumbconv.o src/thumbs.o src/launch.o src/history.o \
       src/ark.o src/arktext.o src/sysctl.o src/gamecfg.o src/opts.o src/cc.o \
       src/match.o src/search.o src/plugtext.o src/plugins.o src/bg.o \
       src/imgdecode.o src/viewer.o src/id3.o src/music.o src/mp4.o src/readwin.o src/vqueue.o src/video.o src/vnext.o src/mbrowse.o src/h264strip.o src/mpegstub.o \
       src/delpath.o src/fsdel.o src/shotpng.o src/osd.o \
       src/arkcfg.o src/psreg.o src/settings.o \
       src/textenc.o src/pdf.o src/menu.o src/groups.o src/osk.o src/fileops.o src/reader.o src/fm.o src/preview.o \
       src/saver.o src/saverdraw.o src/boot.o src/bootdraw.o src/umd.o src/sfx.o src/theme.o \
       src/url.o src/html.o src/weblay.o src/net.o src/webfetch.o src/web.o src/clockfix.o \
       src/m3u.o src/musicsort.o src/playlists.o src/resume.o src/nowplaying.o src/ui_menus.o src/vplace.o src/srt.o \
       src/sfokey.o src/savedata.o src/pathmap.o src/lrc.o src/safestart.o src/zip.o src/flac.o src/audiofmt.o src/avi.o src/recjpeg.o src/recorder.o \
       src/shotplug.o src/arklauncher.o

INCDIR = src build
# -MMD -MP: the compiler lists every header each .o includes (src/*.d), so a changed
# header rebuilds everything that uses it. The hand-written lists below missed
# music.o for mp4.h once: music.c kept the old, smaller Mp4 inside its Aac and
# mp4_open_audio() overwrote the fields after it (M4A hung the PSP).
CFLAGS = -O2 -G0 -Wall -Wextra -Wno-unused-parameter -MMD -MP
ASFLAGS = $(CFLAGS)

LIBS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lgif -ljpeg -lpng -lz -llz4 -lm -lpspmpeg -lpspmpegbase -lpspaudiocodec -lpspmp3 -lpspaudio -lpspgu -lpsppower -lpspwlan -lpspsystemctrl_user -lpspkubridge -lpspusb -lpspusbstor -lpspumd -lpthread
# Not named here, though used: sceUtility, sceRtc, sceNetInet and sceNetResolver (linked by
# psp-gcc's specs) and sceNet / sceNetApctl (added by build.mak). Naming a stub library twice
# splits its import table: psp-fixup-imports warns "stubs out of order" and the PSP then
# fails to link or hangs while loading the network modules.

BUILD_PRX = 1
PSP_FW_VERSION = 660
# All the memory there is (PARAM.SFO MEMSIZE=1): the SDK's default now is MEMSIZE=2, "limited, for the
# Vita", which leaves XLT the 24 MB for applications unless ARK's high memory is on, and XLT (~7 MB) and
# its 18 MB heap don't fit in that: the heap can't be made and every allocation fails (an empty library).
# (build.mak warns that the flag "is not necessary": it is, as its own default is 2.)
PSP_LARGE_MEMORY = 1

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = XLT
PSP_EBOOT_ICON = art/ICON0.PNG
# While XLT is highlighted in the XMB: a screenshot behind it and a silent tour in place of
# the icon. Made from captures of XLT by tools/make_xmb_media.sh, and kept in git.
PSP_EBOOT_PIC1 = art/PIC1.PNG
PSP_EBOOT_ICON1 = art/ICON1.PMF

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
-include $(OBJS:.o=.d)
EXTRA_CLEAN += $(OBJS:.o=.d)

# The XMB icon: the logo on a tile like the launcher's (tools/make_logo.py), 144x80.
# The SVGs it draws from are build products, in build/.
art/ICON0.PNG: tools/make_logo.py
	@mkdir -p art build
	python3 -I tools/make_logo.py build
	rsvg-convert -w 144 -h 80 build/icon0.svg -o $@
EBOOT.PBP: art/ICON0.PNG art/PIC1.PNG art/ICON1.PMF

# The in-game screenshot plugin: a kernel PRX built on its own, carried inside XLT,
# which installs it at start-up (src/shotplug.c).
XLTSHOT_SRC = plugin/xltshot/main.c plugin/xltshot/pngout.c plugin/xltshot/pngout.h plugin/xltshot/Makefile
plugin/xltshot/xltshot.prx: $(XLTSHOT_SRC)
	$(MAKE) -C plugin/xltshot
build/xltshot_data.h: plugin/xltshot/xltshot.prx tools/bake_bin.py
	@mkdir -p build
	python3 -I tools/bake_bin.py $< xltshot_prx $@
src/shotplug.o: build/xltshot_data.h
EXTRA_CLEAN += build/xltshot_data.h plugin/xltshot/xltshot.prx plugin/xltshot/xltshot.elf plugin/xltshot/*.o

# The same shapes as a texture for the boot screen.
build/logo_data.h: tools/make_logo.py
	@mkdir -p build
	python3 -I tools/make_logo.py --tex $@
src/boot.o src/bootdraw.o: build/logo_data.h

# The interface sounds, synthesised (tools/bake_sounds.py); build/ui_sounds_demo.wav is to listen to on the PC.
build/sfx_data.h build/sfx_ids.h: tools/bake_sounds.py
	@mkdir -p build
	python3 -I tools/bake_sounds.py build/sfx_data.h build/ui_sounds_demo.wav
src/sfx.o src/ui.o src/boot.o src/main.o src/settings.o src/cc.o src/opts.o src/menu.o src/fm.o src/osk.o src/search.o src/viewer.o: build/sfx_ids.h

# The web browser: its icons (the atlas is full) and the root certificates it trusts.
build/webicon_data.h: tools/bake_webicons.py tools/bake_atlas.py
	@mkdir -p build
	python3 -I tools/bake_webicons.py $@ build/webicon_preview.png
build/cacerts_data.h: tools/bake_cacerts.py
	@mkdir -p build
	python3 -I tools/bake_cacerts.py $@
src/web.o src/ui.o: build/webicon_data.h src/web.h
src/webfetch.o: build/cacerts_data.h
src/web.o src/webfetch.o src/html.o src/weblay.o src/url.o src/net.o: src/webfetch.h src/html.h src/weblay.h src/url.h src/net.h
src/web.o src/osk.o: src/osk.h build/sfx_ids.h build/atlas_data.h src/ui_internal.h
src/imgdecode.o src/webfetch.o: src/imgdecode.h

# The atlas is baked offline from the fonts and icon painters in tools/.
build/atlas_data.h: tools/bake_atlas.py
	@mkdir -p build
	python3 -I tools/bake_atlas.py $@ build/atlas_preview.png

# The screensaver's clock face and glow, baked the same way.
build/saver_data.h: tools/bake_saver.py tools/bake_atlas.py
	@mkdir -p build
	python3 -I tools/bake_saver.py $@ build/saver_preview.png

src/saverdraw.o: build/saver_data.h build/atlas_data.h src/saverdraw.h src/gfx.h src/ui_internal.h
src/saver.o: build/atlas_data.h src/saver.h src/saverdraw.h src/gamecfg.h
src/ui.o src/main.o src/settings.o: src/saver.h
src/settings.o src/gamecfg.o src/saver.o: src/gamecfg.h

src/gfx.o src/ui.o src/main.o src/thumbs.o src/opts.o src/cc.o src/search.o src/viewer.o src/osd.o src/settings.o \
src/menu.o src/osk.o src/reader.o src/fm.o: build/atlas_data.h src/gfx.h
src/ui.o src/main.o src/cc.o src/fm.o src/reader.o src/menu.o src/osk.o: src/fm.h src/reader.h src/menu.h src/osk.h src/fileops.h
src/fm.o src/fileops.o: src/fileops.h
src/pdf.o src/reader.o: src/pdf.h src/textenc.h src/xio.h
src/textenc.o: src/textenc.h
src/ui.o src/main.o: src/history.h src/launch.h src/ui.h
src/ui.o src/opts.o src/cc.o src/search.o: src/ui_internal.h src/search.h src/match.h src/opts.h src/cc.h src/anim.h
src/opts.o src/cc.o src/main.o src/ark.o src/arktext.o: src/ark.h src/arktext.h src/gamecfg.h src/sysctl.h
src/gamecfg.o: src/gamecfg.h src/launch.h
src/opts.o src/plugins.o src/plugtext.o: src/plugins.h src/plugtext.h
src/bg.o src/opts.o src/cc.o src/main.o src/viewer.o: src/bg.h
src/ui.o src/viewer.o src/thumbconv.o src/imgdecode.o src/library.o src/opts.o: src/imgdecode.h src/viewer.h src/entry.h
src/ui.o src/cc.o src/main.o src/music.o: src/music.h
src/ui.o src/cc.o src/main.o src/video.o src/music.o src/preview.o: src/video.h
src/ui.o src/preview.o: src/preview.h src/disc.h
src/video.o src/mp4.o src/library.o: src/mp4.h
src/video.o src/vplace.o src/ui.o: src/vplace.h src/video.h
src/video.o src/srt.o: src/srt.h src/textenc.h
src/savedata.o src/sfokey.o: src/sfokey.h
src/viewer.o src/pathmap.o: src/pathmap.h
src/nowplaying.o src/lrc.o: src/lrc.h src/textenc.h src/id3.h
src/viewer.o src/main.o src/ui.o src/saver.o: src/viewer.h
src/savedata.o src/ui.o: src/savedata.h src/ui_internal.h src/menu.h src/fileops.h src/thumbs.h build/atlas_data.h build/sfx_ids.h
src/library.o src/id3.o: src/id3.h
# Music: playlists and the list's order.
src/m3u.o src/playlists.o: src/m3u.h src/textenc.h
src/musicsort.o src/library.o src/gamecfg.o src/opts.o src/ui.o src/playlists.o: src/musicsort.h src/entry.h
src/playlists.o src/ui.o: src/playlists.h src/library.h src/music.h
src/resume.o src/music.o src/nowplaying.o: src/resume.h
src/nowplaying.o src/ui.o src/cc.o src/main.o: src/nowplaying.h src/music.h src/ui_internal.h build/atlas_data.h build/sfx_ids.h src/gfx.h
src/music.o src/playlists.o src/nowplaying.o: src/bg.h
src/library.o src/gamecfg.o src/opts.o src/ui.o src/settings.o: src/gamecfg.h
src/opts.o src/ui.o: src/ui_internal.h
src/ui.o src/ui_menus.o: src/ui_state.h src/ui_internal.h src/playlists.h src/musicsort.h src/menu.h src/osk.h src/groups.h build/atlas_data.h build/sfx_ids.h
src/launch.o: src/launch.h src/disc.h src/entry.h
src/ui.o: src/anim.h src/ui.h src/library.h src/thumbs.h src/entry.h
src/library.o src/probe.o src/thumbs.o: src/entry.h
src/disc.o src/probe.o src/thumbconv.o: src/disc.h src/xio.h

test: build/atlas_data.h build/saver_data.h build/webicon_data.h build/sfx_ids.h
	tests/run.sh
.PHONY: test
