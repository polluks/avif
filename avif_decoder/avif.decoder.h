#ifndef AVIF_DECODER_H
#define AVIF_DECODER_H

#include <exec/types.h>

/*
 * avif.decoder
 *   input  port 0 : MMF_VIDEO_AVIF  (AV1 elementary stream)
 *   output port 1 : MMFC_VIDEO_ARGB32
 */

#define MMF_VIDEO_AVIF (0x00002000 + 9)   /* same as avif.demuxer */

#endif /* AVIF_DECODER_H */