# $VER: Makefile 1.0 (15.09.2026)
#
# Builds libdav1d.a (scalar C, no asm) for the avif.decoder plugin.
# dav1d 1.5.4 vendored subset: portable C99 sources only (see ../README).
#
# This Makefile is invoked from the parent avif_decoder Makefile, or directly.

CC ?= gcc
# -DNDEBUG compiles out dav1d's internal assertions, the way upstream release
# builds do: a damaged stream must return an error, never abort the plugin.
# The parent avif_decoder Makefile passes the same flag down through CFLAGS.
CFLAGS ?= -O2 -fomit-frame-pointer -DNDEBUG
# MorphOS cross build: override above on the command line, e.g.
#   make CC=ppc-morphos-gcc CFLAGS="-noixemul -nostartfiles -O2 -mcpu=604e -fomit-frame-pointer -DNDEBUG"

ARCH_CFLAGS = -DHAVE_ASM=0
# Host (little-endian x86) test builds must override endianness:
ENDIAN_FLAG =
# For MorphOS/PPC (big-endian) keep the default ENDIANNESS_BIG=1 from config.h.
# For host tests use:  make ENDIAN_FLAG=-UENDIANNESS_BIG

CPPFLAGS = $(ARCH_CFLAGS) $(ENDIAN_FLAG) \
	-I. -Iinclude -Icompat/gcc

# non-bitdepth sources (compiled once)
SRCS = \
	src/cdf.c \
	src/cpu.c \
	src/ctx.c \
	src/data.c \
	src/decode.c \
	src/dequant_tables.c \
	src/getbits.c \
	src/intra_edge.c \
	src/itx_1d.c \
	src/lf_mask.c \
	src/lib.c \
	src/log.c \
	src/mem.c \
	src/msac.c \
	src/obu.c \
	src/pal.c \
	src/picture.c \
	src/qm.c \
	src/ref.c \
	src/refmvs.c \
	src/scan.c \
	src/tables.c \
	src/thread_task.c \
	src/warpmv.c \
	src/wedge.c

# bitdepth sources (compiled once per bitdepth: 8 and 16)
TMPL_SRCS = \
	src/cdef_apply_tmpl.c \
	src/cdef_tmpl.c \
	src/fg_apply_tmpl.c \
	src/filmgrain_tmpl.c \
	src/ipred_prepare_tmpl.c \
	src/ipred_tmpl.c \
	src/itx_tmpl.c \
	src/lf_apply_tmpl.c \
	src/loopfilter_tmpl.c \
	src/looprestoration_tmpl.c \
	src/lr_apply_tmpl.c \
	src/mc_tmpl.c \
	src/recon_tmpl.c

OBJS = $(SRCS:.c=.o) $(TMPL_SRCS:src/%.c=build/%.8.o) $(TMPL_SRCS:src/%.c=build/%.16.o)

TMPL8  = $(TMPL_SRCS:src/%.c=build/%.8.o)
TMPL16 = $(TMPL_SRCS:src/%.c=build/%.16.o)

.PHONY: all clean

all: libdav1d.a

libdav1d.a: $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

build/%.8.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBITDEPTH=8 -c $< -o $@

build/%.16.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBITDEPTH=16 -c $< -o $@

clean:
	rm -f libdav1d.a src/*.o build/*.o
	rm -rf build