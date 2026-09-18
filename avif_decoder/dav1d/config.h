/*
 * dav1d config.h - hand-written for MorphOS/PPC32 scalar C build.
 * Replaces meson-generated config.h.
 * For host test builds, compile with -UENDIANNESS_BIG to override.
 */
#ifndef DAV1D_SRC_CONFIG_H
#define DAV1D_SRC_CONFIG_H

/* Both bitdepths: 8bpc covers 8-bit AVIF, 16bpc covers 10/12-bit AVIF */
#define CONFIG_8BPC    1
#define CONFIG_16BPC   1

/* No assembly - PPC32 scalar only */
#define HAVE_ASM       0

/* MorphOS is big-endian PPC */
#ifndef ENDIANNESS_BIG
#define ENDIANNESS_BIG 1
#endif

/* No logging inside the decoder library */
#define CONFIG_LOG     0

/* Keep full DSP tables (do not trim) */
#define TRIM_DSP_FUNCTIONS 0

/* Architecture: none of the supported asm targets */
#define ARCH_AARCH64     0
#define ARCH_ARM         0
#define ARCH_LOONGARCH   0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define ARCH_PPC64LE     0
#define ARCH_RISCV       0
#define ARCH_RV32        0
#define ARCH_RV64        0
#define ARCH_X86         0
#define ARCH_X86_64      0
#define ARCH_X86_32      0

/* Memory: MorphOS has malloc but no POSIX alignment helpers */
#define HAVE_POSIX_MEMALIGN 0
#define HAVE_MEMALIGN       0
#define HAVE_ALIGNED_ALLOC  0

/* Clocks / signals */
#define HAVE_CLOCK_GETTIME 0
#define HAVE_SIGACTION     0

/* dlsym not available (no shared library loader) */
#define HAVE_DLSYM 0

/* pthreads - MorphOS has POSIX threads */
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP     0
#define HAVE_PTHREAD_SET_NAME_NP    0
#define HAVE_PTHREAD_NP_H           0

/* ELF auxval not applicable on PPC32 MorphOS */
#define HAVE_GETAUXVAL     0
#define HAVE_ELF_AUX_INFO  0

/* System headers */
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H    1
#define HAVE_IO_H        0

/* No prefix in symbol names */
#define PREFIX 0

#endif /* DAV1D_SRC_CONFIG_H */