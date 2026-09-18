#ifndef AVIF_DEMUXER_H
#define AVIF_DEMUXER_H

#include <exec/types.h>

#ifndef MMA_VIDEOMASK
#define MMA_VIDEOMASK 0x00002000
#endif

/*
 * Reggae format identifiers for the AVIF plugin pair.
 *
 * avif.demuxer  output port 1 : MMF_VIDEO_AVIF  (AV1 elementary stream,
 *                                                AVIF container)
 * avif.decoder  input  port 0 : MMF_VIDEO_AVIF
 *               output port 1 : MMFC_VIDEO_ARGB32
 */

#define MMF_VIDEO_AVIF (MMA_VIDEOMASK + 9)
#define MMF_VIDEO_AV1  (MMA_VIDEOMASK + 10)

/*
 * Data handed to the decoder via MMA_ExtraData (Get on input port).
 * The elementary stream (AV1 OBU byte stream) is produced by the demuxer
 * with the av1C sequence header OBU prepended when the stored item itself
 * does not start with a sequence header.
 */
struct AvifInfo
{
	ULONG ai_Width;            /* image width  in pixels   */
	ULONG ai_Height;           /* image height in pixels   */
	ULONG ai_BitsPerPixel;     /* luma bit depth 8/10/12   */
	ULONG ai_UseAlpha;         /* TRUE = alpha channel     */
	ULONG ai_AlphaPremul;      /* TRUE = alpha premultiplied (no alpha item) */
	ULONG ai_Monochrome;       /* TRUE = single plane      */
	ULONG ai_ChromaSubX;       /* chroma subsampling x     */
	ULONG ai_ChromaSubY;       /* chroma subsampling y     */
	UBYTE *ai_ES;              /* AV1 elementary stream    */
	ULONG ai_ESLength;         /* length in bytes          */
	UBYTE ai_Av1c[32];         /* av1C codec config record */
	ULONG ai_Av1cLength;
};

/* Metadata identifiers for MMA_MetaData. */

#define AVIF_META_TITLE        0x01
#define AVIF_META_COPYRIGHT    0x02
#define AVIF_META_DESCRIPTION  0x03

#define AVIF_ALPHA_URN         "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha"

#endif /* AVIF_DEMUXER_H */