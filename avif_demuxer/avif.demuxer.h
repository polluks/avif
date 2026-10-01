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
 *
 * The elementary streams are the AV1 OBU byte streams stored in the item
 * extents, verbatim.  av1C is an AV1CodecConfigurationRecord, not an OBU,
 * so it is only passed on for informational purposes and is never prepended
 * to ai_ES.
 *
 * ai_AlphaES is the separately coded alpha plane (AVIF alpha auxiliary
 * image item) and is NULL when the image has no alpha channel.
 */
struct AvifInfo
{
	ULONG ai_Width;            /* image width  in pixels   */
	ULONG ai_Height;           /* image height in pixels   */
	ULONG ai_BitsPerPixel;     /* luma bit depth 8/10/12   */
	ULONG ai_UseAlpha;         /* TRUE = alpha channel     */
	ULONG ai_AlphaPremul;      /* TRUE = alpha premultiplied in the colour planes */
	ULONG ai_Monochrome;       /* TRUE = single plane      */
	ULONG ai_ChromaSubX;       /* chroma subsampling x     */
	ULONG ai_ChromaSubY;       /* chroma subsampling y     */
	UBYTE *ai_ES;              /* colour plane AV1 stream  */
	ULONG ai_ESLength;         /* length in bytes          */
	UBYTE *ai_AlphaES;         /* alpha plane AV1 stream, or NULL */
	ULONG ai_AlphaESLength;    /* length in bytes          */
	ULONG ai_HaveNclx;         /* TRUE = colour fields below are valid */
	ULONG ai_ColourPrimaries;         /* ITU-T H.273 code point  */
	ULONG ai_TransferCharacteristics; /* ITU-T H.273 code point  */
	ULONG ai_MatrixCoefficients;      /* ITU-T H.273 code point  */
	ULONG ai_FullRangeFlag;    /* TRUE = full range        */
	UBYTE ai_Av1c[32];         /* av1C codec config record */
	ULONG ai_Av1cLength;
};

/*
 * Metadata.
 *
 * MMA_MetaData's storage contract is not visible from this repository, so the
 * demuxer does not guess at it.  Instead the strings extracted from the XMP
 * payload are exposed through the private attribute AVIF_META_DATA below,
 * which hands out a pointer to a struct AvifMetaData.  A higher level
 * consumer can copy whatever it needs from that.
 *
 * The block id 0x6176 ('av') is chosen to stay clear of the MMA_ blocks.
 */

#ifndef MM_TAG
#define MM_TAG(block, id) ((((ULONG)(block)) << 16) | (ULONG)(id))
#endif

#define AVIF_META_DATA     MM_TAG(0x6176, 1)

/* entry identifiers used in struct AvifMetaEntry */
#define AVIF_META_TITLE        0x01
#define AVIF_META_COPYRIGHT    0x02
#define AVIF_META_DESCRIPTION  0x03

#define AVIF_MAX_META_ENTRIES   8
#define AVIF_MAX_STRING_LEN   256

struct AvifMetaEntry
{
	ULONG          ame_Tag;
	CONST UBYTE   *ame_String; /* NUL terminated, owned by the demuxer */
};

struct AvifMetaData
{
	struct AvifMetaEntry amd_Entries[AVIF_MAX_META_ENTRIES];
	ULONG                amd_Count;
};

#define AVIF_ALPHA_URN         "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha"

#endif /* AVIF_DEMUXER_H */