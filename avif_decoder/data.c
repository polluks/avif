#define SYSTEM_PRIVATE

#include <proto/intuition.h>
#include <clib/alib_protos.h>
#include <classes/multimedia/multimedia.h>
#include <classes/multimedia/video.h>

#include "avif.decoder.h"


const LONG FmtTable0[] = {
  MMF_VIDEO_AVIF,
  0
};


const struct TagItem ClassTags[] = {
  {MMA_ClassType,               MMCLASS_DECODER},
  {MMA_MediaType,               MMT_PICTURE},
  {MMA_SupportedFormats,        (ULONG)FmtTable0},
  {TAG_END, 0}
};


const struct TagItem* ClassAttributes(void)
{
  return ClassTags;
}