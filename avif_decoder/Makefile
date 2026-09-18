# $VER: makefile 1.1 (15.09.2026)

CC = ppc-morphos-gcc
CFLAGS = -noixemul -nostartfiles -O2 -fomit-frame-pointer -mcpu=604e -mmultiple
CFLAGS += -W -Wall -Wno-parentheses
LD = ppc-morphos-gcc
LDFLAGS = -nostartfiles -noixemul
LIBS = -labox -lm -lmath -lpthread
STRIP = ppc-morphos-strip --remove-section .comment
OUTPUT = avif.decoder
OBJS = avif.decoder.o libdav1d.a
BLOB = data

DAV1D = dav1d
DAV1D_AR = $(DAV1D)/libdav1d.a

#===============================================================================

.PHONY: all clean install dav1d

all: $(OUTPUT)

dav1d: $(DAV1D_AR)

$(DAV1D_AR):
	$(MAKE) -C $(DAV1D) CC=$(CC) CFLAGS="$(CFLAGS)" libdav1d.a

clean:
	-rm -f *.o *.db $(OUTPUT) $(BLOB) $(BLOB).o
	$(MAKE) -C $(DAV1D) clean 2>/dev/null || true

install: all
	mkdir -p /SYS/MorphOS/Classes/Multimedia
	cp $(OUTPUT) /SYS/MorphOS/Classes/Multimedia/
	-flushlib $(OUTPUT)

#===============================================================================

$(OUTPUT).db: $(OBJS) $(BLOB)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $(OUTPUT).db
	ppc-morphos-objcopy --add-section .dtcode=$(BLOB) $(OUTPUT).db
	ppc-morphos-objcopy --set-section-flags .dtcode=readonly $(OUTPUT).db

$(OUTPUT): $(OUTPUT).db
	$(STRIP) $(OUTPUT).db -o $(OUTPUT)

$(BLOB): $(BLOB).o
	ppc-morphos-objcopy -R .comment $(BLOB).o $(BLOB)

$(BLOB).o: data.c avif.decoder.h
	$(CC) -s -O2 -nostdlib -o $@ -c data.c -labox

#===============================================================================

avif.decoder.o: avif.decoder.c avif.decoder.h ../avif_demuxer/avif.demuxer.h class_version.h $(DAV1D_AR)
	$(CC) $(CFLAGS) -I$(DAV1D) -I$(DAV1D)/include -c $<