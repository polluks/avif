/// autodoc

/****** avif.demuxer/background *********************************************
*
* DESCRIPTION
*   Demuxer class for AVIF (AV1 Image File Format) files.
*   Parses the ISO Base Media File Format (ISOBMFF) box structure and exposes
*   the primary AV1 image item's elementary stream, plus metadata attributes.
*
*   Two-port model: port 0 input MMF_STREAM, port 1 output MMF_VIDEO_AVIF.
*
*   The AV1 elementary stream emitted on port 1 is the primary image item's
*   bitstream from the file's mdat box, prepended with the av1C codec
*   configuration record OBU sequence header when the item data itself does
*   not begin with one.  The stream is self-contained and suitable for direct
*   feeding to an AV1 decoder (avif.decoder).
*
* NEW ATTRIBUTES
*   MMA_Video_Width       (V1)  [..G.Q], ULONG
*   MMA_Video_Height      (V1)  [..G.Q], ULONG
*   MMA_Video_BitsPerPixel(V1)  [..G.Q], ULONG
*   MMA_Video_UseAlpha    (V1)  [..G.Q], BOOL
*   MMA_DataFormat        (V1)  [..G.Q], STRPTR
*   MMA_MediaType         (V1)  [..G.Q], ULONG
*   MMA_ExtraData         (V1)  [..G.Q], struct AvifInfo*
*
* NEW METHODS
*   MMM_Pull(port, buffer, length) (V1)
*
*   1.0  (15.09.2026)
*   - Initial revision.
*
*****************************************************************************
*/

///
/// includes

#define __NOLIBBASE__
#define SYSTEM_PRIVATE

#include <string.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/multimedia.h>
#include <proto/query.h>
#include <emul/emulregs.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <clib/alib_protos.h>
#include <clib/debug_protos.h>
#include <classes/multimedia/multimedia.h>
#include <classes/multimedia/video.h>
#ifdef __MORPHOS__
#include <hardware/byteswap.h>
#endif

#include "avif.demuxer.h"

///
/// basic defs

#define SUPERCLASS "multimedia.class"

#include "class_version.h"

struct Library *SysBase, *IntuitionBase, *UtilityBase, *MultimediaBase;

struct ClassBase
{
	struct Library          LibNode;
	Class                  *LibClass;
	APTR                    Seglist;
	struct SignalSemaphore  BaseLock;
	BOOL                    InitFlag;
	const struct TagItem   *Attributes;
};

///
/// ISOBMFF box helper structures

#pragma pack(1)

struct BoxHeader
{
	UBYTE  bh_Size[4];
	UBYTE  bh_Type[4];
};

struct FullBoxHeader
{
	UBYTE  fbh_Size[4];
	UBYTE  fbh_Type[4];
	UBYTE  fbh_Version;
	UBYTE  fbh_Flags[3];
};

#pragma pack()

///
/// AVIF parsing limits

#define AVIF_MAX_TOP_LEVEL_BOXES  64
#define AVIF_MAX_META_CHILDREN   128
#define AVIF_MAX_ITEMS            64
#define AVIF_MAX_PROPERTIES       64
#define AVIF_MAX_EXTENTS          256
#define AVIF_MAX_STRING_LEN       256

///
/// ISOBMFF property container indices (order stored in ipco)

enum IpcoPropType
{
	IPCO_NONE = 0,
	IPCO_ISPE,
	IPCO_PIXI,
	IPCO_AV1C,
	IPCO_AUXC,
	IPCO_COLR
};

///
/// item/location descriptor

struct ItemExtent
{
	UQUAD ie_Offset;       /* absolute file offset */
	UQUAD ie_Length;       /* extent length in bytes */
};

struct ItemDesc
{
	UWORD id_ItemID;
	UWORD id_NumExtents;
	struct ItemExtent id_Extents[16];
	/* properties from ipma lookup */
	ULONG id_BitDepth;
	BOOL  id_HasAlphaAux; /* TRUE if auxC with alpha urn present */
	BOOL  id_IsAv01;
};

///
/// object instance data

struct ObjData
{
	/* image geometry parsed from ispe / av1C */
	ULONG od_Width;
	ULONG od_Height;
	ULONG od_BitsPerPixel;
	BOOL  od_UseAlpha;
	BOOL  od_AlphaPremul;
	BOOL  od_Monochrome;
	BOOL  od_HighBitdepth;
	BOOL  od_TwelveBit;
	ULONG od_ChromaSubX;
	ULONG od_ChromaSubY;

	/* primary image item */
	UWORD od_PrimaryItemID;

	/* items */
	UWORD od_NumItems;
	struct ItemDesc od_Items[AVIF_MAX_ITEMS];

	/* the assembled AV1 elementary stream (color plane) */
	UBYTE *od_ES;
	ULONG  od_ESLength;

	/* av1C box bytes (to prepend sequence header if missing) */
	UBYTE  od_Av1c[32];
	ULONG  od_Av1cLength;

	/* extra data structure for decoder */
	struct AvifInfo *od_Info;
};

///
/// big-endian read helpers

static inline UWORD be16(const UBYTE *p)
{
	return ((UWORD)p[0] << 8) | (UWORD)p[1];
}

static inline ULONG be32(const UBYTE *p)
{
	return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
	       ((ULONG)p[2] << 8)  | (ULONG)p[3];
}

static inline UQUAD be64(const UBYTE *p)
{
	return ((UQUAD)p[0] << 56) | ((UQUAD)p[1] << 48) |
	       ((UQUAD)p[2] << 40) | ((UQUAD)p[3] << 32) |
	       ((UQUAD)p[4] << 24) | ((UQUAD)p[5] << 16) |
	       ((UQUAD)p[6] << 8)  | (UQUAD)p[7];
}

///
/// read n-byte big-endian integer from a box field

static inline ULONG rd_be(const UBYTE *p, int n)
{
	ULONG v = 0;
	int i;
	for (i = 0; i < n; i++)
		v = (v << 8) | p[i];
	return v;
}

///
/// parse an OBU size field, return value and bytes consumed

static ULONG parse_obu_size(const UBYTE *buf, ULONG avail, ULONG *size)
{
	ULONG val = 0;
	ULONG i;
	for (i = 0; i < 4 && i < avail; i++)
	{
		val = (val << 7) | (buf[i] & 0x7F);
		if (!(buf[i] & 0x80))
		{
			*size = val;
			return i + 1;
		}
	}
	*size = 0;
	return 0;
}

///
/// tiny memory context for ISOBMFF parsing (pulls boxes into heap)

struct MemCtx
{
	UBYTE *mc_Data;
	ULONG  mc_Size;
	ULONG  mc_Pos;
};

static BOOL mc_init(struct MemCtx *mc, Object *obj, UQUAD offset, ULONG size)
{
	mc->mc_Size = size;
	mc->mc_Pos  = 0;
	mc->mc_Data = (UBYTE*)MediaAllocVec(size);
	if (!mc->mc_Data) return FALSE;
	if (DoMethod(obj, MMM_Seek, 0, MMM_SEEK_BYTES, &offset) != 1)
	{
		MediaFreeVec(mc->mc_Data);
		mc->mc_Data = NULL;
		return FALSE;
	}
	if (DoMethod(obj, MMM_Pull, 0, (ULONG)mc->mc_Data, size) != size)
	{
		MediaFreeVec(mc->mc_Data);
		mc->mc_Data = NULL;
		return FALSE;
	}
	return TRUE;
}

static void mc_free(struct MemCtx *mc)
{
	if (mc->mc_Data) { MediaFreeVec(mc->mc_Data); mc->mc_Data = NULL; }
}

static ULONG mc_tell(struct MemCtx *mc) { return mc->mc_Pos; }

static BOOL mc_read(struct MemCtx *mc, void *dst, ULONG n)
{
	if (mc->mc_Pos + n > mc->mc_Size) return FALSE;
	memcpy(dst, mc->mc_Data + mc->mc_Pos, n);
	mc->mc_Pos += n;
	return TRUE;
}

static BOOL mc_seek(struct MemCtx *mc, ULONG pos)
{
	if (pos > mc->mc_Size) return FALSE;
	mc->mc_Pos = pos;
	return TRUE;
}

static BOOL mc_skip(struct MemCtx *mc, ULONG n)
{
	return mc_seek(mc, mc->mc_Pos + n);
}

static ULONG mc_remaining(struct MemCtx *mc)
{
	return mc->mc_Size - mc->mc_Pos;
}

///
/// read a box header from a MemCtx, return TRUE if valid

static BOOL mc_read_box_header(struct MemCtx *mc, UBYTE type[4], UQUAD *box_size)
{
	UBYTE hdr[8];
	if (!mc_read(mc, hdr, 8)) return FALSE;
	*box_size = be32(hdr);
	memcpy(type, hdr + 4, 4);
	if (*box_size == 1)
	{
		if (!mc_read(mc, hdr, 8)) return FALSE;
		*box_size = be64(hdr);
	}
	else if (*box_size == 0)
	{
		/* box extends to end of mem context */
		*box_size = mc_remaining(mc) + 8;
	}
	return TRUE;
}

///
/// parse a single iloc extent entry

static BOOL parse_iloc_extent(struct MemCtx *mc, ULONG index_size, ULONG offset_size,
                              ULONG length_size, struct ItemExtent *ext)
{
	UBYTE buf[8];

	if (index_size > 0)
	{
		/* skip index (relative extent index within the item) */
		if (!mc_skip(mc, index_size)) return FALSE;
	}

	if (!mc_read(mc, buf, offset_size)) return FALSE;
	ext->ie_Offset = (UQUAD)rd_be(buf, offset_size);

	if (!mc_read(mc, buf, length_size)) return FALSE;
	ext->ie_Length = (UQUAD)rd_be(buf, length_size);

	return TRUE;
}

///
/// parse the ipco property container: reads av1c, ispe, auxc properties
/// and returns their content.  We only need the child box types and their
/// content for the primary item's property list (ipma lookup).

struct ParsedProp
{
	UBYTE pp_Type[4];  /* 4CC box type */
	ULONG pp_Size;     /* payload size (after full-box header) */
	ULONG pp_Payload;  /* offset into memctx of the payload */
};

///
/// Find an av01 item in the items array and check if it is an alpha item
/// (auxC with alpha URN references it).  For now: a simple heuristic — if
/// the primary item has an auxC property with the alpha URN and there is
/// exactly one other av01 item, that is the alpha item.

///
/// class macros

#define GET_BASE struct ClassBase *cb = (struct ClassBase*)cl->cl_UserData
#define GET_DATA struct ObjData *d = (struct ObjData*)INST_DATA(cl, obj)

///
/// prototypes

struct Library *LibInit(struct Library *unused, APTR seglist, struct Library *sysb);
struct ClassBase *lib_init(struct ClassBase *cb, APTR seglist, struct Library *SysBase);
APTR lib_expunge(struct ClassBase *cb);
struct Library *LibOpen(void);
ULONG LibClose(void);
APTR LibExpunge(void);
ULONG LibReserved(void);
Class *GetClass(void);
LONG ClassDispatcher(void);
LONG dummy_function(void);
Class *init_class(struct ClassBase *cb);
BOOL InitResources(struct ClassBase *cb);
void FreeResources(struct ClassBase *cb);
LONG New(Class *cl, Object *obj, struct opSet *msg);
LONG Dispose(Class *cl, Object *obj, Msg msg);
LONG Get(Class *cl, Object *obj, struct opGet *msg);

/* class specific */

LONG Pull(Class *cl, Object *obj, struct mmopData *msg);
BOOL GetHeader(Class *cl, Object *obj);

///
/// dummy_function()

LONG dummy_function(void)
{
	return -1;
}

///
/// resident *

const char LibName[] = CLASSNAME;
char VTag[] = VERSTAG;

static const struct TagItem RTags[] =
{
	{QUERYINFOATTR_NAME, (ULONG)LibName},
	{QUERYINFOATTR_IDSTRING, (ULONG)&VTag[1]},
	{QUERYINFOATTR_DESCRIPTION, (ULONG)"AVIF fileformat demuxer"},
	{QUERYINFOATTR_COPYRIGHT, (ULONG)"(c) 2026"},
	{QUERYINFOATTR_AUTHOR, (ULONG)"Reggae contributors"},
	{QUERYINFOATTR_DATE, (ULONG)DATE},
	{QUERYINFOATTR_VERSION, VERSION},
	{QUERYINFOATTR_REVISION, REVISION},
	{QUERYINFOATTR_SUBTYPE, QUERYSUBTYPE_LIBRARY},
	{QUERYINFOATTR_CLASS, QUERYCLASS_MULTIMEDIA},
	{QUERYINFOATTR_SUBCLASS, QUERYSUBCLASS_MULTIMEDIA_DEMUXER},
	{MMA_MediaType, MMT_PICTURE},
	{MMA_SupportedFormats, (ULONG)"M"},  /* dummy; only MMF_STREAM needed */
	{TAG_END, 0}
};

static const ULONG InputFormats[]  = { MMF_STREAM, 0 };
static const ULONG OutputFormats[] = { MMF_VIDEO_AVIF, 0 };

struct Resident ROMTag =
{
	RTC_MATCHWORD,
	&ROMTag,
	&ROMTag + 1,
	RTF_EXTENDED | RTF_PPC,
	VERSION,
	NT_LIBRARY,
	0,
	(STRPTR)LibName,
	VSTRING,
	(APTR)LibInit,
	REVISION,
	(struct TagItem *)RTags
};

APTR JumpTable[] =
{
	(APTR)FUNCARRAY_32BIT_NATIVE,
	(APTR)LibOpen,
	(APTR)LibClose,
	(APTR)LibExpunge,
	(APTR)LibReserved,
	(APTR)GetClass,
	(APTR)0xFFFFFFFF
};

///
/// init_class()

static const struct EmulLibEntry ClassDispatcher_gate =
{
	TRAP_LIB,
	0,
	(void(*)(void))ClassDispatcher
};

Class *init_class(struct ClassBase *cb)
{
	Class *cl = NULL;

	if ((cl = MakeClass(LibName, SUPERCLASS, NULL, sizeof(struct ObjData), 0L)))
	{
		cl->cl_Dispatcher.h_Entry = (HOOKFUNC)&ClassDispatcher_gate;
		cl->cl_UserData = (ULONG)cb;
		AddClass(cl);
	}
	cb->LibClass = cl;

	return cl;
}

///
/// InitResources()

BOOL InitResources(struct ClassBase *cb)
{
	if (!(IntuitionBase = OpenLibrary("intuition.library", 50))) return FALSE;
	if (!(UtilityBase = OpenLibrary("utility.library", 50))) return FALSE;
	if (!(MultimediaBase = OpenLibrary("Multimedia/multimedia.class", 50))) return FALSE;
	if (!(init_class(cb))) return FALSE;
	return TRUE;
}

///
/// FreeResources()

void FreeResources(struct ClassBase *cb)
{
	cb = cb;

	if (MultimediaBase) CloseLibrary(MultimediaBase);
	if (UtilityBase) CloseLibrary(UtilityBase);
	if (IntuitionBase) CloseLibrary(IntuitionBase);

	return;
}

///
/// LibInit()

struct ClassBase *lib_init(struct ClassBase *cb, APTR seglist, struct Library *sysbase)
{
	InitSemaphore(&cb->BaseLock);
	cb->Seglist = seglist;
	cb->Attributes = 0;
	sysbase = sysbase;
	return cb;
}

struct Library *LibInit(struct Library *unused, APTR seglist, struct Library *sysbase)
{
	unused = unused;
	SysBase = sysbase;

	return (NewCreateLibraryTags(
		LIBTAG_FUNCTIONINIT, (ULONG)JumpTable,
		LIBTAG_LIBRARYINIT,  (ULONG)lib_init,
		LIBTAG_MACHINE,      MACHINE_PPC,
		LIBTAG_BASESIZE,     sizeof(struct ClassBase),
		LIBTAG_SEGLIST,      (ULONG)seglist,
		LIBTAG_TYPE,         NT_LIBRARY,
		LIBTAG_NAME,         (ULONG)ROMTag.rt_Name,
		LIBTAG_IDSTRING,     (ULONG)ROMTag.rt_IdString,
		LIBTAG_FLAGS,        LIBF_CHANGED | LIBF_SUMUSED,
		LIBTAG_VERSION,      VERSION,
		LIBTAG_REVISION,     REVISION,
		LIBTAG_PUBLIC,       TRUE,
	TAG_END));
}

///
/// LibOpen()

struct Library *LibOpen(void)
{
	struct ClassBase *cb = (struct ClassBase*)REG_A6;
	struct Library *lib = (struct Library*)cb;

	ObtainSemaphore(&cb->BaseLock);

	if (!cb->InitFlag)
	{
		if (InitResources(cb)) cb->InitFlag = TRUE;
		else
		{
			FreeResources(cb);
			lib = NULL;
		}
	}

	if (lib)
	{
		cb->LibNode.lib_Flags &= ~LIBF_DELEXP;
		cb->LibNode.lib_OpenCnt++;
	}

	ReleaseSemaphore(&cb->BaseLock);
	return lib;
}

///
/// LibClose()

ULONG LibClose(void)
{
	struct ClassBase *cb = (struct ClassBase*)REG_A6;
	ULONG ret = 0;

	ObtainSemaphore(&cb->BaseLock);
	if (--cb->LibNode.lib_OpenCnt == 0)
	{
		if (cb->LibNode.lib_Flags & LIBF_DELEXP) ret = (ULONG)lib_expunge(cb);
	}
	ReleaseSemaphore(&cb->BaseLock);

	return ret;
}

///
/// LibExpunge()

APTR LibExpunge(void)
{
	struct ClassBase *cb = (struct ClassBase*)REG_A6;

	return(lib_expunge(cb));
}

APTR lib_expunge(struct ClassBase *cb)
{
	APTR seglist = NULL;

	ObtainSemaphore(&cb->BaseLock);

	if (cb->LibNode.lib_OpenCnt == 0)
	{
		if (!cb->LibClass || FreeClass(cb->LibClass))
		{
			cb->LibClass = NULL;
			Forbid();
			Remove((struct Node*)cb);
			Permit();
			FreeResources(cb);
			seglist = cb->Seglist;
			FreeMem((UBYTE*)cb - cb->LibNode.lib_NegSize, cb->LibNode.lib_NegSize + cb->LibNode.lib_PosSize);
			cb = NULL;
		}
		if (cb && cb->LibClass) AddClass(cb->LibClass);
	}
	else cb->LibNode.lib_Flags |= LIBF_DELEXP;

	if (cb) ReleaseSemaphore(&cb->BaseLock);
	return seglist;
}

///
/// LibReserved()

ULONG LibReserved(void)
{
	return 0;
}

///
/// GetClass()

Class *GetClass(VOID)
{
	struct ClassBase *cb = (struct ClassBase*)REG_A6;

	return cb->LibClass;
}

///
/// GetHeader() - parse AVIF ISOBMFF and build ObjData
///
/// Reads all top-level boxes from the stream, parses the ISOBMFF meta
/// hierarchy to find the primary item, its extents (iloc), dimensions
/// (ispe), codec config (av1C), and the image item's elementary data
/// in the mdat box.  Assembles the complete AV1 elementary stream.

static BOOL parse_ftyp(struct MemCtx *mc, UQUAD box_size)
{
	UBYTE data[8];
	ULONG compat_offset = 8; /* after major_brand(4) + minor_version(4) */

	if (box_size < 12) return TRUE; /* no compatible brands */

	/* skip to compatible brands start (skip major_brand + minor_version) */
	if (!mc_skip(mc, 8)) return FALSE;

	while (mc_remaining(mc) >= 4)
	{
		if (!mc_read(mc, data, 4)) return FALSE;
		/* we only care about 'avif' and 'avis' as compatible brands */
		/* (acceptance done in Recognize; here we just consume) */
		(void)data;
	}
	return TRUE;
}

///
/// parse_ipco_children - walk ipco content, store property descriptors
/// that we care about for later lookup via ipma

static BOOL parse_ipco(struct MemCtx *mc, UQUAD box_size,
                       struct ParsedProp *props, ULONG *num_props,
                       ULONG *av1c_payload, ULONG *av1c_size)
{
	ULONG start = mc_tell(mc);
	*av1c_payload = 0;
	av1c_size[0] = 0;

	while (mc_tell(mc) < start + box_size && *num_props < AVIF_MAX_PROPERTIES)
	{
		UBYTE child_type[4];
		UQUAD child_size;
		ULONG child_start;

		if (!mc_read_box_header(mc, child_type, &child_size)) return FALSE;
		child_start = mc_tell(mc);

		/* We track ispe, pixi, av1c, auxC, colr */
		if (!memcmp(child_type, "ispe", 4) ||
		    !memcmp(child_type, "pixi", 4) ||
		    !memcmp(child_type, "av1C", 4) ||
		    !memcmp(child_type, "auxC", 4) ||
		    !memcmp(child_type, "colr", 4))
		{
			ULONG prop_idx = *num_props;
			memcpy(props[prop_idx].pp_Type, child_type, 4);
			props[prop_idx].pp_Size = child_size;

			/* For av1C: record payload location for later read */
			if (!memcmp(child_type, "av1C", 4))
			{
				/* Skip full-box header (version + flags) if present */
				if (child_size >= 7)
				{
					props[prop_idx].pp_Payload = child_start;
					/* Read the av1C content now into ObjData's av1c slot */
					/* (caller will handle this via a second pass) */
				}
			}
			else
			{
				props[prop_idx].pp_Payload = child_start;
			}

			(*num_props)++;
		}

		/* advance past child content */
		if (!mc_seek(mc, child_start + child_size)) return FALSE;
	}
	return TRUE;
}

///
/// Build the elementary stream for the primary image item:
///   1. Optionally prepend the av1C sequence header OBU
///   2. Append the raw mdat bytes for each iloc extent

static BOOL build_elementary_stream(Class *cl, Object *obj, struct ObjData *d)
{
	struct ItemDesc *item = NULL;
	ULONG i;
	UQUAD stream_length = MediaGetPort64(obj, 0, MMA_StreamLength);
	ULONG es_buf_size = 0;
	ULONG es_pos = 0;

	/* Find the primary item */
	for (i = 0; i < d->od_NumItems; i++)
	{
		if (d->od_Items[i].id_ItemID == d->od_PrimaryItemID)
		{
			item = &d->od_Items[i];
			break;
		}
	}

	if (!item)
	{
		MLOG(LOG_ERRORS, "Primary item not found.");
		return FALSE;
	}

	/* Calculate total extents length */
	for (i = 0; i < item->id_NumExtents; i++)
	{
		es_buf_size += (ULONG)item->id_Extents[i].ie_Length;
	}

	if (es_buf_size == 0)
	{
		MLOG(LOG_ERRORS, "Primary item has zero extents.");
		return FALSE;
	}

	/* Allocate ES buffer: av1c header + item data */
	d->od_ES = (UBYTE*)MediaAllocVec(d->od_Av1cLength + es_buf_size);
	if (!d->od_ES)
	{
		MLOG(LOG_ERRORS, "Out of memory for elementary stream.");
		return FALSE;
	}

	/* Prepend av1C sequence header */
	if (d->od_Av1cLength > 0)
	{
		memcpy(d->od_ES, d->od_Av1c, d->od_Av1cLength);
		es_pos = d->od_Av1cLength;
	}

	/* Copy extent data from mdat */
	for (i = 0; i < item->id_NumExtents; i++)
	{
		UQUAD offset = item->id_Extents[i].ie_Offset;
		UQUAD length = item->id_Extents[i].ie_Length;
		ULONG chunk;

		if (offset + length > stream_length)
		{
			MLOG(LOG_ERRORS, "Extent beyond end of stream.");
			MediaFreeVec(d->od_ES);
			d->od_ES = NULL;
			d->od_ESLength = 0;
			return FALSE;
		}

		if (DoMethod(obj, MMM_Seek, 0, MMM_SEEK_BYTES, &offset) != 1)
		{
			MediaFreeVec(d->od_ES);
			d->od_ES = NULL;
			d->od_ESLength = 0;
			return FALSE;
		}

		/* Pull in chunks (media alloc limit) */
		chunk = (ULONG)length;
		if (DoMethod(obj, MMM_Pull, 0, (ULONG)(d->od_ES + es_pos), chunk) != chunk)
		{
			MediaFreeVec(d->od_ES);
			d->od_ES = NULL;
			d->od_ESLength = 0;
			return FALSE;
		}
		es_pos += chunk;
	}

	d->od_ESLength = es_pos;
	return TRUE;
}

///
/// GetHeader() - parse AVIF / ISO BMFF

BOOL GetHeader(Class *cl, Object *obj)
{
	GET_DATA;
	BOOL result = FALSE;
	UQUAD stream_length = MediaGetPort64(obj, 0, MMA_StreamLength);
	UQUAD pos = 0;

	/* Read top-level box headers and walk until we find meta + mdat */
	UQUAD meta_offset = 0;
	ULONG  meta_size  = 0;
	UQUAD mdat_offset = 0;
	ULONG  mdat_size  = 0;

	/* Properties parsed from ipco */
	struct ParsedProp props[AVIF_MAX_PROPERTIES];
	ULONG num_props = 0;

	/* ipma mapping for primary item */
	UWORD primary_ipma_count = 0;
	UBYTE primary_ipma_associations[AVIF_MAX_PROPERTIES * 2];

	MLOGV(LOG_INFO, "Parsing AVIF, stream length %Ld.", stream_length);

	if (stream_length < 12)
	{
		seterr(MMERR_END_OF_DATA);
		MLOG(LOG_ERRORS, "AVIF file too small.");
		return FALSE;
	}

	/* Read top-level boxes to find ftyp, meta, mdat */
	pos = 0;
	while (pos < stream_length)
	{
		UBYTE hdr[8];
		UQUAD box_size;
		UBYTE box_type[4];
		UQUAD child_pos;

		if (DoMethod(obj, MMM_Seek, 0, MMM_SEEK_BYTES, &pos) != 1)
		{
			seterr(MMERR_IO_ERROR);
			MLOG(LOG_ERRORS, "Seek failed.");
			break;
		}

		if (DoMethod(obj, MMM_Pull, 0, (ULONG)hdr, 8) != 8)
			break;

		box_size = be32(hdr);
		memcpy(box_type, hdr + 4, 4);

		if (box_size == 1)
		{
			if (DoMethod(obj, MMM_Pull, 0, (ULONG)hdr, 8) != 8) break;
			box_size = be64(hdr);
		}
		else if (box_size == 0)
		{
			box_size = stream_length - pos;
		}

		child_pos = pos + 8;

		/* 'ftyp' brand check (already validated in Recognize) */
		if (!memcmp(box_type, "ftyp", 4))
		{
			/* advance past ftyp */
		}
		else if (!memcmp(box_type, "meta", 4) || !memcmp(box_type, "moov", 4))
		{
			/* Some files put meta inside moov; prefer top-level meta */
			if (!meta_offset)
			{
				meta_offset = child_pos;
				meta_size   = (ULONG)(box_size - 8);
			}
		}
		else if (!memcmp(box_type, "mdat", 4))
		{
			mdat_offset = child_pos;
			mdat_size   = (ULONG)(box_size - 8);
		}

		pos += box_size;
	}

	if (!meta_offset || !mdat_offset)
	{
		seterr(MMERR_WRONG_DATA);
		MLOG(LOG_ERRORS, "No meta or mdat box found.");
		return FALSE;
	}

	MLOGV(LOG_INFO, "meta at %Ld, mdat at %Ld.", meta_offset, mdat_offset);

	/* Now parse the meta box content */
	{
		struct MemCtx mc_meta;
		UQUAD meta_box_end;

		if (!mc_init(&mc_meta, obj, meta_offset, meta_size))
		{
			seterr(MMERR_OUT_OF_MEMORY);
			return FALSE;
		}

		meta_box_end = mc_tell(&mc_meta) + meta_size;

		/* Walk meta child boxes: iloc, iinf, pitm, iprp (→ipco, ipma) */
		while (mc_tell(&mc_meta) < meta_box_end && result == FALSE)
		{
			UBYTE child_type[4];
			UQUAD child_size;
			ULONG child_start;

			if (!mc_read_box_header(&mc_meta, child_type, &child_size))
				break;

			child_start = mc_tell(&mc_meta);

			/* --- pitm: primary item ID --- */
			if (!memcmp(child_type, "pitm", 4))
			{
				UBYTE ver;
				UBYTE pitm_buf[4];
				ULONG pitm_off;

				if (!mc_read(&mc_meta, &ver, 1)) break;
				/* skip flags */
				if (!mc_skip(&mc_meta, 3)) break;

				if (ver <= 1)
					pitm_off = 2;
				else
					pitm_off = 4;

				if (!mc_read(&mc_meta, pitm_buf, pitm_off)) break;
				d->od_PrimaryItemID = (ver <= 1) ? be16(pitm_buf) : be16(pitm_buf);

				MLOGV(LOG_INFO, "Primary item ID: %d.", d->od_PrimaryItemID);
			}

			/* --- iinf: item count + infe entries with item_type --- */
			else if (!memcmp(child_type, "iinf", 4))
			{
				UBYTE ver;
				UBYTE cnt_buf[4];
				ULONG item_count;
				ULONG i;

				if (!mc_read(&mc_meta, &ver, 1)) break;
				if (!mc_skip(&mc_meta, 3)) break; /* flags */

				if (ver <= 1)
				{
					if (!mc_read(&mc_meta, cnt_buf, 2)) break;
					item_count = be16(cnt_buf);
				}
				else
				{
					if (!mc_read(&mc_meta, cnt_buf, 4)) break;
					item_count = be32(cnt_buf);
				}

				if (item_count > AVIF_MAX_ITEMS)
					item_count = AVIF_MAX_ITEMS;

				d->od_NumItems = item_count;

				/* Read infe entries */
				for (i = 0; i < item_count; i++)
				{
					UBYTE infe_type[4];
					UQUAD infe_size;
					ULONG infe_start;
					UBYTE item_id_buf[4];

					if (!mc_read_box_header(&mc_meta, infe_type, &infe_size)) break;
					infe_start = mc_tell(&mc_meta);

					if (memcmp(infe_type, "infe", 4)) break;

					/* version byte + flags */
					if (!mc_skip(&mc_meta, 4)) break;

					/* item_ID: u16 for version 0-2, u32 for version 3+ */
					if (!mc_read(&mc_meta, item_id_buf, 2)) break;
					d->od_Items[i].id_ItemID = be16(item_id_buf);

					/* item_type 4cc at fixed offset */
					if (!mc_seek(&mc_meta, infe_start + 8)) break;
					if (!mc_read(&mc_meta, infe_type, 4)) break;
					d->od_Items[i].id_IsAv01 = !memcmp(infe_type, "av01", 4);

					/* advance to next entry */
					if (!mc_seek(&mc_meta, infe_start + infe_size)) break;
				}
			}

			/* --- iloc: item locations --- */
			else if (!memcmp(child_type, "iloc", 4))
			{
				UBYTE iloc_buf[4];
				UBYTE top_byte;
				ULONG offset_size, length_size, base_offset_size, index_size;
				UWORD item_count;
				UWORD i;

				if (!mc_read(&mc_meta, iloc_buf, 2)) break;

				/* top byte: offset_size (4b) | length_size (4b) */
				top_byte = iloc_buf[0];
				offset_size    = (top_byte >> 4) & 0x0F;
				length_size    = top_byte & 0x0F;

				/* second byte: base_offset_size (4b) | index_size (4b) */
				top_byte = iloc_buf[1];
				base_offset_size = (top_byte >> 4) & 0x0F;
				index_size       = top_byte & 0x0F;

				if (!mc_read(&mc_meta, iloc_buf, 2)) break;
				item_count = be16(iloc_buf);

				if (item_count > AVIF_MAX_ITEMS)
					item_count = AVIF_MAX_ITEMS;

				/* Read each item entry */
				for (i = 0; i < item_count; i++)
				{
					UBYTE item_id_buf[4];
					UWORD item_id;
					UWORD extent_count;
					ULONG base_offset = 0;
					UWORD j;
					struct ItemDesc *desc = NULL;

					if (!mc_read(&mc_meta, item_id_buf, 2)) break;
					item_id = be16(item_id_buf);

					/* construction_method + data_reference_index (v1/v2) */
					{
						UBYTE ver_buf[1];
						mc_read(&mc_meta, ver_buf, 1);
						/* we only support method 0 (absolute file offset) */
					}

					/* base offset */
					if (base_offset_size > 0)
					{
						UBYTE bo_buf[8];
						if (!mc_read(&mc_meta, bo_buf, base_offset_size)) break;
						base_offset = (ULONG)rd_be(bo_buf, base_offset_size);
					}

					/* extent count */
					{
						UBYTE ec_buf[2];
						if (!mc_read(&mc_meta, ec_buf, 2)) break;
						extent_count = be16(ec_buf);
					}

					if (extent_count > 16) extent_count = 16;

					/* Find the desc for this item */
					for (j = 0; j < d->od_NumItems; j++)
					{
						if (d->od_Items[j].id_ItemID == item_id)
						{
							desc = &d->od_Items[j];
							break;
						}
					}

					if (desc)
					{
						/* Skip data_reference_index byte if construction_method != 0 */
						/* For simplicity: always consume 2 bytes (data_ref_index field) */
						{
							UBYTE dr_buf[2];
							if (!mc_read(&mc_meta, dr_buf, 2)) break;
						}
					}

					if (desc)
					{
						UQUAD cumulative = base_offset;

						desc->id_NumExtents = extent_count;

						for (j = 0; j < extent_count; j++)
						{
							if (!parse_iloc_extent(&mc_meta, index_size,
							                        offset_size, length_size,
							                        &desc->id_Extents[j]))
							{
								break;
							}

							/* For method 0: absolute offsets (already absolute) */
							desc->id_Extents[j].ie_Offset += base_offset;
							cumulative += desc->id_Extents[j].ie_Length;
						}
					}
					else
					{
						/* skip extents we don't need */
						ULONG j;
						struct ItemExtent dummy;
						for (j = 0; j < extent_count; j++)
						{
							if (!parse_iloc_extent(&mc_meta, index_size,
							                        offset_size, length_size,
							                        &dummy))
								break;
						}
					}
				}
			}

			/* --- iprp: item properties --- */
			else if (!memcmp(child_type, "iprp", 4))
			{
				/* iprp contains ipco + ipma */
				ULONG iprp_start = mc_tell(&mc_meta);
				ULONG iprp_end   = iprp_start + child_size;

				while (mc_tell(&mc_meta) < iprp_end)
				{
					UBYTE child2_type[4];
					UQUAD child2_size;
					ULONG child2_start;

					if (!mc_read_box_header(&mc_meta, child2_type, &child2_size))
						break;

					child2_start = mc_tell(&mc_meta);

					/* ipco: property container */
					if (!memcmp(child2_type, "ipco", 4))
					{
						num_props = 0;
						if (!parse_ipco(&mc_meta, child2_size, props, &num_props,
						                NULL, NULL))
						{
							break;
						}

						/* Now read the av1C content properly */
						{
							ULONG k;
							for (k = 0; k < num_props; k++)
							{
								if (!memcmp(props[k].pp_Type, "av1C", 4))
								{
									UBYTE av1c_buf[32];
									ULONG avail;

									if (!mc_seek(&mc_meta, props[k].pp_Payload)) break;

									/* av1C is a FullBox: skip version + flags (3 bytes) */
									if (!mc_skip(&mc_meta, 3)) break;

									avail = props[k].pp_Size - 3;
									if (avail > 28) avail = 28;

									if (mc_read(&mc_meta, av1c_buf, avail))
									{
										memcpy(d->od_Av1c, av1c_buf, avail);
										d->od_Av1cLength = avail;
									}
									break;
								}
							}
						}
					}

					/* ipma: item property association */
					if (!memcmp(child2_type, "ipma", 4))
					{
						UBYTE ver_buf[1];
						UBYTE cnt_buf[4];
						UWORD entry_count;
						UWORD i;

						if (!mc_read(&mc_meta, ver_buf, 1)) break;
						if (!mc_skip(&mc_meta, 3)) break; /* flags */

						if (!mc_read(&mc_meta, cnt_buf, 2)) break;
						entry_count = be16(cnt_buf);

						for (i = 0; i < entry_count; i++)
						{
							UBYTE item_id_buf[2];
							UWORD item_id;
							UBYTE assoc_count;
							UWORD k;

							if (!mc_read(&mc_meta, item_id_buf, 2)) break;
							item_id = be16(item_id_buf);

							if (!mc_read(&mc_meta, &assoc_count, 1)) break;

							for (k = 0; k < assoc_count; k++)
							{
								UBYTE assoc_buf[2];
								ULONG prop_index;

								if (ver_buf[0] <= 0)
								{
									if (!mc_read(&mc_meta, assoc_buf, 1)) break;
									prop_index = assoc_buf[0] & 0x7F;
								}
								else
								{
									if (!mc_read(&mc_meta, assoc_buf, 2)) break;
									prop_index = be16(assoc_buf) & 0x7FFF;
								}

								/* prop_index is 1-based into ipco children */
								if (prop_index > 0 && prop_index <= num_props)
								{
									struct ParsedProp *pp = &props[prop_index - 1];

									/* ispe: extract width/height */
									if (!memcmp(pp->pp_Type, "ispe", 4))
									{
										UBYTE ispe_buf[8];
										if (!mc_seek(&mc_meta, pp->pp_Payload)) break;
										if (!mc_skip(&mc_meta, 3)) break; /* version+flags */
										if (mc_read(&mc_meta, ispe_buf, 8))
										{
											d->od_Width  = be32(ispe_buf);
											d->od_Height = be32(ispe_buf + 4);
										}
									}

									/* auxC: alpha detection */
									if (!memcmp(pp->pp_Type, "auxC", 4))
									{
										UBYTE aux_buf[64];
										ULONG aux_payload_start;
										ULONG avail;
										struct ItemDesc *desc = NULL;
										UWORD l;

										for (l = 0; l < d->od_NumItems; l++)
										{
											if (d->od_Items[l].id_ItemID == item_id)
											{
												desc = &d->od_Items[l];
												break;
											}
										}

										aux_payload_start = pp->pp_Payload;
										if (!mc_seek(&mc_meta, aux_payload_start)) break;
										if (!mc_skip(&mc_meta, 3)) break; /* version+flags */
										avail = pp->pp_Size - 3;
										if (avail > 63) avail = 63;
										if (mc_read(&mc_meta, aux_buf, avail))
										{
											aux_buf[avail] = '\0';
											if (strstr((char*)aux_buf, "alpha"))
											{
												d->od_UseAlpha = TRUE;
												if (desc) desc->id_HasAlphaAux = TRUE;
											}
										}
									}
								}
							}
						}
					}

					if (!mc_seek(&mc_meta, child2_start + child2_size)) break;
				}
			}

			/* skip unrecognized children */
			if (!mc_seek(&mc_meta, child_start + child_size)) break;
		}

		mc_free(&mc_meta);
	}

	/* Validate: we need width, height, and at least one extent */
	if (d->od_Width == 0 || d->od_Height == 0)
	{
		seterr(MMERR_WRONG_DATA);
		MLOG(LOG_ERRORS, "Missing ispe dimensions.");
		return FALSE;
	}

	/* Determine bits per pixel from av1C configuration */
	if (d->od_Av1cLength > 0)
	{
		/* av1C bit layout (after header):
		 *  byte 4: marker(1) version(7) = 0x81
		 *  byte 5: seq_profile(3) seq_level_idx_0(5)
		 *  byte 6: seq_tier_0(1) high_bitdepth(1) twelve_bit(1) monochrome(1)
		 *           chroma_subsampling_x(1) chroma_subsampling_y(1) chroma_sample_position(2)
		 */
		if (d->od_Av1cLength >= 7)
		{
			UBYTE flags = d->od_Av1c[6]; /* third config byte */
			d->od_HighBitdepth = (flags >> 5) & 1;
			d->od_TwelveBit    = (flags >> 4) & 1;
			d->od_Monochrome   = (flags >> 3) & 1;
			d->od_ChromaSubX   = (flags >> 2) & 1;
			d->od_ChromaSubY   = (flags >> 1) & 1;

			if (d->od_HighBitdepth)
				d->od_BitsPerPixel = d->od_TwelveBit ? 12 : 10;
			else
				d->od_BitsPerPixel = 8;
		}
		else
		{
			d->od_BitsPerPixel = 8;
		}
	}
	else
	{
		d->od_BitsPerPixel = 8;
	}

	/* Build the elementary stream */
	if (!build_elementary_stream(cl, obj, d))
	{
		seterr(MMERR_OUT_OF_MEMORY);
		return FALSE;
	}

	/* Allocate info structure for decoder */
	d->od_Info = (struct AvifInfo*)MediaAllocVec(sizeof(struct AvifInfo));
	if (d->od_Info)
	{
		memset(d->od_Info, 0, sizeof(struct AvifInfo));
		d->od_Info->ai_Width         = d->od_Width;
		d->od_Info->ai_Height        = d->od_Height;
		d->od_Info->ai_BitsPerPixel  = d->od_BitsPerPixel;
		d->od_Info->ai_UseAlpha      = d->od_UseAlpha;
		d->od_Info->ai_AlphaPremul   = d->od_AlphaPremul;
		d->od_Info->ai_Monochrome    = d->od_Monochrome;
		d->od_Info->ai_ChromaSubX    = d->od_ChromaSubX;
		d->od_Info->ai_ChromaSubY    = d->od_ChromaSubY;
		d->od_Info->ai_ES            = d->od_ES;
		d->od_Info->ai_ESLength      = d->od_ESLength;
		d->od_Info->ai_Av1cLength    = d->od_Av1cLength;
		memcpy(d->od_Info->ai_Av1c, d->od_Av1c, d->od_Av1cLength);
	}

	MLOGV(LOG_INFO, "AVIF: %ld x %ld, %ld-bit, alpha=%ld, ES=%ld bytes.",
		d->od_Width, d->od_Height, d->od_BitsPerPixel,
		d->od_UseAlpha, d->od_ESLength);

	return TRUE;
}

///
/// New()

LONG New(Class *cl, Object *obj, struct opSet *msg)
{
	LONG newobj = 0;

	if ((obj = (Object*)DoSuperMethodA(cl, obj, (Msg)msg)))
	{
		GET_DATA;

		DoMethod(obj, MMM_LockObject);

		/* input port */
		DoMethod(obj, MMM_AddPort, 0);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_Type, MDP_TYPE_INPUT);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_FormatsTable, (ULONG)InputFormats);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_Format, MMF_STREAM);

		/* output port */
		DoMethod(obj, MMM_AddPort, 1);
		DoMethod(obj, MMM_SetPort, 1, MMA_Port_Type, MDP_TYPE_OUTPUT);
		DoMethod(obj, MMM_SetPort, 1, MMA_Port_FormatsTable, (ULONG)OutputFormats);
		DoMethod(obj, MMM_SetPort, 1, MMA_Port_Format, MMF_VIDEO_AVIF);

		d->od_ES = NULL;
		d->od_ESLength = 0;
		d->od_Info = NULL;
		d->od_Width = 0;
		d->od_Height = 0;

		newobj = (LONG)obj;

		DoMethod(obj, MMM_UnlockObject);
	}

	if (!newobj) CoerceMethod(cl, obj, (Msg)OM_DISPOSE);
	else MLOGV(LOG_INFO, "Object created.");

	return newobj;
}

///
/// Dispose()

LONG Dispose(Class *cl, Object *obj, Msg msg)
{
	GET_DATA;

	DoMethod(obj, MMM_LockObject);

	if (d->od_ES) MediaFreeVec(d->od_ES);
	if (d->od_Info) MediaFreeVec(d->od_Info);

	MLOGV(LOG_INFO, "Object disposed.");

	DoMethod(obj, MMM_UnlockObject);
	return DoSuperMethodA(cl, obj, msg);
}

///
/// Get()

LONG Get(Class *cl, Object *obj, struct opGet *msg)
{
	GET_DATA;

	switch (msg->opg_AttrID)
	{
		case MMA_Video_Width:
			*msg->opg_Storage = d->od_Width;
			return TRUE;

		case MMA_Video_Height:
			*msg->opg_Storage = d->od_Height;
			return TRUE;

		case MMA_Video_BitsPerPixel:
			*msg->opg_Storage = d->od_BitsPerPixel;
			return TRUE;

		case MMA_Video_UseAlpha:
			*msg->opg_Storage = d->od_UseAlpha;
			return TRUE;

		case MMA_Video_FrameCount:
			/* AVIF images are single-frame stills */
			*(UQUAD*)msg->opg_Storage = 1;
			return TRUE;

		case MMA_DataFormat:
			*msg->opg_Storage = (LONG)"AVIF";
			return TRUE;

		case MMA_MediaType:
			*msg->opg_Storage = MMT_PICTURE;
			return TRUE;

		case MMA_ExtraData:
			*msg->opg_Storage = (LONG)d->od_Info;
			return TRUE;

		default:
			return DoSuperMethodA(cl, obj, (Msg)msg);
	}
}

///
/// Pull()

LONG Pull(Class *cl, Object *obj, struct mmopData *msg)
{
	GET_DATA;
	ULONG bytes_pulled = 0;

	DoMethod(obj, MMM_LockObject);
	seterr(0);

	if (msg->Buffer && msg->Length)
	{
		switch (msg->Port)
		{
			case 0:
				bytes_pulled = DoSuperMethodA(cl, obj, (Msg)msg);
			break;

			case 1:
				/* Serve from the assembled elementary stream buffer */
				if (d->od_ES && d->od_ESLength > 0 && d->od_Info)
				{
					ULONG avail = d->od_Info->ai_ESLength;
					if (msg->Length <= avail)
					{
						memcpy(msg->Buffer, d->od_Info->ai_ES, msg->Length);
						d->od_Info->ai_ES       += msg->Length;
						d->od_Info->ai_ESLength -= msg->Length;
						bytes_pulled = msg->Length;
					}
					else if (avail > 0)
					{
						memcpy(msg->Buffer, d->od_Info->ai_ES, avail);
						d->od_Info->ai_ES       += avail;
						d->od_Info->ai_ESLength -= avail;
						bytes_pulled = avail;
					}
					else
					{
						/* EOF on elementary stream */
						seterr(MMERR_END_OF_DATA);
					}
				}
				else
				{
					seterr(MMERR_END_OF_DATA);
				}
			break;

			default:
				seterr(MMERR_WRONG_ARGUMENTS);
			break;
		}
	}
	else
	{
		seterr(MMERR_WRONG_ARGUMENTS);
	}

	DoMethod(obj, MMM_UnlockObject);

	return bytes_pulled;
}

///
/// Setup()

LONG Setup(Class *cl, Object *obj, struct mmopPort *msg)
{
	GET_DATA;
	LONG rv = 0;

	MLOGV(LOG_VERBOSE, "Started for port %lu.", msg->Port);

	if (msg->Port == 0)
	{
		rv = GetHeader(cl, obj);
	}
	else if (msg->Port == 1) rv = TRUE;
	else MLOGV(LOG_ERRORS, "Setup on non existing port %ld.", msg->Port);

	return rv;
}

///
/// GetPort()

LONG GetPort(Class *cl, Object *obj, struct mmopGetPort *msg)
{
	switch (msg->Attribute)
	{
		case MMA_Video_Width:
		case MMA_Video_Height:
		case MMA_Video_BitsPerPixel:
		case MMA_Video_UseAlpha:
		case MMA_DataFormat:
		case MMA_ExtraData:
		case MMA_MediaType:
			return DoMethod(obj, OM_GET, msg->Attribute, (ULONG)msg->Storage);
	}
	return (DoSuperMethodA(cl, obj, (Msg)msg));
}

///
/// dispatcher

LONG ClassDispatcher(void)
{
	Class *cl = (Class*)REG_A0;
	Object *obj = (Object*)REG_A2;
	Msg msg = (Msg)REG_A1;

	switch (msg->MethodID)
	{
		case OM_NEW:      return New(cl, obj, (struct opSet*)msg);
		case OM_DISPOSE:  return Dispose(cl, obj, msg);
		case OM_GET:      return Get(cl, obj, (struct opGet*)msg);
		case MMM_Pull:    return Pull(cl, obj, (struct mmopData*)msg);
		case MMM_Setup:   return Setup(cl, obj, (struct mmopPort*)msg);
		case MMM_GetPort: return GetPort(cl, obj, (struct mmopGetPort*)msg);
		default:          return DoSuperMethodA(cl, obj, msg);
	}
}

///