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
UBYTE bh_Size[4];
	UBYTE bh_Type[4];
	UBYTE bh_LargeSize[8];
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
/// AV1 OBU types (see AV1CodecConfigurationRecord / OBU syntax)

#define AV1_OBU_SEQUENCE_HEADER   1
#define AV1_OBU_TEMPORAL_DELIMITER 2

///
/// AVIF parsing limits

#define AVIF_MAX_TOP_LEVEL_BOXES  64
#define AVIF_MAX_ITEMS            64
#define AVIF_MAX_PROPERTIES       64
#define AVIF_MAX_ITEM_PROPS       32
#define AVIF_MAX_META_ENTRIES      8
#define AVIF_MAX_STRING_LEN       256

///
/// upper bound on the size of the 'meta' box we are willing to buffer, so a
/// corrupt file cannot make us allocate an arbitrary amount of memory

#define AVIF_MAX_META_SIZE   (16UL * 1024 * 1024)

///
/// upper bound on the assembled elementary stream, so that a corrupt
/// extent table cannot ask for an absurd allocation

#define AVIF_MAX_ES_SIZE    (64UL * 1024 * 1024)

///
/// AVIF auxiliary type URNs (ISO 23001-8 / AVIF)

#define AVIF_ALPHA_PREMUL_URN "urn:mpeg:mpegB:cicp:systems:auxiliary:premult"

///
/// maximum number of extents recorded per item

#define AVIF_MAX_ITEM_EXTENTS 16

///
/// item/location descriptor

struct ItemExtent
{
	UQUAD ie_Offset;       /* absolute file offset */
	UQUAD ie_Length;       /* extent length in bytes */
};

struct ItemDesc
{
	UQUAD id_ItemID;
	UWORD id_NumExtents;
	struct ItemExtent id_Extents[AVIF_MAX_ITEM_EXTENTS];
	/* property indices from ipma, 1-based, in ipco order */
	UWORD id_NumProps;
	UWORD id_Props[AVIF_MAX_ITEM_PROPS];
	/* infe item type ("av01", "Grid", "mime", ...) */
	UBYTE id_ItemType[4];
	UBYTE id_ContentType[AVIF_MAX_STRING_LEN];  /* infe content_type */
	BOOL  id_IsAv01;
	BOOL  id_HasLocation;
};

///
/// object instance data

struct ObjData
{
	/* image geometry parsed from ispe / av1C */
	ULONG od_Width;
	ULONG od_Height;
	ULONG od_BitsPerPixel;
	BOOL  od_UseAlpha;       /* a separate alpha item exists */
	BOOL  od_AlphaPremul;    /* alpha is premultiplied into the colour planes */
	BOOL  od_Monochrome;
	BOOL  od_HighBitdepth;
	BOOL  od_TwelveBit;
	ULONG od_ChromaSubX;
	ULONG od_ChromaSubY;

	/* colour signalling from the colr box (nclx), if present */
	BOOL  od_HaveNclx;
	UWORD od_ColourPrimaries;
	UWORD od_TransferCharacteristics;
	UWORD od_MatrixCoefficients;
	BOOL  od_FullRangeFlag;

	/* primary image item */
	UQUAD od_PrimaryItemID;
	BOOL  od_HavePrimaryItemID;

	/* the auxiliary alpha item, if the primary item is derived */
	UQUAD od_AlphaItemID;
	BOOL  od_HaveAlphaItem;

	/* number of frames in the sequence (1 for a still image) */
	ULONG od_FrameCount;

	/* items */
	UWORD od_NumItems;
	struct ItemDesc od_Items[AVIF_MAX_ITEMS];

	/* the assembled AV1 elementary stream (colour plane) */
	UBYTE *od_ES;
	ULONG  od_ESLength;

	/* the assembled AV1 elementary stream (alpha plane, may be NULL) */
	UBYTE *od_AlphaES;
	ULONG  od_AlphaESLength;

	/* av1C box bytes (informational, the codec config record) */
	UBYTE  od_Av1c[32];
	ULONG  od_Av1cLength;

	/* parsed metadata strings, handed out via AVIF_META_DATA */
	UWORD  od_NumMeta;
	UWORD  od_MetaTags[AVIF_MAX_META_ENTRIES];
	UBYTE  od_MetaStrings[AVIF_MAX_META_ENTRIES][AVIF_MAX_STRING_LEN];
	struct AvifMetaData od_Meta;

	/* read position used by Pull() on the output port */
	ULONG od_ESPos;

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

static inline UQUAD rd_be(const UBYTE *p, int n)
{
	UQUAD v = 0;
	int i;
	if (n < 1 || n > 8) return 0;
	for (i = 0; i < n; i++)
		v = (v << 8) | (UQUAD)p[i];
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
/// read a box header from a MemCtx.
///
/// On success *box_size is the total box size including the header,
/// *hdr_size is the number of header bytes actually consumed (8, 16 or 24
/// for a uuid box), the read position is left immediately after the header
/// (i.e. at the box payload) and *payload_size is box_size - hdr_size.
///
static BOOL mc_read_box_header(struct MemCtx *mc, UBYTE type[4], UQUAD *box_size,
                               ULONG *hdr_size, UQUAD *payload_size)
{
	UBYTE hdr[16];
	ULONG start = mc_tell(mc);

	*box_size = 0;
	*hdr_size = 0;
	*payload_size = 0;

	if (mc_remaining(mc) < 8) return FALSE;
	if (!mc_read(mc, hdr, 8)) return FALSE;

	*box_size = be32(hdr);
	memcpy(type, hdr + 4, 4);
	*hdr_size = 8;

	if (*box_size == 1)
	{
		/* 64 bit largesize follows the 32 bit size */
		if (mc_remaining(mc) < 8) return FALSE;
		if (!mc_read(mc, hdr, 8)) return FALSE;
		*box_size = be64(hdr);
		*hdr_size = 16;
	}
	else if (*box_size == 0)
	{
		/* box extends to the end of the enclosing container */
		*box_size = (UQUAD)mc->mc_Size - start;
	}

	/* reject boxes that cannot possibly fit in the remaining data */
	if (*box_size < *hdr_size) return FALSE;
	if (*box_size > (UQUAD)(mc->mc_Size - start)) return FALSE;

	if (!memcmp(type, "uuid", 4))
	{
		/* uuid boxes carry a 16 byte usertype after the header */
		if (*box_size < *hdr_size + 16) return FALSE;
		*hdr_size += 16;
	}

	*payload_size = *box_size - *hdr_size;
	return TRUE;
}

///
/// read an n-byte big-endian integer straight out of a MemCtx

static BOOL mc_read_uint(struct MemCtx *mc, ULONG n, UQUAD *val)
{
	UBYTE buf[8];

	/* a zero width field is legal in iloc (base_offset_size, index_size) */
	if (n > 8) return FALSE;
	*val = 0;
	if (n == 0) return TRUE;
	if (!mc_read(mc, buf, n)) return FALSE;
	*val = rd_be(buf, (int)n);
	return TRUE;
}

///
/// parse the ipco property container: reads av1c, ispe, auxc properties
/// and returns their content.  We only need the child box types and their
/// content for the primary item's property list (ipma lookup).

struct ParsedProp
{
	UBYTE *pp_Data;    /* box payload, pointing into the meta MemCtx */
	ULONG  pp_Length;  /* payload byte count */
	UBYTE  pp_Type[4]; /* 4CC box type */
};

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

static BOOL parse_ftyp(struct MemCtx *mc, ULONG box_size)
{
	UBYTE major[4];
	UBYTE compat[4];
	BOOL found = FALSE;

	if (box_size < 8) return FALSE;

	/* major_brand */
	if (!mc_read(mc, major, 4)) return FALSE;
	if (mc_read(mc, compat, 4)) /* minor_version, 4 bytes */
	{
		/* accept the AVIF major brands directly */
		if (!memcmp(major, "avif", 4) || !memcmp(major, "avis", 4) ||
		    !memcmp(major, "avio", 4) || !memcmp(major, "mif1", 4))
		{
			found = TRUE;
		}
	}

	/* compatible_brands */
	while (mc_remaining(mc) >= 4)
	{
		if (!mc_read(mc, compat, 4)) break;
		if (!memcmp(compat, "avif", 4) || !memcmp(compat, "avis", 4))
		{
			found = TRUE;
		}
	}

	return found;
}

///
/// parse_ipco - walk the ipco property container and record one entry per
/// child box.
///
/// Every child must be recorded, including types we do not interpret: ipma
/// property indices are 1-based over all ipco children, so skipping unknown
/// boxes would shift every subsequent index and attach the wrong property
/// to the item.
///
static BOOL parse_ipco(struct MemCtx *mc, struct ParsedProp *props, ULONG *num_props)
{
	while (mc_remaining(mc) >= 8)
	{
		UBYTE child_type[4];
		UQUAD child_size, child_payload;
		ULONG child_hdr, child_start;

		if (!mc_read_box_header(mc, child_type, &child_size,
		                        &child_hdr, &child_payload))
		{
			return FALSE;
		}
		child_start = mc_tell(mc);

		if (*num_props < AVIF_MAX_PROPERTIES)
		{
			ULONG prop_idx = *num_props;
			memcpy(props[prop_idx].pp_Type, child_type, 4);
			props[prop_idx].pp_Data   = mc->mc_Data + child_start;
			props[prop_idx].pp_Length = (ULONG)child_payload;
			(*num_props)++;
		}
		else
		{
			MLOGV(LOG_WARN, "Too many ipco properties, ignoring box.");
		}

		/* advance past the whole box, header included */
		if (!mc_seek(mc, child_start + (ULONG)child_payload)) return FALSE;
	}
	return TRUE;
}

///
/// Validate a parsed extent against the stream bounds.

static BOOL extent_is_valid(UQUAD offset, UQUAD length, UQUAD stream_length)
{
	if (offset > stream_length) return FALSE;
	if (length > stream_length - offset) return FALSE;
	return TRUE;
}

///
/// Does the elementary stream already carry an AV1 sequence header OBU?
///
/// The item data in a well formed AVIF file starts with a temporal delimiter
/// followed by a sequence header OBU, so the stream is self contained and
/// nothing has to be prepended.  When the header is missing we cannot invent
/// one from av1C (av1C holds an AV1CodecConfigurationRecord, not an OBU), so
/// we report the condition and let the decoder try anyway.
///
static BOOL es_has_sequence_header(const UBYTE *buf, ULONG len)
{
	ULONG pos = 0;
	ULONG seen = 0;

	while (pos < len && seen < 8)
	{
		ULONG obu_size = 0;
		ULONG used;
		UBYTE hdr;
		UWORD type;

		hdr = buf[pos];
		pos++;

		if (hdr & 0x80) return FALSE;               /* forbidden bit set */
		type = (UWORD)((hdr >> 3) & 0x0F);

		if (hdr & 0x02)
		{
			used = parse_obu_size(buf + pos, len - pos, &obu_size);
			if (!used) return FALSE;
			pos += used;
		}
		else
		{
			obu_size = len - pos;
		}

		if (obu_size > len - pos) return FALSE;

		if (type == AV1_OBU_SEQUENCE_HEADER) return TRUE;

		pos += (ULONG)obu_size;
		seen++;
	}
	return FALSE;
}

///
/// Build the elementary stream for the primary image item.
///
/// The AV1 bitstream is stored verbatim in the item's extents; the av1C box
/// is a codec configuration record and is *not* an OBU, so it must never be
/// prepended to the item data.
///
static BOOL build_elementary_stream(Class *cl, Object *obj, struct ObjData *d,
                                    UQUAD item_id, UBYTE **out_buf,
                                    ULONG *out_len, LONG *err)
{
	struct ItemDesc *item = NULL;
	ULONG i;
	UQUAD stream_length = MediaGetPort64(obj, 0, MMA_StreamLength);
	UQUAD total = 0;
	ULONG es_buf_size;
	ULONG es_pos = 0;

	*err = 0;
	*out_buf = NULL;
	*out_len = 0;

	/* Find the item */
	for (i = 0; i < d->od_NumItems; i++)
	{
		if (d->od_Items[i].id_ItemID == item_id)
		{
			item = &d->od_Items[i];
			break;
		}
	}

	if (!item)
	{
		MLOG(LOG_ERRORS, "Item %lu not found in item list.", (ULONG)item_id);
		*err = MMERR_WRONG_DATA;
		return FALSE;
	}

	if (!item->id_HasLocation || item->id_NumExtents == 0)
	{
		MLOG(LOG_ERRORS, "Item %lu has no location info.", (ULONG)item_id);
		*err = MMERR_WRONG_DATA;
		return FALSE;
	}

	/* Validate every extent and sum the lengths before allocating anything. */
	for (i = 0; i < item->id_NumExtents; i++)
	{
		UQUAD offset = item->id_Extents[i].ie_Offset;
		UQUAD length = item->id_Extents[i].ie_Length;

		if (!extent_is_valid(offset, length, stream_length))
		{
			MLOG(LOG_ERRORS, "Extent %lu out of stream bounds.", i);
			*err = MMERR_WRONG_DATA;
			return FALSE;
		}

		if (length > (UQUAD)AVIF_MAX_ES_SIZE ||
		    total > (UQUAD)AVIF_MAX_ES_SIZE - length)
		{
			MLOG(LOG_ERRORS, "Elementary stream too large.");
			*err = MMERR_OUT_OF_MEMORY;
			return FALSE;
		}
		total += length;
	}

	if (total == 0)
	{
		MLOG(LOG_ERRORS, "Primary item has zero length.");
		*err = MMERR_WRONG_DATA;
		return FALSE;
	}

	es_buf_size = (ULONG)total;

	*out_buf = (UBYTE*)MediaAllocVec(es_buf_size);
	if (!*out_buf)
	{
		MLOG(LOG_ERRORS, "Out of memory for elementary stream.");
		*err = MMERR_OUT_OF_MEMORY;
		return FALSE;
	}

	/* Copy the extent data out of the file */
	for (i = 0; i < item->id_NumExtents; i++)
	{
		UQUAD offset = item->id_Extents[i].ie_Offset;
		ULONG  length = (ULONG)item->id_Extents[i].ie_Length;

		if (length == 0) continue;

		if (DoMethod(obj, MMM_Seek, 0, MMM_SEEK_BYTES, &offset) != 1)
		{
			MLOG(LOG_ERRORS, "Seek to extent %lu failed.", i);
			goto fail_io;
		}

		if (DoMethod(obj, MMM_Pull, 0, (ULONG)(*out_buf + es_pos), length) != length)
		{
			MLOG(LOG_ERRORS, "Short read on extent %lu.", i);
			goto fail_io;
		}
		es_pos += length;
	}

	*out_len = es_pos;

	/*
	 * Only av01 items carry an OBU stream.  The same helper is used to
	 * read other items (for example an XMP 'mime' item), where the
	 * absence of a sequence header is expected rather than suspicious.
	 */
	if (item->id_IsAv01 && !es_has_sequence_header(*out_buf, *out_len))
	{
		MLOGV(LOG_WARN, "av01 item has no sequence header OBU.");
	}

	return TRUE;

fail_io:
	MediaFreeVec(*out_buf);
	*out_buf = NULL;
	*out_len = 0;
	*err = MMERR_IO_ERROR;
	return FALSE;
}

///
/// GetHeader() - parse AVIF / ISO BMFF

///
/// Locate (or create) the item descriptor for an item ID.

static struct ItemDesc *find_item(struct ObjData *d, UQUAD item_id, BOOL create)
{
	UWORD i;

	for (i = 0; i < d->od_NumItems; i++)
	{
		if (d->od_Items[i].id_ItemID == item_id) return &d->od_Items[i];
	}

	if (!create || d->od_NumItems >= AVIF_MAX_ITEMS) return NULL;

	i = d->od_NumItems;
	memset(&d->od_Items[i], 0, sizeof(struct ItemDesc));
	d->od_Items[i].id_ItemID = item_id;
	d->od_NumItems++;
	return &d->od_Items[i];
}

///
/// Release everything GetHeader() may have allocated.  Called both before a
/// parse and on failure, so a repeated GetHeader() cannot leak.

static void free_header_data(struct ObjData *d)
{
	if (d->od_ES) { MediaFreeVec(d->od_ES); d->od_ES = NULL; }
	if (d->od_AlphaES) { MediaFreeVec(d->od_AlphaES); d->od_AlphaES = NULL; }
	if (d->od_Info) { MediaFreeVec(d->od_Info); d->od_Info = NULL; }
	d->od_ESLength = 0;
	d->od_ESPos = 0;
	d->od_AlphaESLength = 0;
	d->od_NumMeta = 0;
	memset(&d->od_Meta, 0, sizeof(d->od_Meta));

	/*
	 * Reset the parsed container state as well.  GetHeader() can run more
	 * than once on the same object (Setup() on port 0 is not guaranteed to
	 * be called exactly once), and stale items or a stale primary item ID
	 * would otherwise survive into the next parse.
	 */
	d->od_NumItems = 0;
	memset(d->od_Items, 0, sizeof(d->od_Items));
	d->od_PrimaryItemID = 0;
	d->od_HavePrimaryItemID = FALSE;
	d->od_AlphaItemID = 0;
	d->od_HaveAlphaItem = FALSE;
	d->od_AlphaPremul = FALSE;
	d->od_FrameCount = 1;
	d->od_Width = 0;
	d->od_Height = 0;
	d->od_BitsPerPixel = 0;
	d->od_UseAlpha = FALSE;
	d->od_Monochrome = FALSE;
	d->od_ChromaSubX = 0;
	d->od_ChromaSubY = 0;
	d->od_HighBitdepth = FALSE;
	d->od_TwelveBit = FALSE;
	d->od_HaveNclx = FALSE;
	d->od_ColourPrimaries = 0;
	d->od_TransferCharacteristics = 0;
	d->od_MatrixCoefficients = 0;
	d->od_FullRangeFlag = FALSE;
	d->od_Av1cLength = 0;
}

///
/// parse 'pitm' - primary item reference

static BOOL parse_pitm(struct MemCtx *mc, struct ObjData *d)
{
	UBYTE ver_flags[4];
	UQUAD id = 0;

	if (mc_remaining(mc) < 4) return FALSE;
	if (!mc_read(mc, ver_flags, 4)) return FALSE;

	if (ver_flags[0] >= 2)
	{
		if (!mc_read_uint(mc, 4, &id)) return FALSE;
	}
	else
	{
		if (!mc_read_uint(mc, 2, &id)) return FALSE;
	}

	d->od_PrimaryItemID = id;
	d->od_HavePrimaryItemID = TRUE;
	return TRUE;
}

///
/// read a NUL terminated string into a bounded buffer

static BOOL read_string(struct MemCtx *mc, UBYTE *dst, ULONG dst_size)
{
	ULONG i = 0;

	for (;;)
	{
		UBYTE c;
		if (!mc_read(mc, &c, 1)) return FALSE;
		if (!c) break;
		if (i + 1 < dst_size) dst[i++] = c;
	}
	dst[i] = 0;
	return TRUE;
}

///
/// parse 'infe' - item type entry

static BOOL parse_infe(struct MemCtx *mc, struct ObjData *d)
{
	UBYTE ver_flags[4];
	UQUAD item_id = 0;
	ULONG version;
	struct ItemDesc *item;

	if (mc_remaining(mc) < 4) return FALSE;
	if (!mc_read(mc, ver_flags, 4)) return FALSE;
	version = ver_flags[0];

	item = NULL;

	if (version >= 2)
	{
		ULONG id_size = (version >= 3) ? 4 : 2;

		if (!mc_read_uint(mc, id_size, &item_id)) return FALSE;
		if (!mc_skip(mc, 2)) return FALSE;             /* protection_index */

		item = find_item(d, item_id, TRUE);
		if (!item) return FALSE;

		if (mc_remaining(mc) < 4) return FALSE;
		if (!mc_read(mc, item->id_ItemType, 4)) return FALSE;

		/* item_name, then content_type for 'mime' and 'uri' items */
		{
			UBYTE name[AVIF_MAX_STRING_LEN];

			if (!read_string(mc, name, sizeof(name))) return FALSE;

			if (!memcmp(item->id_ItemType, "mime", 4) ||
			    !memcmp(item->id_ItemType, "uri ", 4))
			{
				if (!read_string(mc, item->id_ContentType,
				                 sizeof(item->id_ContentType)))
				{
					return FALSE;
				}
			}
		}

		item->id_IsAv01 = !memcmp(item->id_ItemType, "av01", 4);
		return TRUE;
	}

	/* version 0 / 1: item_ID, protection_index, item_name, content_type */
	if (!mc_read_uint(mc, 2, &item_id)) return FALSE;
	item = find_item(d, item_id, TRUE);
	if (item) item->id_IsAv01 = TRUE;
	return TRUE;
}

///
/// parse 'iinf' - item information

static BOOL parse_iinf(struct MemCtx *mc, struct ObjData *d)
{
	UBYTE ver_flags[4];
	ULONG version;
	UQUAD count = 0;
	UQUAD i;

	if (mc_remaining(mc) < 4) return FALSE;
	if (!mc_read(mc, ver_flags, 4)) return FALSE;
	version = ver_flags[0];

	if (version == 0)
	{
		if (!mc_read_uint(mc, 2, &count)) return FALSE;
	}
	else
	{
		if (!mc_read_uint(mc, 4, &count)) return FALSE;
	}

	for (i = 0; i < count; i++)
	{
		UBYTE type[4];
		UQUAD box_size, payload;
		ULONG hdr_size, box_end;

		if (!mc_read_box_header(mc, type, &box_size, &hdr_size, &payload)) return FALSE;
		if (memcmp(type, "infe", 4)) return FALSE;
		if (payload > mc_remaining(mc)) return FALSE;
		box_end = mc_tell(mc) + (ULONG)payload;

		if (!parse_infe(mc, d)) return FALSE;

		if (!mc_seek(mc, box_end)) return FALSE;
	}

	return TRUE;
}

///
/// parse 'iloc' - item locations

static BOOL parse_iloc(struct MemCtx *mc, struct ObjData *d, UQUAD stream_length)
{
	UBYTE ver_flags[4];
	UBYTE sizes[2];
	ULONG version, offset_size, length_size, base_offset_size, index_size;
	UQUAD count = 0, i;

	if (mc_remaining(mc) < 4) return FALSE;
	if (!mc_read(mc, ver_flags, 4)) return FALSE;
	version = ver_flags[0];

	if (mc_remaining(mc) < 2) return FALSE;
	if (!mc_read(mc, sizes, 2)) return FALSE;

	offset_size      = (sizes[0] >> 4) & 0x0F;
	length_size      = sizes[0] & 0x0F;
	base_offset_size = (sizes[1] >> 4) & 0x0F;
	index_size       = sizes[1] & 0x0F;

	if (version < 2) index_size = 0;

	if (offset_size > 8 || length_size > 8 ||
	    base_offset_size > 8 || index_size > 8) return FALSE;

	if (version < 2)
	{
		if (!mc_read_uint(mc, 2, &count)) return FALSE;
	}
	else
	{
		if (!mc_read_uint(mc, 4, &count)) return FALSE;
	}

	for (i = 0; i < count; i++)
	{
		UQUAD item_id = 0, base_offset = 0, extent_count = 0, e;
		ULONG id_size = (version >= 2) ? 4 : 2;
		ULONG construction = 0;
		struct ItemDesc *item;

		if (!mc_read_uint(mc, id_size, &item_id)) return FALSE;

		if (version >= 1)
		{
			UQUAD v;
			if (!mc_read_uint(mc, 2, &v)) return FALSE;
			construction = (ULONG)(v & 0x0F);
		}

		/* data_reference_index */
		if (!mc_skip(mc, 2)) return FALSE;

		item = find_item(d, item_id, TRUE);
		if (!item) return FALSE;

		if (!mc_read_uint(mc, base_offset_size, &base_offset)) return FALSE;
		if (!mc_read_uint(mc, 2, &extent_count)) return FALSE;

		if (construction != 0)
		{
			/*
			 * Method 0 uses absolute file offsets, which is all AVIF
			 * needs.  Method 1 resolves against an idat box we do not
			 * buffer, and method 2 resolves against the position of
			 * the iloc box itself, so both cannot be honoured without
			 * more context than we keep.  Reject them rather than
			 * silently reading from the wrong offset.
			 */
			MLOGV(LOG_WARN, "iloc item %lu: construction method %lu unsupported.",
			      (ULONG)item_id, construction);
			return FALSE;
		}

		if (extent_count > AVIF_MAX_ITEM_EXTENTS) extent_count = AVIF_MAX_ITEM_EXTENTS;

		item->id_HasLocation = TRUE;
		item->id_NumExtents = 0;

		for (e = 0; e < extent_count; e++)
		{
			UQUAD offset = 0, length = 0, tmp;
			struct ItemExtent ext;

			if (index_size > 0 && !mc_read_uint(mc, index_size, &tmp)) return FALSE;
			if (!mc_read_uint(mc, offset_size, &offset)) return FALSE;
			if (!mc_read_uint(mc, length_size, &length)) return FALSE;

			/* a zero length extent runs to the end of the file */
			if (length == 0)
			{
				length = stream_length > offset ? stream_length - offset : 0;
			}

			/* construction method 0: file offset is base + extent offset */
			if (base_offset > (UQUAD)~0ULL - offset)
			{
				MLOGV(LOG_WARN, "iloc item %lu extent %lu offset overflow.",
				      (ULONG)item_id, (ULONG)e);
				return FALSE;
			}
			offset += base_offset;

			if (!extent_is_valid(offset, length, stream_length))
			{
				MLOGV(LOG_WARN, "iloc item %lu extent %lu out of bounds.",
				      (ULONG)item_id, (ULONG)e);
				return FALSE;
			}

			ext.ie_Offset = offset;
			ext.ie_Length = length;

			if (item->id_NumExtents < AVIF_MAX_ITEM_EXTENTS)
			{
				item->id_Extents[item->id_NumExtents++] = ext;
			}
		}
	}

	return TRUE;
}

///
/// parse 'ipma' - item property association

static BOOL parse_ipma(struct MemCtx *mc, struct ObjData *d,
                       ULONG version, ULONG flags)
{
	ULONG wide;
	UQUAD count = 0, i;

	/* entry_count is 32 bits in every version of the spec */
	if (!mc_read_uint(mc, 4, &count)) return FALSE;

	wide = (flags & 1) ? 1 : 0;

	for (i = 0; i < count; i++)
	{
		UQUAD item_id = 0, a;
		UBYTE assoc_count;
		ULONG id_size = (version >= 1) ? 4 : 2;
		struct ItemDesc *item;

		if (!mc_read_uint(mc, id_size, &item_id)) return FALSE;
		if (!mc_read(mc, &assoc_count, 1)) return FALSE;

		item = find_item(d, item_id, TRUE);
		if (!item) return FALSE;

		for (a = 0; a < assoc_count; a++)
		{
			UQUAD v = 0;
			UWORD prop;

			if (wide)
			{
				if (!mc_read_uint(mc, 2, &v)) return FALSE;
				prop = (UWORD)(v & 0x7FFF);
			}
			else
			{
				UBYTE b;
				if (!mc_read(mc, &b, 1)) return FALSE;
				prop = (UWORD)(b & 0x7F);
			}

			if (!prop) continue;
			if (prop > AVIF_MAX_PROPERTIES) continue;

			if (item->id_NumProps < AVIF_MAX_ITEM_PROPS)
			{
				item->id_Props[item->id_NumProps++] = prop;
			}
		}
	}

	return TRUE;
}

///
/// parse 'iref' - item references.  We only care about 'dimg' from the
/// primary item, which points at the derived (alpha) item.

static BOOL parse_iref(struct MemCtx *mc, struct ObjData *d)
{
	UBYTE ver_flags[4];

	/* FullBox header; the version only affects the iref box itself. */
	if (mc_remaining(mc) < 4) return FALSE;
	if (!mc_read(mc, ver_flags, 4)) return FALSE;

	while (mc_remaining(mc) >= 8)
	{
		UBYTE type[4];
		UQUAD box_size, payload;
		ULONG hdr_size, box_end;
		UQUAD from_id = 0, ref_count = 0, r;
		BOOL is_dimg;

		if (!mc_read_box_header(mc, type, &box_size, &hdr_size, &payload)) return FALSE;
		if (payload > mc_remaining(mc)) return FALSE;
		box_end = mc_tell(mc) + (ULONG)payload;

		is_dimg = !memcmp(type, "dimg", 4);

		if (mc_remaining(mc) >= 4)
		{
			UBYTE inner[4];
			ULONG id_size;

			if (!mc_read(mc, inner, 4)) return FALSE;

			/*
			 * Each child box is a SingleItemTypeReference with its own
			 * version, so the item ID width comes from the child box
			 * and not from the enclosing iref version.
			 */
			id_size = (inner[0] >= 1) ? 4 : 2;

			if (!mc_read_uint(mc, id_size, &from_id)) return FALSE;
			if (!mc_read_uint(mc, 2, &ref_count)) return FALSE;

			if (is_dimg && d->od_HavePrimaryItemID && from_id == d->od_PrimaryItemID)
			{
				for (r = 0; r < ref_count; r++)
				{
					UQUAD to_id = 0;
					if (!mc_read_uint(mc, id_size, &to_id)) return FALSE;
					if (!d->od_HaveAlphaItem)
					{
						d->od_AlphaItemID = to_id;
						d->od_HaveAlphaItem = TRUE;
					}
				}
			}
		}

		if (!mc_seek(mc, box_end)) return FALSE;
	}

	return TRUE;
}

///
/// Look for a property by type among the properties associated with an item.

static const struct ParsedProp *item_prop(const struct ParsedProp *props,
                                          ULONG num_props,
                                          const struct ItemDesc *item,
                                          const char *type)
{
	UWORD i;

	for (i = 0; i < item->id_NumProps; i++)
	{
		UWORD idx = item->id_Props[i];
		if (idx == 0 || idx > num_props) continue;
		if (!memcmp(props[idx - 1].pp_Type, type, 4)) return &props[idx - 1];
	}
	return NULL;
}

///
/// parse an 'ispe' property payload (FullBox: skip version/flags)

static void parse_ispe(const struct ParsedProp *p, struct ObjData *d)
{
	ULONG w, h;

	if (p->pp_Length < 12) return;
	w = be32(p->pp_Data + 4);
	h = be32(p->pp_Data + 8);
	if (!w || !h) return;

	d->od_Width  = w;
	d->od_Height = h;
}

///
/// parse a 'pixi' property payload (FullBox, channel bit depths)

static void parse_pixi(const struct ParsedProp *p, struct ObjData *d)
{
	ULONG count, i, maxbits = 0;

	if (p->pp_Length < 6) return;
	count = p->pp_Data[4];
	if (count == 0 || 5 + count > p->pp_Length) return;

	for (i = 0; i < count; i++)
	{
		if (p->pp_Data[5 + i] > maxbits) maxbits = p->pp_Data[5 + i];
	}
	if (maxbits) d->od_BitsPerPixel = maxbits;
}

///
/// parse an 'av1C' property payload.
///
/// libavif (and the samples this plugin is tested against) write av1C as a
/// plain box whose payload starts directly with the AV1CodecConfigurationRecord:
///   marker(1) version(7) | seq_profile(3) seq_level_idx_0(5) | ...
/// Other writers treat av1C as a FullBox, so accept both layouts and pick the
/// one whose first byte carries the marker bit.
///
/// The record is *not* an OBU stream and must never be prepended to the
/// elementary stream.
///
static void parse_av1c(const struct ParsedProp *p, struct ObjData *d)
{
	const UBYTE *r;
	ULONG len;

	if (p->pp_Length < 4) return;

	if (p->pp_Data[0] & 0x80)
	{
		r = p->pp_Data;
	}
	else if (p->pp_Length >= 8 && (p->pp_Data[4] & 0x80))
	{
		r = p->pp_Data + 4;      /* FullBox layout: skip version/flags */
	}
	else
	{
		return;
	}
	len = p->pp_Length - (ULONG)(r - p->pp_Data);

	if (len < 3) return;

	/* seq_profile(3) seq_level_idx_0(5) */

	/*
	 * Third record byte, bits counted from the most significant:
	 *   seq_tier_0(1) high_bitdepth(1) twelve_bit(1) monochrome(1)
	 *   chroma_subsampling_x(1) chroma_subsampling_y(1)
	 *   chroma_sample_position(2)
	 */
	d->od_Monochrome   = (r[2] & 0x10) ? TRUE : FALSE;
	d->od_HighBitdepth = (r[2] & 0x40) ? TRUE : FALSE;
	d->od_TwelveBit    = (r[2] & 0x20) ? TRUE : FALSE;
	d->od_ChromaSubX   = (r[2] & 0x08) ? 1 : 0;
	d->od_ChromaSubY   = (r[2] & 0x04) ? 1 : 0;

	if (d->od_TwelveBit)      d->od_BitsPerPixel = 12;
	else if (d->od_HighBitdepth) d->od_BitsPerPixel = 10;
	else                      d->od_BitsPerPixel = 8;

	d->od_Av1cLength = (len < sizeof(d->od_Av1c)) ? len : sizeof(d->od_Av1c);
	memcpy(d->od_Av1c, r, d->od_Av1cLength);
}

///
/// parse an 'auxC' property payload (FullBox + NUL terminated URN)

static void parse_auxc(const struct ParsedProp *p, struct ObjData *d,
                       BOOL *is_alpha_item, BOOL *is_premul)
{
	UBYTE urn[AVIF_MAX_STRING_LEN];
	ULONG i = 0;

	if (p->pp_Length < 5) return;

	for (;;)
	{
		UBYTE c;
		if (4 + i >= p->pp_Length) return;
		c = p->pp_Data[4 + i];
		if (!c) break;
		if (i + 1 < sizeof(urn)) urn[i++] = c;
	}
	urn[i] = 0;

	if (!strcmp((char*)urn, AVIF_ALPHA_URN))
	{
		*is_alpha_item = TRUE;
	}
	else if (!strcmp((char*)urn, AVIF_ALPHA_PREMUL_URN))
	{
		*is_premul = TRUE;
	}
}

///
/// parse a 'colr' property payload (nclx colour information)
///
/// colr is a plain box: the payload starts with the colour_type 4CC, so for
/// nclx the seven bytes that follow are colour_primaries,
/// transfer_characteristics, matrix_coefficients (all u16) and a byte whose
/// top bit is the full range flag.

static void parse_colr(const struct ParsedProp *p, struct ObjData *d)
{
	const UBYTE *r;

	if (p->pp_Length < 11) return;
	r = p->pp_Data;
	if (memcmp(r, "nclx", 4)) return;     /* ICC profiles are not signalled */

	d->od_ColourPrimaries          = be16(r + 4);
	d->od_TransferCharacteristics  = be16(r + 6);
	d->od_MatrixCoefficients       = be16(r + 8);
	d->od_FullRangeFlag            = (r[10] & 0x80) ? TRUE : FALSE;
	d->od_HaveNclx                 = TRUE;
}

///
/// pull the payload of an item into a NUL terminated heap buffer

static UBYTE *read_item_string(Class *cl, Object *obj, struct ObjData *d,
                               struct ItemDesc *item, ULONG *out_len)
{
	UBYTE *buf = NULL;
	ULONG len = 0;
	UBYTE *tmp;
	LONG err = 0;

	*out_len = 0;

	if (!build_elementary_stream(cl, obj, d, item->id_ItemID, &buf, &len, &err))
	{
		return NULL;
	}

	/* make room for a terminator */
	tmp = (UBYTE*)MediaAllocVec(len + 1);
	if (!tmp)
	{
		MediaFreeVec(buf);
		return NULL;
	}
	memcpy(tmp, buf, len);
	tmp[len] = 0;
	MediaFreeVec(buf);

	*out_len = len;
	return tmp;
}

///
/// copy a string into the metadata slot for a tag

static void meta_add(struct ObjData *d, ULONG tag, const UBYTE *s, ULONG len)
{
	ULONG i;

	if (!len || len >= AVIF_MAX_STRING_LEN) return;

	for (i = 0; i < d->od_NumMeta; i++)
	{
		if (d->od_MetaTags[i] == tag) break;
	}
	if (i >= AVIF_MAX_META_ENTRIES) return;

	memcpy(d->od_MetaStrings[i], s, len);
	d->od_MetaStrings[i][len] = 0;
	d->od_MetaTags[i] = tag;
	if (i == d->od_NumMeta) d->od_NumMeta++;
}

///
/// strip leading and trailing whitespace from a metadata value

static void meta_trim(const UBYTE **s, ULONG *len)
{
	while (*len && (**s == ' ' || **s == '\t' || **s == '\r' || **s == '\n'))
	{
		(*s)++;
		(*len)--;
	}
	while (*len)
	{
		UBYTE c = (*s)[*len - 1];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
		(*len)--;
	}
}

///
/// find "<rdf:li" starting at or after *pos, leaving *pos on the tag

static BOOL find_rdf_li(const UBYTE *xmp, ULONG len, ULONG *pos)
{
	while (*pos + 7 <= len)
	{
		const UBYTE *lt = (const UBYTE*)memchr(xmp + *pos, '<', len - *pos);
		ULONG at;

		if (!lt) return FALSE;
		at = (ULONG)(lt - xmp);

		if (at + 8 <= len && !memcmp(xmp + at, "<rdf:li", 8))
		{
			*pos = at;
			return TRUE;
		}
		if (at + 2 <= len && xmp[at + 1] == '/') return FALSE;  /* </something> */
		*pos = at + 1;
	}
	return FALSE;
}

///
/// pull title, copyright and description out of an XMP payload
///
/// Only the handful of Dublin Core properties we advertise is looked for, in
/// the two forms libavif and Photoshop emit:
///
///   <rdf:Description dc:title="Hello" .../>
///   <dc:title><rdf:Alt><rdf:li xml:lang="x-default">Hello</rdf:li></rdf:Alt></dc:title>
///
static void parse_xmp(struct ObjData *d, const UBYTE *xmp, ULONG len)
{
	static const struct
	{
		const char *elem;
		ULONG tag;
	} wanted[] =
	{
		{ "dc:title",       AVIF_META_TITLE },
		{ "dc:rights",      AVIF_META_COPYRIGHT },
		{ "dc:description", AVIF_META_DESCRIPTION }
	};
	ULONG w;

	for (w = 0; w < (sizeof(wanted) / sizeof(wanted[0])); w++)
	{
		ULONG nlen = (ULONG)strlen(wanted[w].elem);
		ULONG pos = 0;

		while (pos + nlen <= len)
		{
			const UBYTE *hit;
			ULONG at, after;
			BOOL done = FALSE;

			hit = (const UBYTE*)memchr(xmp + pos, wanted[w].elem[0], len - pos);
			if (!hit) break;
			at = (ULONG)(hit - xmp);

			if (at + nlen > len || memcmp(xmp + at, wanted[w].elem, nlen))
			{
				pos = at + 1;
				continue;
			}
			after = at + nlen;

			/* attribute form: dc:title="value" */
			if (after < len && xmp[after] == '=')
			{
				ULONG q = after + 1;
				if (q < len && (xmp[q] == '"' || xmp[q] == '\''))
				{
					UBYTE quote = xmp[q];
					ULONG vstart = q + 1;
					ULONG vlen = 0;

					while (vstart + vlen < len && xmp[vstart + vlen] != quote)
					{
						vlen++;
					}
					if (vstart + vlen < len)
					{
						meta_add(d, wanted[w].tag, xmp + vstart, vlen);
						pos = vstart + vlen + 1;
						done = TRUE;
					}
				}
			}

			/* element form: <dc:title> ... <rdf:li ...>value</rdf:li> ... */
			if (!done && after < len && xmp[after] == '>')
			{
				ULONG li = after + 1;

				if (find_rdf_li(xmp, len, &li))
				{
					/* skip past the rest of the <rdf:li ...> tag */
					ULONG gt = li;
					while (gt + 1 < len && xmp[gt] != '>') gt++;
					if (gt + 1 < len)
					{
						ULONG vstart = gt + 1;
						const UBYTE *close;
						ULONG vend;
						const UBYTE *vs;
						ULONG vl;

						close = (const UBYTE*)memchr(xmp + vstart, '<', len - vstart);
						if (!close) close = xmp + len;
						vend = (ULONG)(close - xmp);

						vs = xmp + vstart;
						vl = vend - vstart;
						meta_trim(&vs, &vl);
						if (vl)
						{
							meta_add(d, wanted[w].tag, vs, vl);
							pos = vend;
							done = TRUE;
						}
					}
				}
				if (!done) pos = after + 1;
			}
		}
	}
}

///
///
/// GetHeader() - parse AVIF / ISO BMFF

BOOL GetHeader(Class *cl, Object *obj)
{
	GET_DATA;
	struct ObjData *pd = d;
	UQUAD stream_length = MediaGetPort64(obj, 0, MMA_StreamLength);
	UQUAD pos = 0, meta_start = 0;
	UQUAD meta_size = 0;
	BOOL have_ftyp = FALSE;
	struct ParsedProp props[AVIF_MAX_PROPERTIES];
	ULONG num_props = 0;
	struct MemCtx mc_meta;
	ULONG i;
	LONG err = 0;

	MLOGV(LOG_INFO, "Parsing AVIF, stream length %Ld.", stream_length);

	/* any previous parse result is dropped first, so a repeated Setup(0)
	   cannot leak the elementary stream or the AvifInfo copy */
	free_header_data(pd);

	if (stream_length < 16)
	{
		seterr(MMERR_END_OF_DATA);
		MLOG(LOG_ERRORS, "AVIF file too small.");
		return FALSE;
	}

	/* walk the top level boxes looking for ftyp and meta */
	while (pos + 8 <= stream_length)
	{
		UBYTE hdr[16];
		UQUAD box_size;
		UBYTE box_type[4];
		ULONG avail = (stream_length - pos) > 16 ? 16 : (ULONG)(stream_length - pos);

		if (DoMethod(obj, MMM_Seek, 0, MMM_SEEK_BYTES, &pos) != 1)
		{
			seterr(MMERR_IO_ERROR);
			MLOG(LOG_ERRORS, "Seek failed.");
			return FALSE;
		}
		memset(hdr, 0, sizeof(hdr));
		if (DoMethod(obj, MMM_Pull, 0, (ULONG)hdr, avail) != avail) break;

		box_size = be32(hdr);
		memcpy(box_type, hdr + 4, 4);

		if (box_size == 1)
		{
			if (avail < 16) break;
			box_size = be64(hdr + 8);
		}
		else if (box_size == 0)
		{
			box_size = stream_length - pos;
		}

		if (box_size < 8 || box_size > stream_length - pos) break;

		if (!memcmp(box_type, "ftyp", 4))
		{
			/*
			 * An AVIF file is only interesting if the brand says so.
			 * The major brand alone is not enough: files carrying
			 * 'mif1' as the major brand list 'avif' or 'avis' in
			 * the compatible brands.
			 */
			if (box_size <= AVIF_MAX_META_SIZE)
			{
				struct MemCtx mc_ftyp;

				if (mc_init(&mc_ftyp, obj, pos, (ULONG)box_size) &&
				    mc_seek(&mc_ftyp, 8) &&
				    parse_ftyp(&mc_ftyp, (ULONG)box_size - 8))
				{
					have_ftyp = TRUE;
				}
				mc_free(&mc_ftyp);
			}
		}
		else if (!memcmp(box_type, "meta", 4))
		{
			meta_start = pos;
			meta_size  = box_size;
		}

		pos += box_size;
	}

	if (!have_ftyp)
	{
		seterr(MMERR_WRONG_DATA);
		MLOG(LOG_ERRORS, "No ftyp box found.");
		return FALSE;
	}

	if (!meta_size)
	{
		seterr(MMERR_WRONG_DATA);
		MLOG(LOG_ERRORS, "No meta box found.");
		return FALSE;
	}

	if (meta_size > AVIF_MAX_META_SIZE)
	{
		seterr(MMERR_OUT_OF_MEMORY);
		MLOG(LOG_ERRORS, "meta box too large (%Ld bytes).", meta_size);
		return FALSE;
	}

	if (!mc_init(&mc_meta, obj, meta_start, (ULONG)meta_size))
	{
		seterr(MMERR_OUT_OF_MEMORY);
		MLOG(LOG_ERRORS, "Cannot buffer meta box.");
		return FALSE;
	}

	/* 'meta' is a FullBox: 4 bytes of version/flags precede the children */
	if (!mc_skip(&mc_meta, 12))
	{
		mc_free(&mc_meta);
		seterr(MMERR_WRONG_DATA);
		MLOG(LOG_ERRORS, "Truncated meta box.");
		return FALSE;
	}

	/* first pass: parse the boxes we can resolve independently of the
	   others.  iinf may legally appear before or after iloc and ipma,
	   so item records are keyed by item ID instead of by position. */
	while (mc_remaining(&mc_meta) >= 8)
	{
		UBYTE type[4];
		UQUAD box_size, payload;
		ULONG hdr_size, box_end;
		BOOL ok = TRUE;

		if (!mc_read_box_header(&mc_meta, type, &box_size, &hdr_size, &payload))
		{
			ok = FALSE;
		}
		else
		{
			box_end = mc_tell(&mc_meta) + (ULONG)payload;

			if (payload > mc_remaining(&mc_meta))
			{
				ok = FALSE;
			}
			else if (!memcmp(type, "pitm", 4))
			{
				ok = parse_pitm(&mc_meta, pd);
			}
			else if (!memcmp(type, "iinf", 4))
			{
				ok = parse_iinf(&mc_meta, pd);
			}
			else if (!memcmp(type, "iloc", 4))
			{
				ok = parse_iloc(&mc_meta, pd, stream_length);
			}
			else if (!memcmp(type, "iref", 4))
			{
				ok = parse_iref(&mc_meta, pd);
			}
			else if (!memcmp(type, "iprp", 4))
			{
				/* iprp holds ipco and one or more ipma boxes */
				while (mc_tell(&mc_meta) < box_end && ok)
				{
					UBYTE ptype[4];
					UQUAD psize, ppayload;
					ULONG phdr, pend;

					if (!mc_read_box_header(&mc_meta, ptype, &psize,
					                       &phdr, &ppayload))
					{
						ok = FALSE;
						break;
					}
					if (ppayload > mc_remaining(&mc_meta))
					{
						ok = FALSE;
						break;
					}
					pend = mc_tell(&mc_meta) + (ULONG)ppayload;

					if (!memcmp(ptype, "ipco", 4))
					{
						ok = parse_ipco(&mc_meta, props, &num_props);
					}
					else if (!memcmp(ptype, "ipma", 4))
					{
						UBYTE vf[4];
						if (!mc_read(&mc_meta, vf, 4)) { ok = FALSE; break; }
						ok = parse_ipma(&mc_meta, pd, vf[0], vf[3] & 1);
					}

					if (!mc_seek(&mc_meta, pend)) { ok = FALSE; break; }
				}
			}

			if (!mc_seek(&mc_meta, box_end)) ok = FALSE;
		}

		if (!ok)
		{
			MLOG(LOG_ERRORS, "Corrupt meta box.");
			mc_free(&mc_meta);
			seterr(MMERR_WRONG_DATA);
			free_header_data(pd);
			return FALSE;
		}
	}

	/*
	 * The property payload pointers inside props[] point into mc_meta, so
	 * the meta buffer must stay alive until the property resolution below
	 * has finished.
	 */
	if (!pd->od_HavePrimaryItemID)
	{
		seterr(MMERR_WRONG_DATA);
		MLOG(LOG_ERRORS, "No primary item reference.");
		mc_free(&mc_meta);
		free_header_data(pd);
		return FALSE;
	}

	/* resolve the properties of the primary item */
	{
		struct ItemDesc *item = find_item(pd, pd->od_PrimaryItemID, FALSE);
		const struct ParsedProp *p;

		if (!item || !item->id_IsAv01)
		{
			seterr(MMERR_WRONG_DATA);
			MLOG(LOG_ERRORS, "Primary item %lu is missing or not AV1.",
			      (ULONG)pd->od_PrimaryItemID);
			mc_free(&mc_meta);
			free_header_data(pd);
			return FALSE;
		}

		p = item_prop(props, num_props, item, "ispe");
		if (p) parse_ispe(p, pd);

		p = item_prop(props, num_props, item, "pixi");
		if (p) parse_pixi(p, pd);

		p = item_prop(props, num_props, item, "av1C");
		if (p) parse_av1c(p, pd);

		p = item_prop(props, num_props, item, "colr");
		if (p) parse_colr(p, pd);

		p = item_prop(props, num_props, item, "auxC");
		if (p)
		{
			BOOL is_alpha = FALSE, is_premul = FALSE;
			parse_auxc(p, pd, &is_alpha, &is_premul);
			if (is_premul) pd->od_AlphaPremul = TRUE;
		}

		if (!pd->od_Width || !pd->od_Height)
		{
			seterr(MMERR_WRONG_DATA);
			MLOG(LOG_ERRORS, "Primary item has no ispe property.");
			mc_free(&mc_meta);
			free_header_data(pd);
			return FALSE;
		}
	}

	/* the alpha item, if any, carries an auxC property with the alpha URN */
	for (i = 0; i < pd->od_NumItems; i++)
	{
		struct ItemDesc *it = &pd->od_Items[i];
		const struct ParsedProp *p;
		BOOL is_alpha = FALSE, is_premul = FALSE;

		if (it->id_ItemID == pd->od_PrimaryItemID) continue;

		p = item_prop(props, num_props, it, "auxC");
		if (!p) continue;

		parse_auxc(p, pd, &is_alpha, &is_premul);

		if (is_alpha && !pd->od_HaveAlphaItem)
		{
			pd->od_AlphaItemID = it->id_ItemID;
			pd->od_HaveAlphaItem = TRUE;
		}
		if (is_premul) pd->od_AlphaPremul = TRUE;
	}

	/*
	 * Metadata lives in a separate item ("mime" with an rdf/xml content
	 * type, referenced by an iref "cdsc" from the primary item).  Find it
	 * by content type, which does not depend on the item ordering.
	 */
	for (i = 0; i < pd->od_NumItems; i++)
	{
		struct ItemDesc *it = &pd->od_Items[i];
		UBYTE *payload;
		ULONG plen = 0;

		if (memcmp(it->id_ItemType, "mime", 4)) continue;
		if (strncmp((char*)it->id_ContentType, "application/rdf+xml",
		            sizeof("application/rdf+xml") - 1)) continue;
		if (!it->id_HasLocation) continue;

		payload = read_item_string(cl, obj, pd, it, &plen);
		if (!payload) continue;

		parse_xmp(pd, payload, plen);
		MediaFreeVec(payload);
		break;
	}

	/* the property payloads live inside the meta buffer, so it can go now */
	mc_free(&mc_meta);

	pd->od_UseAlpha = pd->od_HaveAlphaItem;
	pd->od_FrameCount = 1;

	/* assemble the colour plane elementary stream */
	if (!build_elementary_stream(cl, obj, pd, pd->od_PrimaryItemID, &pd->od_ES,
	                             &pd->od_ESLength, &err))
	{
		seterr(err ? err : MMERR_WRONG_DATA);
		free_header_data(pd);
		return FALSE;
	}

	/* and the alpha plane, when the image has one */
	if (pd->od_HaveAlphaItem)
	{
		struct ItemDesc *ait = find_item(pd, pd->od_AlphaItemID, FALSE);
		if (!ait || !ait->id_HasLocation)
		{
			seterr(MMERR_WRONG_DATA);
			MLOG(LOG_ERRORS, "Alpha item %lu has no location.",
			      (ULONG)pd->od_AlphaItemID);
			free_header_data(pd);
			return FALSE;
		}
		if (!build_elementary_stream(cl, obj, pd, pd->od_AlphaItemID,
		                             &pd->od_AlphaES, &pd->od_AlphaESLength, &err))
		{
			seterr(err ? err : MMERR_WRONG_DATA);
			free_header_data(pd);
			return FALSE;
		}
	}

	pd->od_Info = (struct AvifInfo*)MediaAllocVec(sizeof(struct AvifInfo));
	if (!pd->od_Info)
	{
		seterr(MMERR_OUT_OF_MEMORY);
		free_header_data(pd);
		return FALSE;
	}
	memset(pd->od_Info, 0, sizeof(struct AvifInfo));

	pd->od_Info->ai_Width         = pd->od_Width;
	pd->od_Info->ai_Height        = pd->od_Height;
	pd->od_Info->ai_BitsPerPixel  = pd->od_BitsPerPixel;
	pd->od_Info->ai_UseAlpha      = pd->od_UseAlpha;
	pd->od_Info->ai_AlphaPremul   = pd->od_AlphaPremul;
	pd->od_Info->ai_Monochrome    = pd->od_Monochrome;
	pd->od_Info->ai_ChromaSubX    = pd->od_ChromaSubX;
	pd->od_Info->ai_ChromaSubY    = pd->od_ChromaSubY;
	pd->od_Info->ai_ES            = pd->od_ES;
	pd->od_Info->ai_ESLength      = pd->od_ESLength;
	pd->od_Info->ai_AlphaES       = pd->od_AlphaES;
	pd->od_Info->ai_AlphaESLength = pd->od_AlphaESLength;
	pd->od_Info->ai_Av1cLength    = pd->od_Av1cLength;
	if (pd->od_Av1cLength)
	{
		memcpy(pd->od_Info->ai_Av1c, pd->od_Av1c, pd->od_Av1cLength);
	}
	/* publish the metadata we managed to extract */
	for (i = 0; i < pd->od_NumMeta; i++)
	{
		pd->od_Meta.amd_Entries[pd->od_Meta.amd_Count].ame_Tag =
			pd->od_MetaTags[i];
		pd->od_Meta.amd_Entries[pd->od_Meta.amd_Count].ame_String =
			pd->od_MetaStrings[i];
		pd->od_Meta.amd_Count++;
	}

	if (pd->od_HaveNclx)
	{
		pd->od_Info->ai_ColourPrimaries         = pd->od_ColourPrimaries;
		pd->od_Info->ai_TransferCharacteristics = pd->od_TransferCharacteristics;
		pd->od_Info->ai_MatrixCoefficients      = pd->od_MatrixCoefficients;
		pd->od_Info->ai_FullRangeFlag           = pd->od_FullRangeFlag;
		pd->od_Info->ai_HaveNclx                = 1;
	}

	MLOGV(LOG_INFO, "AVIF: %ld x %ld, %ld-bit, alpha=%ld, ES=%ld bytes.",
	      pd->od_Width, pd->od_Height, pd->od_BitsPerPixel,
	      pd->od_UseAlpha, pd->od_ESLength);

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
		d->od_ESPos = 0;
		d->od_AlphaES = NULL;
		d->od_AlphaESLength = 0;
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
	if (d->od_AlphaES) MediaFreeVec(d->od_AlphaES);
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
			/*
			 * The multimedia framework stores this attribute in a
			 * LONG on MorphOS, so write only the low word and never
			 * a UQUAD, which would overrun the caller's storage.
			 */
			/* AVIF images are single-frame stills */
			*msg->opg_Storage = (LONG)d->od_FrameCount;
			return TRUE;

		case MMA_DataFormat:
			*msg->opg_Storage = (LONG)"AVIF";
			return TRUE;

		case MMA_MediaType:
			*msg->opg_Storage = MMT_PICTURE;
			return TRUE;

		case MMA_ExtraData:
			/*
			 * Hand out a pointer to our own bookkeeping struct.  The
			 * decoder copies what it needs, and this keeps the
			 * struct stable for the lifetime of the object.
			 */
			*msg->opg_Storage = (LONG)d->od_Info;
			return TRUE;

		case AVIF_META_DATA:
			*msg->opg_Storage = (LONG)&d->od_Meta;
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
			{
				/*
				 * Serve the assembled AV1 elementary stream.  The
				 * read position lives in od_ESPos rather than being
				 * advanced inside the AvifInfo handed out through
				 * MMA_ExtraData, so a second Get of that attribute
				 * still sees the full stream.
				 */
				ULONG avail;

				if (!d->od_ES || d->od_ESLength == 0)
				{
					seterr(MMERR_END_OF_DATA);
					break;
				}

				avail = d->od_ESLength - d->od_ESPos;
				if (!avail)
				{
					seterr(MMERR_END_OF_DATA);
					break;
				}

				if (msg->Length < avail) avail = msg->Length;

				memcpy(msg->Buffer, d->od_ES + d->od_ESPos, avail);
				d->od_ESPos += avail;
				bytes_pulled = avail;
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
		case MMA_Video_FrameCount:
		case MMA_DataFormat:
		case MMA_ExtraData:
		case MMA_MediaType:
		case AVIF_META_DATA:
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