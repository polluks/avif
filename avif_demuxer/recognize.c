/* recognition code for AVIF objects */

#define SYSTEM_PRIVATE

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

ULONG Recognize(struct DtCodeContext *dcc, ULONG recog_type)
{
	struct Library *IntuitionBase = dcc->dcc_IntuitionBase;
	struct Library *MultimediaBase = dcc->dcc_MultimediaBase;
	LONG probability = 0;
	UBYTE header[12];

	if (DoMethod(dcc->dcc_Source, MMM_Peek, dcc->dcc_Port, (ULONG)header, 12) == 12)
	{
		/* Check for 'ftyp' box header: size + "ftyp" + major brand */

		if (header[4] == 'f' && header[5] == 't' && header[6] == 'y' && header[7] == 'p')
		{
			/* Accept 'avif', 'avis', 'avio', 'mif1' major brands */

			if ((header[8]  == 'a' && header[9]  == 'v' && header[10] == 'i' && header[11] == 'f') ||
			    (header[8]  == 'a' && header[9]  == 'v' && header[10] == 'i' && header[11] == 's') ||
			    (header[8]  == 'a' && header[9]  == 'v' && header[10] == 'i' && header[11] == 'o') ||
			    (header[8]  == 'm' && header[9]  == 'i' && header[10] == 'f' && header[11] == '1'))
			{
				probability = 9500;

				/* Heuristic: check stream length is plausible (not zero, not tiny) */

				{
					UQUAD stream_length = MediaGetPort64(dcc->dcc_Source, dcc->dcc_Port, MMA_StreamLength);
					if (stream_length > 0 && stream_length > 12)
						probability += 500;
				}
			}
		}
	}

	DoMethod(dcc->dcc_Source, MMM_Restore);
	return probability;
}