include ../../config.mak

vpath %.c $(SRC_PATH)/applications/mp4box

CFLAGS=-I"$(SRC_PATH)/include" $(OPTFLAGS)

ifeq ($(DEBUGBUILD),yes)
CFLAGS+=-g
LDFLAGS+=-g
endif

ifeq ($(GPROFBUILD),yes)
ifeq ($(CONFIG_DARWIN),yes)
CFLAGS+=-fprofile-instr-generate
LDFLAGS+=-fprofile-instr-generate
else
CFLAGS+=-pg
LDFLAGS+=-pg
endif
endif

LINKFLAGS=-L../../bin/gcc -L../../extra_lib/lib/gcc
ifeq ($(CONFIG_EMSCRIPTEN),yes)
else ifeq ($(CONFIG_DARWIN),yes)
#LINKFLAGS+= -Wl,-rpath,'@loader_path'
else
LINKFLAGS+= -Wl,-rpath,'$$ORIGIN' -Wl,-rpath,$(prefix)/lib -Wl,-rpath-link,../../bin/gcc
endif

ifeq ($(STATIC_BUILD),yes)
##include static modules and other deps for libgpac
include ../../static.mak
ifneq ($(PKG_CONFIG), no)
LINKFLAGS+=$(shell $(PKG_CONFIG) ../../gpac.pc --libs --static | sed 's/-lgpac //'  )
else
LINKFLAGS+= -lgpac_static
endif
LINKFLAGS+= $(GPAC_SH_FLAGS)
LINKFLAGS+=$(zlib_ldflags) $(opensvc_ldflags) $(ssl_ldflags) $(jpeg_ldflags) $(openjpeg_ldflags) $(png_ldflags) $(mad_ldflags) $(a52_ldflags) $(xvid_ldflags) $(faad_ldflags)
LINKFLAGS+=$(ffmpeg_ldflags) $(ogg_ldflags) $(vorbis_ldflags) $(theora_ldflags) $(nghttp2_ldflags) $(vtb_ldflags)
LINKFLAGS+=$(EXTRALIBS)
else
LINKFLAGS+=-lgpac
endif


ifeq ($(CONFIG_WIN32),yes)
EXE=.exe
PROG=MP4Box$(EXE)
LINKFLAGS+=$(UNICODEFLAGS)

ifeq ($(HAS_WMAIN),no)
CFLAGS+=-DNO_WMAIN
endif

else

EXT=
PROG=MP4Box

ifeq ($(STATIC_BINARY),yes)
LINKFLAGS+= $(GPAC_SH_FLAGS)
endif

endif

#common obj
OBJS=mp4box.o filedump.o fileimport.o
ifeq ($(DISABLE_STREAMING),no)
OBJS+=live.o
endif



SRCS := $(OBJS:.o=.c)

ifeq ($(CONFIG_WIN32),yes)
OBJS+=$(SRC_PATH)/manifest.o
endif

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) -o ../../bin/gcc/$@ $(OBJS) $(LINKFLAGS) $(LDFLAGS)

clean:
	rm -f $(OBJS) ../../bin/gcc/$(PROG)

dep: depend

depend:
	rm -f .depend
	$(CC) -MM $(CFLAGS) $(SRCS) 1>.depend

distclean: clean
	rm -f Makefile.bak .depend

-include .depend
