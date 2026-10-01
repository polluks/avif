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

/* Brands we accept.  A file is recognised when its major brand is one of
 * these, or when any of its compatible brands is one of these.  The second
 * case matters because encoders may use a generic major brand such as 'mif1'
 * and only list 'avif' in the compatible brands. */

static BOOL brand_is_avif(const UBYTE *brand)
{
	return !memcmp(brand, "avif", 4) || !memcmp(brand, "avis", 4) ||
	       !memcmp(brand, "avio", 4) || !memcmp(brand, "mif1", 4);
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
			ULONG i;
			BOOL major_ok = brand_is_avif(header + 8);

			/*
			 * Walk the compatible brand list: major brand, minor
			 * version, then 4 byte brands.  A 64 bit large size is
			 * accepted here as well; the brand layout is unchanged.
			 */
			for (i = 16; !major_ok && (i + 4) <= bytes_read; i += 4)
			{
				if (brand_is_avif(header + i)) major_ok = TRUE;
			}

			if (major_ok && box_size >= 16)
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