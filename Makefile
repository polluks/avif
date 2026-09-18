# $VER: makefile 1.0 (15.09.2026)

VERSION     =   1

CC = ppc-morphos-gcc
CFLAGS = -noixemul -nostartfiles -O2 -fomit-frame-pointer -mno-prototype -mcpu=604e -mmultiple
CFLAGS += -W -Wall -Wpointer-arith -Wno-parentheses
LD = ppc-morphos-gcc
LDFLAGS = -nostartfiles -noixemul
LIBS = -labox -lm -lmath
STRIP = ppc-morphos-strip --remove-section .comment
VER := $(shell grep "define VERS " class_version.h | cut -d " " -f 5 | tr -d "\042" )
OUTPUT = avif.demuxer
OBJS = avif.demuxer.o

#===============================================================================

.PHONY: all  clean install

all: $(OUTPUT)


clean:
	-rm -rf $(OBJS) *.bak *.s *.db $(OUTPUT) recognize

install: all
	mkdir -p /SYS/MorphOS/Classes/Multimedia
	cp $(OUTPUT) /SYS/MorphOS/Classes/Multimedia/
	-flushlib $(OUTPUT)

#===============================================================================

$(OUTPUT).db: $(OBJS) recognize
	$(LD) $(LDFLAGS) $(OBJS) $(LIBS) -o $(OUTPUT).db
	ppc-morphos-objcopy --add-section .dtcode=recognize $(OUTPUT).db
	ppc-morphos-objcopy --set-section-flags .dtcode=readonly $(OUTPUT).db

$(OUTPUT): $(OUTPUT).db
	$(STRIP) $(OUTPUT).db -o $(OUTPUT)

recognize.db: recognize.c class_version.h avif.demuxer.h
	ppc-morphos-gcc -s -O2 -nostdlib -o recognize.db recognize.c -labox

recognize: recognize.db
	ppc-morphos-objcopy -R .comment recognize.db recognize

#===============================================================================

avif.demuxer.o: avif.demuxer.c avif.demuxer.h class_version.h
	$(CC) $(CFLAGS) -c $<
