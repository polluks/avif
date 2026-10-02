/* recognition code for AVIF objects */

#define SYSTEM_PRIVATE

#include <string.h>
#include <proto/intuition.h>
#include <proto/multimedia.h>
#include <clib/alib_protos.h>

#include "class_version.h"
#include "avif.demuxer.h"

ULONG Recognize(struct DtCodeContext *dcc, ULONG recog_type);

const struct TagItem ClassTags[] = {
	{MMA_RecognizeCode, (ULONG)Recognize},
	{MMA_MediaType, MMT_PICTURE},
	{MMA_ClassType, MMCLASS_DEMUXER},
	{TAG_END, 0}
};

const struct TagItem* ClassAttributes(void)
{
	return ClassTags;
}

/* Brands we accept.  These are the brands libavif accepts (src/read.c,
 * avifCheckAVIFBrand), and parse_ftyp() in avif.demuxer.c applies exactly the
 * same rule, so a file the recogniser claims here is one the demuxer will
 * parse:
 *
 *   major brand    'avif', 'avis', 'avio'
 *   any compatible brand  'avif' or 'avis'
 *
 * A generic major brand such as 'mif1' or 'heic' only counts together with
 * 'avif' or 'avis' in the compatible list; on its own it is plain HEIF and
 * has no AV1 items.  'avio' is only ever a major brand. */

static BOOL brand_is_avif_major(const UBYTE *brand)
{
	return !memcmp(brand, "avif", 4) || !memcmp(brand, "avis", 4) ||
	       !memcmp(brand, "avio", 4);
}

static BOOL brand_is_avif_compatible(const UBYTE *brand)
{
	return !memcmp(brand, "avif", 4) || !memcmp(brand, "avis", 4);
}

ULONG Recognize(struct DtCodeContext *dcc, ULONG recog_type)
{
	LONG probability = 0;
	UBYTE header[64];
	ULONG bytes_read;

	bytes_read = DoMethod(dcc->dcc_Source, MMM_Peek, dcc->dcc_Port,
	                      (ULONG)header, sizeof(header));

	{
		/* 'ftyp' must be the first box: size + "ftyp" + major brand */

		if (bytes_read >= 16 && !memcmp(header + 4, "ftyp", 4))
		{
			ULONG box_size = ((ULONG)header[0] << 24) |
			                 ((ULONG)header[1] << 16) |
			                 ((ULONG)header[2] << 8)  |
			                  (ULONG)header[3];
			ULONG i, first_brand;
			BOOL brand_ok = brand_is_avif_major(header + 8);

			/*
			 * size == 1 means a 64 bit largesize follows the box
			 * type, so the payload starts eight bytes later.  The
			 * brand layout itself is unchanged: major brand, minor
			 * version, then 4 byte compatible brands.
			 */
			first_brand = 16;
			if (box_size == 1)
			{
				UQUAD large_size = 0;
				ULONG b;

				for (b = 0; b < 8; b++)
				{
					large_size = (large_size << 8) | header[8 + b];
				}
				box_size    = (ULONG)large_size;
				first_brand = 24;
				brand_ok    = FALSE;

				/*
				 * A 64 bit ftyp keeps its brands from byte 16 on.
				 * With less than 24 bytes in hand there is nothing
				 * to judge here, and taking the largesize for a
				 * brand would claim files the demuxer then
				 * refuses, so leave them to the parser.
				 */
				if (bytes_read >= 24)
				{
					brand_ok = brand_is_avif_major(header + 16);
				}
			}

			for (i = first_brand; !brand_ok && (i + 4) <= bytes_read; i += 4)
			{
				if (brand_is_avif_compatible(header + i)) brand_ok = TRUE;
			}

			if (brand_ok && box_size >= first_brand)
			{
				probability = 9500;

				/* Heuristic: stream length is plausible */

				{
					UQUAD stream_length = MediaGetPort64(dcc->dcc_Source,
					                                     dcc->dcc_Port,
					                                     MMA_StreamLength);

					if (stream_length >= box_size) probability += 500;
				}
			}
		}
	}

	DoMethod(dcc->dcc_Source, MMM_Restore);
	return probability;
}