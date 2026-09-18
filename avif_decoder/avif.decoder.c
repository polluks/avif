/// autodoc

/****** avif.decoder/background *********************************************
*
* DESCRIPTION
*   Decoder class for AVIF (AV1 Image File Format) images.
*   Consumes the AV1 elementary stream from avif.demuxer (port 0 input late
*   MMF_VIDEO_AVIF) and produces MMFC_VIDEO_ARGB32 on port 1.
*   Uses the bundled dav1d AV1 decoder.
*
* NEW ATTRIBUTES
*   MMA_Video_Width       (V1)  [..G.Q], ULONG
*   MMA_Video_Height      (V1)  [..G.Q], ULONG
*   MMA_Video_BitsPerPixel(V1)  [..G.Q], ULONG
*   MMA_Video_UseAlpha    (V1)  [..G.Q], BOOL
*   MMA_DataFormat        (V1)  [..G.Q], STRPTR
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

#include <dav1d/dav1d.h>
#include "../avif_demuxer/avif.demuxer.h"

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

struct ObjData
{
	/* input format info */
	ULONG    od_Width;
	ULONG    od_Height;
	ULONG    od_BitsPerPixel;
	BOOL     od_UseAlpha;

	/* the elementary stream */
	UBYTE   *od_ES;
	ULONG    od_ESLength;

	/* decoded pixels */
	UBYTE   *od_Bitmap;
	ULONG    od_BitmapBytes;

	/* decoded picture (for repeated pulls) */
	union
	{
		struct AvifInfo *od_Info;
	} od_priv;

	struct AvifInfo od_InfoCopy;

	/* dav1d decoder */
	Dav1dContext *od_Dav1d;
	BOOL          od_DecodeDone;
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
BOOL LoadData(Class *cl, Object *obj);

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
	{QUERYINFOATTR_DESCRIPTION, (ULONG)"AVIF fileformat decoder"},
	{QUERYINFOATTR_COPYRIGHT, (ULONG)"(c) 2026"},
	{QUERYINFOATTR_AUTHOR, (ULONG)"Reggae contributors"},
	{QUERYINFOATTR_DATE, (ULONG)DATE},
	{QUERYINFOATTR_VERSION, VERSION},
	{QUERYINFOATTR_REVISION, REVISION},
	{QUERYINFOATTR_SUBTYPE, QUERYSUBTYPE_LIBRARY},
	{QUERYINFOATTR_CLASS, QUERYCLASS_MULTIMEDIA},
	{QUERYINFOATTR_SUBCLASS, QUERYSUBCLASS_MULTIMEDIA_DECODER},
	{MMA_MediaType, MMT_PICTURE},
	{MMA_SupportedFormats, (ULONG)NULL},   /* filled in data.c */
	{TAG_END, 0}
};

static const ULONG InputFormats[]  = { MMF_VIDEO_AVIF, 0 };
static const ULONG OutputFormats[] = { MMFC_VIDEO_ARGB32, 0 };

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
/// LoadData()
///
/// Reads the input format attributes (from avif.demuxer attached to the
/// input port) and pulls the whole elementary stream, then opens dav1d.

BOOL LoadData(Class *cl, Object *obj)
{
	GET_DATA;
	BOOL ok = FALSE;
	struct AvifInfo *info = (struct AvifInfo*)MediaGetPortFwd(obj, 0, MMA_ExtraData);

	if (!info)
	{
		MLOG(LOG_ERRORS, "No AvifInfo from input port.");
		return FALSE;
	}

	d->od_Width = info->ai_Width;
	d->od_Height = info->ai_Height;
	d->od_BitsPerPixel = info->ai_BitsPerPixel;
	d->od_UseAlpha = info->ai_UseAlpha;

	if (d->od_Width == 0 || d->od_Height == 0 || d->od_Width >= 0x10000 || d->od_Height >= 0x10000)
	{
		MLOG(LOG_ERRORS, "Invalid dimensions %ld x %ld.", d->od_Width, d->od_Height);
		return FALSE;
	}

	/* Pull the complete elementary stream into memory */
	d->od_ES = (UBYTE*)MediaAllocVec(info->ai_ESLength ? info->ai_ESLength : 4096);
	if (!d->od_ES)
	{
		MLOG(LOG_ERRORS, "Out of memory for elementary stream.");
		return FALSE;
	}
	d->od_ESLength = info->ai_ESLength;
	memcpy(d->od_ES, info->ai_ES, d->od_ESLength);

	/* Open dav1d decoder */
	{
		Dav1dSettings s;
		dav1d_default_settings(&s);
		s.n_threads = 1;
		s.max_frame_delay = 1;
		s.apply_grain = 0;
		s.all_layers = 1;
		s.operating_point = 0;
		s.strict_std_compliance = 0;
		s.inloop_filters = DAV1D_INLOOPFILTER_ALL;

		if (dav1d_open(&d->od_Dav1d, &s) != 0)
		{
			MLOG(LOG_ERRORS, "Unable to open dav1d decoder.");
			MediaFreeVec(d->od_ES);
			d->od_ES = NULL;
			return FALSE;
		}
	}

	ok = TRUE;
	return ok;
}

///
/// Decode() - feed the whole elementary stream to dav1d, decode the
/// first picture, convert YUV->ARGB32.

static unsigned clip255(LONG v)
{
	return (unsigned)((v < 0) ? 0 : (v > 255) ? 255 : v);
}

///
/// YUV -> ARGB32 conversion.
/// Supports 8/10/12 bit, 4:2:0 / 4:2:2 / 4:4:4 / 4:0:0 (mono).
/// Matrix coefficients from the sequence header: 1 (BT.709),
/// 5/6 (BT.601/BT.470), 9 (BT.2020). Falls back to BT.601.

static BOOL yuv_to_argb32(const Dav1dPicture *pic, UBYTE *argb)
{
	unsigned i, j;
	const ptrdiff_t stride_y = pic->stride[0];
	const ptrdiff_t stride_c = pic->stride[1];
	const int w = pic->p.w;
	const int h = pic->p.h;
	const int bpc = pic->p.bpc;
	const int shift = (bpc > 8) ? (bpc - 8) : 0;
	const int is16 = (bpc > 8);
	int ss_x = 0, ss_y = 0;
	int use_bt709 = 0, use_bt2020 = 0;

	if (!argb || !pic->data[0]) return FALSE;

	/* chroma subsampling from the pixel layout */
	switch (pic->p.layout)
	{
		case DAV1D_PIXEL_LAYOUT_I420: ss_x = 1; ss_y = 1; break;
		case DAV1D_PIXEL_LAYOUT_I422: ss_x = 1; ss_y = 0; break;
		case DAV1D_PIXEL_LAYOUT_I444: ss_x = 0; ss_y = 0; break;
		default: break; /* I400: monochrome, no chroma */
	}

	/* matrix coefficients */
	if (pic->seq_hdr)
	{
		switch (pic->seq_hdr->mtrx)
		{
			case DAV1D_MC_BT709:  use_bt709 = 1;  break;
			case DAV1D_MC_BT2020_NCL:
			case DAV1D_MC_BT2020_CL: use_bt2020 = 1; break;
			default: break;
		}
	}

	for (i = 0; i < (unsigned)h; i++)
	{
		ULONG *dst = (ULONG*)argb + (i * (ULONG)w);
		for (j = 0; j < (unsigned)w; j++)
		{
			int y, cu = 128, cv = 128;
			int r, g, b;

			if (is16)
			{
				const uint16_t *py = (const uint16_t*)pic->data[0];
				y = (int)(py[(ptrdiff_t)i * stride_y + (ptrdiff_t)j] >> shift);
				if (pic->data[1])
				{
					const uint16_t *pu = (const uint16_t*)pic->data[1];
					const uint16_t *pv = (const uint16_t*)pic->data[2];
					cu = pu[((ptrdiff_t)i >> ss_y) * stride_c + ((ptrdiff_t)j >> ss_x)] >> shift;
					cv = pv[((ptrdiff_t)i >> ss_y) * stride_c + ((ptrdiff_t)j >> ss_x)] >> shift;
				}
			}
			else
			{
				const uint8_t *py = (const uint8_t*)pic->data[0];
				y = (int)py[(ptrdiff_t)i * stride_y + (ptrdiff_t)j];
				if (pic->data[1])
				{
					const uint8_t *pu = (const uint8_t*)pic->data[1];
					const uint8_t *pv = (const uint8_t*)pic->data[2];
					cu = (int)pu[((ptrdiff_t)i >> ss_y) * stride_c + ((ptrdiff_t)j >> ss_x)];
					cv = (int)pv[((ptrdiff_t)i >> ss_y) * stride_c + ((ptrdiff_t)j >> ss_x)];
				}
			}

			/* standard BT.601 */
			if (use_bt709)
			{
				r = y + ((359 * (cv - 128)) >> 8);
				g = y - ((88 * (cu - 128) + 183 * (cv - 128)) >> 8);
				b = y + ((454 * (cu - 128)) >> 8);
			}
			else if (use_bt2020)
			{
				r = y + ((263 * (cv - 128)) >> 7);
				g = y - ((29 * (cu - 128) + 102 * (cv - 128)) >> 7);
				b = y + ((335 * (cu - 128)) >> 7);
			}
			else
			{
				r = y + ((359 * (cv - 128)) >> 8);
				g = y - ((88 * (cu - 128) + 183 * (cv - 128)) >> 8);
				b = y + ((454 * (cu - 128)) >> 8);
			}

			*dst++ = 0xFF000000UL | (clip255(r) << 16) |
			                        (clip255(g) <<  8) |
			                         clip255(b);
		}
	}
	return TRUE;
}

///
/// DecodeOneFrame()

static void data_free_wrap(const uint8_t *buf, void *cookie)
{
	(void)buf;
	(void)cookie;
}

static BOOL DecodeOneFrame(Class *cl, Object *obj)
{
	GET_DATA;
	Dav1dData data;
	Dav1dPicture pic;
	int res;
	BOOL got_picture = FALSE;
	ULONG need = sizeof(struct AvifInfo);
	unsigned nframes = 0;

	/* Feed all data in one go */
	memset(&data, 0, sizeof(data));

	if (dav1d_data_wrap(&data, d->od_ES, d->od_ESLength, data_free_wrap, NULL))
	{
		MLOG(LOG_ERRORS, "Unable to wrap elementary stream.");
		return FALSE;
	}

	res = dav1d_send_data(d->od_Dav1d, &data);
	if (res != 0)
	{
		/* DAV1D_ERR(EAGAIN) means the decoder wants more input */
		if (res != DAV1D_ERR(EAGAIN))
		{
			MLOGV(LOG_ERRORS, "dav1d_send_data failed (%d).", res);
			return FALSE;
		}
	}

	/* Flush: signal end of stream */
	memset(&data, 0, sizeof(data));
	dav1d_send_data(d->od_Dav1d, &data);

	/* Retrieve the first decoded picture */
	memset(&pic, 0, sizeof(pic));
	while ((res = dav1d_get_picture(d->od_Dav1d, &pic)) == 0)
	{
		nframes++;
		if (nframes > 1) break; /* animations: only stills for now */

		/* Scale check */
		if (pic.p.w != (int)d->od_Width || pic.p.h != (int)d->od_Height)
		{
			MLOGV(LOG_ERRORS, "Decoded size %d x %d differs from reported %ld x %ld.",
			      (int)pic.p.w, (int)pic.p.h,
			      d->od_Width, d->od_Height);
			dav1d_picture_unref(&pic);
			return FALSE;
		}

		/* Allocate ARGB32 bitmap */
		d->od_BitmapBytes = d->od_Width * d->od_Height * 4;
		d->od_Bitmap = (UBYTE*)MediaAllocVec(d->od_BitmapBytes);
		if (!d->od_Bitmap)
		{
			MLOG(LOG_ERRORS, "Out of memory for bitmap.");
			dav1d_picture_unref(&pic);
			return FALSE;
		}

		if (!yuv_to_argb32(&pic, d->od_Bitmap))
		{
			MediaFreeVec(d->od_Bitmap);
			d->od_Bitmap = NULL;
			dav1d_picture_unref(&pic);
			return FALSE;
		}

		dav1d_picture_unref(&pic);
		got_picture = TRUE;
	}

	if (!got_picture)
	{
		MLOGV(LOG_ERRORS, "dav1d_get_picture failed (%d).", res);
		return FALSE;
	}

	d->od_DecodeDone = TRUE;

	(void)need;
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

		DoMethod(obj, MMM_AddPort, 0);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_Type, MDP_TYPE_INPUT);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_FormatsTable, (ULONG)InputFormats);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_Format, MMF_VIDEO_AVIF);

		DoMethod(obj, MMM_AddPort, 1);
		DoMethod(obj, MMM_SetPort, 1, MMA_Port_Type, MDP_TYPE_OUTPUT);
		DoMethod(obj, MMM_SetPort, 1, MMA_Port_FormatsTable, (ULONG)OutputFormats);
		DoMethod(obj, MMM_SetPort, 1, MMA_Port_Format, MMFC_VIDEO_ARGB32);

		d->od_Width = 0;
		d->od_Height = 0;
		d->od_BitsPerPixel = 0;
		d->od_UseAlpha = FALSE;
		d->od_ES = NULL;
		d->od_ESLength = 0;
		d->od_Bitmap = NULL;
		d->od_BitmapBytes = 0;
		d->od_Dav1d = NULL;
		d->od_DecodeDone = FALSE;

		newobj = (LONG)obj;

		DoMethod(obj, MMM_UnlockObject);
	}

	if (!newobj) CoerceMethod(cl, obj, (Msg)OM_DISPOSE);

	return newobj;
}

///
/// Dispose()

LONG Dispose(Class *cl, Object *obj, Msg msg)
{
	GET_DATA;

	DoMethod(obj, MMM_LockObject);

	if (d->od_Bitmap) MediaFreeVec(d->od_Bitmap);
	if (d->od_ES) MediaFreeVec(d->od_ES);
	if (d->od_Dav1d) dav1d_close(&d->od_Dav1d);

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

		case MMA_DataFormat:
			*msg->opg_Storage = (LONG)"AVIF";
			return TRUE;

		case MMA_MediaType:
			*msg->opg_Storage = MMT_PICTURE;
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
				/* Decode once, then serve bitmap rows */
				if (!d->od_DecodeDone)
				{
					if (!LoadData(cl, obj)) break;
					if (!DecodeOneFrame(cl, obj)) break;
				}

				if (d->od_Bitmap && d->od_BitmapBytes)
				{
					ULONG avail = d->od_BitmapBytes;
					ULONG want = msg->Length & 0xFFFFFFFC;  /* whole pixels */
					if (want > avail) want = avail;
					memcpy(msg->Buffer, d->od_Bitmap, want);
					bytes_pulled = want;
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

	if (msg->Port == 0)
	{
		rv = LoadData(cl, obj);
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