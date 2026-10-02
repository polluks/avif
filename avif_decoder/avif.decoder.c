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
	BOOL     od_AlphaPremul;
	BOOL     od_Monochrome;

	/* colour signalling from the demuxer, used to override the sequence
	 * header when the container is more specific than the stream */
	BOOL     od_HaveNclx;
	ULONG    od_MatrixCoefficients;
	BOOL     od_FullRange;

	/* the elementary streams */
	UBYTE   *od_ES;
	ULONG    od_ESLength;
	UBYTE   *od_AlphaES;
	ULONG    od_AlphaESLength;

	/* decoded pixels */
	UBYTE   *od_Bitmap;
	ULONG    od_BitmapBytes;
	ULONG    od_Pos;          /* read position used by Pull on port 1 */

	/* state */
	BOOL          od_Loaded;
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
static void FreeData(struct ObjData *d);

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
/// FreeData() - release everything LoadData() may have allocated.

static void FreeData(struct ObjData *d)
{
	if (d->od_Bitmap) MediaFreeVec(d->od_Bitmap);
	d->od_Bitmap = NULL;
	d->od_BitmapBytes = 0;
	d->od_Pos = 0;

	if (d->od_ES) MediaFreeVec(d->od_ES);
	d->od_ES = NULL;
	d->od_ESLength = 0;

	if (d->od_AlphaES) MediaFreeVec(d->od_AlphaES);
	d->od_AlphaES = NULL;
	d->od_AlphaESLength = 0;

	d->od_Loaded     = FALSE;
	d->od_DecodeDone = FALSE;
}

///
/// LoadData() - single initialisation path used by both Setup() and Pull().
///
/// Reads the attributes published by avif.demuxer on the input port and copies
/// the elementary streams it owns.  It is idempotent, so whichever of Setup()
/// on port 0 or the first Pull() on port 1 happens first, the decoder is only
/// ever configured once.

BOOL LoadData(Class *cl, Object *obj)
{
	GET_DATA;
	struct AvifInfo *info;
	ULONG es_len, alpha_len;

	if (d->od_Loaded) return TRUE;

	/*
	 * The AvifInfo pointer travels as an attribute value, so it has to be
	 * fetched through storage: MediaGetPortFwd() hands the value back in
	 * the return register, which is 32 bit on MorphOS and cannot carry a
	 * pointer.  It is also a varargs call, so the storage pointer has to
	 * be passed even when only the value is wanted.
	 */
	ULONG info_ptr = 0;

	if (!MediaGetPortFwd(obj, 0, MMA_ExtraData, &info_ptr))
	{
		MLOG(LOG_ERRORS, "No AvifInfo from input port.");
		return FALSE;
	}
	info = (struct AvifInfo*)(APTR)info_ptr;
	if (!info)
	{
		MLOG(LOG_ERRORS, "No AvifInfo from input port.");
		return FALSE;
	}

	d->od_Width              = info->ai_Width;
	d->od_Height             = info->ai_Height;
	d->od_BitsPerPixel       = info->ai_BitsPerPixel;
	d->od_UseAlpha           = info->ai_UseAlpha ? TRUE : FALSE;
	d->od_AlphaPremul        = info->ai_AlphaPremul ? TRUE : FALSE;
	d->od_Monochrome         = info->ai_Monochrome ? TRUE : FALSE;
	d->od_HaveNclx           = info->ai_HaveNclx ? TRUE : FALSE;
	d->od_MatrixCoefficients = info->ai_MatrixCoefficients;
	d->od_FullRange          = info->ai_FullRangeFlag ? TRUE : FALSE;

	if (d->od_Width == 0 || d->od_Height == 0 ||
	    d->od_Width >= 0x10000 || d->od_Height >= 0x10000)
	{
		MLOG(LOG_ERRORS, "Invalid dimensions %lu x %lu.",
		     d->od_Width, d->od_Height);
		return FALSE;
	}

	es_len    = info->ai_ESLength;
	alpha_len = info->ai_AlphaES ? info->ai_AlphaESLength : 0;

	if (!es_len || !info->ai_ES)
	{
		MLOG(LOG_ERRORS, "Empty elementary stream.");
		return FALSE;
	}

	/*
	 * Copy the streams, so the decoder does not depend on the demuxer's
	 * buffers staying alive and unmodified.
	 */
	d->od_ES = (UBYTE*)MediaAllocVec(es_len);
	if (!d->od_ES)
	{
		MLOG(LOG_ERRORS, "Out of memory for elementary stream.");
		return FALSE;
	}
	memcpy(d->od_ES, info->ai_ES, es_len);
	d->od_ESLength = es_len;

	if (alpha_len)
	{
		d->od_AlphaES = (UBYTE*)MediaAllocVec(alpha_len);
		if (!d->od_AlphaES)
		{
			MLOG(LOG_ERRORS, "Out of memory for alpha stream.");
			MediaFreeVec(d->od_ES);
			d->od_ES = NULL;
			d->od_ESLength = 0;
			return FALSE;
		}
		memcpy(d->od_AlphaES, info->ai_AlphaES, alpha_len);
		d->od_AlphaESLength = alpha_len;
	}

	d->od_Loaded = TRUE;
	return TRUE;
}

///
/// clip a value to the 8 bit range

static UBYTE clip255(int v)
{
	if (v < 0)   return 0;
	if (v > 255) return 255;
	return (UBYTE)v;
}

///
/// Colour conversion.
///
/// The AVIF container wins over the sequence header when it carries nclx
/// colour information, because nclx describes the colour the image was
/// encoded in and is what the file's other properties (primary, transfer)
/// agree with.
///
/// YUV samples are first scaled to 16 bit full range (Y in 0..65535, chroma
/// centred on 0), then converted with integer coefficients in the same scale,
/// then the high byte is taken.  Scaling to 16 bit rather than 8 bit keeps the
/// 10 and 12 bit paths from throwing away two to four bits of accuracy before
/// the matrix is even applied.

struct ColorParams
{
	int ylo;           /* lowest luma code value in use      */
	int yscale;        /* 16.16 fixed point luma scale       */
	int cscale;        /* 16.16 fixed point chroma scale     */
	/* matrix coefficients, in 16 bit luma scale */
	int r_cv, g_cu, g_cv, b_cu;
	/* matrix_coefficients 0: H.273 identity (GBR), not a YCbCr matrix */
	int gbr;
};

/*
 * Coefficients follow ITU-T H.273 / BT.601, BT.709, BT.2020 with
 *
 *   R = Y + r_cv * Cr
 *   G = Y - g_cu * Cu - g_cv * Cr
 *   B = Y + b_cu * Cu
 *
 * where Cu and Cr are the chroma differences scaled so that a full swing
 * chroma sample (half of the code range) maps to +-32768.
 */
static void pick_matrix(const struct ObjData *d, const Dav1dPicture *pic,
                        struct ColorParams *cp)
{
	ULONG m = 0;

	/* BT.601 is the safe default for everything AVIF can carry */
	cp->r_cv = 359; cp->g_cu = 88; cp->g_cv = 183; cp->b_cu = 454;
	cp->gbr = FALSE;

	if (d->od_HaveNclx) m = d->od_MatrixCoefficients;
	else if (pic && pic->seq_hdr) m = (ULONG)pic->seq_hdr->mtrx;

	/*
	 * The table below is the exact inverse of the H.273 forward matrix,
	 * rounded to the 16.16 scale in which the coefficients live:
	 *
	 *   R = Y + 2*(1-Kr)*Cr
	 *   G = Y - 2*Kb*(1-Kb)/Kg * Cu - 2*Kr*(1-Kr)/Kg * Cr
	 *   B = Y + 2*(1-Kb)*Cu
	 *
	 * Note that the green coefficients are 2*Kx*(1-Kx)/Kg, not the
	 * frequently quoted 2*Kx/(1-Kx); the latter is only valid when
	 * Kg == 1-Kr-Kb is folded in, and is wrong by up to 25% for 601.
	 */
	switch (m)
	{
		case 1:   /* BT.709 */
		case 7:   /* SMPTE 240M, approximated as BT.709 here */
		case 12:  /* chromaticity derived, as BT.709 */
		case 13:  /* chromaticity derived constant luminance */
		case 14:  /* ICtCp, approximated as BT.709 */
			cp->r_cv = 403; cp->g_cu = 48; cp->g_cv = 120; cp->b_cu = 475;
		break;

		case 9:   /* BT.2020 non constant luminance */
		case 10:  /* BT.2020 constant luminance */
		case 11:  /* BT.2020 constant luminance */
			cp->r_cv = 378; cp->g_cu = 42; cp->g_cv = 146; cp->b_cu = 482;
		break;

		case 4:   /* FCC 70 */
			cp->r_cv = 358; cp->g_cu = 85; cp->g_cv = 182; cp->b_cu = 456;
		break;

		case 5:   /* BT.470BG */
		case 6:   /* BT.601 / SMPTE 170M */
		case 2:   /* unspecified, RGB is not what AVIF stores */
		case 8:   /* YCgCo, out of scope for AVIF */
		default:
		break;

		case 0:   /* H.273 identity: G in the luma plane, R and B in chroma */
			cp->gbr = TRUE;
		break;
	}

	if (d->od_FullRange)
	{
		cp->ylo = 0;
		cp->yscale = (65536 << 8) / 255;
		cp->cscale = (65536 << 8) / 255;
	}
	else
	{
		/*
		 * ITU-T H.273 limited range: luma occupies codes 16..234 and
		 * chroma 16..240, so both are expanded onto the full 0..255
		 * range.  Chroma neutral is code 128 in both cases; the range
		 * expansion is expressed purely through cscale.
		 */
		cp->ylo = 16;
		cp->yscale = (65536 << 8) / 219;
		cp->cscale = (65536 << 8) / 224;
	}
}

///
/// plane_sample() - read one sample of an 8 or 16 bit plane at (x, y),
/// clamped to the plane bounds.
///
/// stride is the distance between two rows measured in samples, not in bytes:
/// Dav1dPicture.stride[] is a byte count, so 16 bit planes have to halve it
/// before it is used to index a uint16_t.

static int plane_sample(const void *data, ptrdiff_t stride, int is16, int shift,
                        int x, int y, int w, int h)
{
	if (x >= w) x = w - 1;
	if (y >= h) y = h - 1;

	if (is16)
	{
		const uint16_t *p = (const uint16_t*)data;
		return (int)p[(ptrdiff_t)y * stride + x] >> shift;
	}
	else
	{
		const uint8_t *p = (const uint8_t*)data;
		return (int)p[(ptrdiff_t)y * stride + x];
	}
}

///
/// chroma_at() - fetch a chroma sample with bilinear upsampling.
///
/// In AV1 the chroma samples are co-sited with the top left luma sample of
/// the block they cover, so luma pixel (x, y) sits at chroma coordinate
/// (x / 2^ss_x, y / 2^ss_y).  That lands exactly between two chroma samples
/// whenever the corresponding luma coordinate is odd, which is the case that
/// a plain shift would get wrong.  Interpolating there removes the blocky
/// chroma edges a nearest neighbour upsample leaves behind.

static int chroma_at(const void *data, ptrdiff_t stride, int is16, int shift,
                     int x, int y, int ss_x, int ss_y, int cw, int ch)
{
	int fx = x & ((1 << ss_x) - 1);
	int fy = y & ((1 << ss_y) - 1);
	int x0 = x >> ss_x;
	int y0 = y >> ss_y;
	int a = plane_sample(data, stride, is16, shift, x0, y0, cw, ch);

	if (!fx && !fy) return a;

	if (fy)
	{
		int b = plane_sample(data, stride, is16, shift, fx ? x0 + 1 : x0, y0, cw, ch);
		int c = plane_sample(data, stride, is16, shift, x0, y0 + 1, cw, ch);

		if (!fx) return (a + c + 1) >> 1;

		{
			int d = plane_sample(data, stride, is16, shift, x0 + 1, y0 + 1, cw, ch);
			return (a + b + c + d + 2) >> 2;
		}
	}
	else
	{
		int b = plane_sample(data, stride, is16, shift, x0 + 1, y0, cw, ch);
		return (a + b + 1) >> 1;
	}
}

///
/// yuv_to_argb32() - scalar YUV to ARGB32 conversion.
///
/// Supports 8/10/12 bit samples, 4:4:4 / 4:2:2 / 4:2:0 / monochrome, limited
/// and full range, and an optional separately coded alpha plane.

static BOOL yuv_to_argb32(const Dav1dPicture *pic, const Dav1dPicture *alpha_pic,
                          const struct ObjData *d, UBYTE *argb)
{
	struct ColorParams cp;
	ULONG i, j;
	const int w = (int)d->od_Width;
	const int h = (int)d->od_Height;
	const int shift = (pic->p.bpc > 8) ? (pic->p.bpc - 8) : 0;
	const int is16 = (pic->p.bpc > 8);
	const int ss_x = (pic->p.layout == DAV1D_PIXEL_LAYOUT_I420 ||
	                  pic->p.layout == DAV1D_PIXEL_LAYOUT_I422);
	const int ss_y = (pic->p.layout == DAV1D_PIXEL_LAYOUT_I420);
	int a_shift = 0, a_is16 = 0;
	int cw, ch;
	/*
	 * Dav1dPicture.stride[] is a byte distance between two rows, so for
	 * the 10 and 12 bit planes it has to be halved before it is used as
	 * an offset into a uint16_t array.
	 */
	ptrdiff_t ystride = pic->stride[0], cstride = pic->stride[1];
	ptrdiff_t astride = 0;

	if (!argb || !pic->data[0]) return FALSE;

	if (pic->p.bpc > 8)
	{
		ystride /= 2;
		cstride /= 2;
	}

	if (pic->p.w != w || pic->p.h != h)
	{
		MLOGV(LOG_ERRORS, "Decoded size %d x %d differs from %lu x %lu.",
		      pic->p.w, pic->p.h, d->od_Width, d->od_Height);
		return FALSE;
	}

	if (alpha_pic)
	{
		if (!alpha_pic->data[0] ||
		    alpha_pic->p.w != w || alpha_pic->p.h != h)
		{
			MLOGV(LOG_ERRORS, "Alpha plane %d x %d is unusable.",
			      alpha_pic->p.w, alpha_pic->p.h);
			return FALSE;
		}

		a_shift = (alpha_pic->p.bpc > 8) ? (alpha_pic->p.bpc - 8) : 0;
		a_is16 = (alpha_pic->p.bpc > 8);
		astride = alpha_pic->stride[0];

		if (a_is16) astride /= 2;
	}

	pick_matrix(d, pic, &cp);

	/* chroma plane dimensions, rounded up for odd image sizes */
	cw = (w + (1 << ss_x) - 1) >> ss_x;
	ch = (h + (1 << ss_y) - 1) >> ss_y;

	for (i = 0; i < (ULONG)h; i++)
	{
		UBYTE *dst = argb + i * (ULONG)w * 4;

		for (j = 0; j < (ULONG)w; j++)
		{
			int y, cu = 128, cv = 128, a = 255;
			int Y, Cb, Cr, r, g, b;

			if (is16)
			{
				const uint16_t *py = (const uint16_t*)pic->data[0];
				y = (int)py[(ptrdiff_t)i * ystride + (ptrdiff_t)j] >> shift;
			}
			else
			{
				const uint8_t *py = (const uint8_t*)pic->data[0];
				y = (int)py[(ptrdiff_t)i * ystride + (ptrdiff_t)j];
			}

			if (pic->data[1] && pic->data[2])
			{
				cu = chroma_at(pic->data[1], cstride, is16, shift,
				               (int)j, (int)i, ss_x, ss_y, cw, ch);
				cv = chroma_at(pic->data[2], cstride, is16, shift,
				               (int)j, (int)i, ss_x, ss_y, cw, ch);
			}

			/* luma to 16 bit full range, 0..65535 */
			Y = ((y - cp.ylo) * cp.yscale) >> 8;
			if (Y < 0) Y = 0;
			if (Y > 65535) Y = 65535;

			/* chroma to signed 16 bit, centred on code 128 */
			if (pic->data[1])
			{
				Cb = ((cu - 128) * cp.cscale) >> 8;
				Cr = ((cv - 128) * cp.cscale) >> 8;
			}
			else
			{
				Cb = Cr = 0;
			}

			if (!pic->data[1])
			{
				/* monochrome: Y only */
				r = g = b = Y;
			}
			else if (cp.gbr)
			{
				/*
				 * H.273 identity (GBR): the luma plane carries
				 * green and the two chroma planes carry red
				 * and blue, both centred on code 128.
				 */
				r = 32768 + Cr;
				g = Y;
				b = 32768 + Cb;
			}
			else
			{
				r = Y + ((cp.r_cv * Cr) >> 8);
				g = Y - ((cp.g_cu * Cb + cp.g_cv * Cr) >> 8);
				b = Y + ((cp.b_cu * Cb) >> 8);
			}

			if (alpha_pic)
			{
				if (a_is16)
				{
					const uint16_t *pa = (const uint16_t*)alpha_pic->data[0];
					a = (int)pa[(ptrdiff_t)i * astride + (ptrdiff_t)j] >> a_shift;
				}
				else
				{
					const uint8_t *pa = (const uint8_t*)alpha_pic->data[0];
					a = (int)pa[(ptrdiff_t)i * astride + (ptrdiff_t)j];
				}

				if (a < 0) a = 0;
				if (a > 255) a = 255;

				/*
				 * ARGB32 carries straight alpha, so the colour is
				 * left untouched here.  When the file says the
				 * colour planes were premultiplied they are already
				 * scaled and must be handed on as they are.
				 */
			}

			/*
			 * ARGB32 is a big endian 32 bit value, so in memory the
			 * bytes are alpha, red, green, blue in that order.
			 */
			dst[0] = (UBYTE)a;
			dst[1] = clip255(r >> 8);
			dst[2] = clip255(g >> 8);
			dst[3] = clip255(b >> 8);
			dst += 4;
		}
	}

	return TRUE;
}


///
/// data_free_wrap() - dav1d does not take ownership of the data we wrap, so
/// the buffers stay ours and there is nothing to free here.

static void data_free_wrap(const uint8_t *buf, void *cookie)
{
	(void)buf;
	(void)cookie;
}

///
/// decode_one_stream() - run one AV1 stream through a dav1d instance and
/// return its first picture.
///
/// On success the caller owns both the returned picture and the decoder
/// instance: dav1d_picture_unref() releases the picture and dav1d_close()
/// releases the context.  The context has to outlive the picture because the
/// picture holds references into it.

static BOOL decode_one_stream(const UBYTE *es, ULONG len,
                              Dav1dContext **ctx_out, Dav1dPicture *out)
{
	Dav1dSettings s;
	Dav1dContext *ctx = NULL;
	Dav1dData data;
	int res;

	memset(out, 0, sizeof(*out));
	*ctx_out = NULL;

	if (!es || !len) return FALSE;

	dav1d_default_settings(&s);
	s.n_threads = 1;
	s.max_frame_delay = 1;
	/*
	 * Film grain synthesis stays enabled.  The grain parameters are part
	 * of the coded image, so this is what avifdec and libavif render; the
	 * tests compare against those, and dropping the grain would silently
	 * lose encoded detail.
	 */
	s.all_layers = 1;
	s.operating_point = 0;
	s.strict_std_compliance = 0;
	s.inloop_filters = DAV1D_INLOOPFILTER_ALL;

	if (dav1d_open(&ctx, &s) != 0)
	{
		MLOGV(LOG_ERRORS, "Unable to open dav1d decoder.");
		return FALSE;
	}

	memset(&data, 0, sizeof(data));
	if (dav1d_data_wrap(&data, es, len, data_free_wrap, NULL) != 0)
	{
		MLOG(LOG_ERRORS, "Unable to wrap elementary stream.");
		dav1d_close(&ctx);
		return FALSE;
	}

	/*
	 * dav1d consumes the sequence header first and answers EAGAIN because it
	 * wants the frame that follows.  Flushing with an empty Dav1dData then
	 * tells it no more data is coming, which is what lets the pending frame
	 * be decoded.
	 */
	res = dav1d_send_data(ctx, &data);
	if (res != 0 && res != DAV1D_ERR(EAGAIN))
	{
		MLOGV(LOG_ERRORS, "dav1d_send_data failed (%d).", res);
		dav1d_close(&ctx);
		return FALSE;
	}

	memset(&data, 0, sizeof(data));
	res = dav1d_send_data(ctx, &data);
	if (res != 0 && res != DAV1D_ERR(EAGAIN))
	{
		MLOGV(LOG_ERRORS, "dav1d_send_data flush failed (%d).", res);
		dav1d_close(&ctx);
		return FALSE;
	}

	res = dav1d_get_picture(ctx, out);
	if (res == 0)
	{
		*ctx_out = ctx;
		return TRUE;
	}

	MLOGV(LOG_ERRORS, "No picture decoded from stream (%d).", res);
	dav1d_close(&ctx);
	return FALSE;
}

///
/// DecodeOneFrame() - decode the colour stream and, when present, the alpha
/// stream, then convert to ARGB32.

static BOOL DecodeOneFrame(Class *cl, Object *obj)
{
	GET_DATA;
	Dav1dPicture pic;
	Dav1dPicture apic;
	Dav1dContext *ctx = NULL;
	Dav1dContext *actx = NULL;
	BOOL have_alpha = FALSE;
	BOOL ok = FALSE;
	ULONG bitmap_bytes;

	if (d->od_DecodeDone) return TRUE;

	if (!decode_one_stream(d->od_ES, d->od_ESLength, &ctx, &pic)) return FALSE;

	if (d->od_AlphaES && d->od_AlphaESLength)
	{
		have_alpha = decode_one_stream(d->od_AlphaES, d->od_AlphaESLength,
		                               &actx, &apic);
		if (!have_alpha)
		{
			/*
			 * A missing alpha plane is not fatal: fall back to an
			 * opaque image rather than failing the whole decode.
			 */
			MLOGV(LOG_WARN, "Alpha stream did not decode, using opaque.");
		}
	}

	/*
	 * 32 bit overflow guard.  Both dimensions are already known to be
	 * below 0x10000, so this rejects anything whose bitmap would not fit
	 * in a positive LONG range.
	 */
	if (d->od_Width > 0x1FFFFFFFUL / d->od_Height)
	{
		MLOG(LOG_ERRORS, "Bitmap size overflow.");
		goto cleanup;
	}

	bitmap_bytes = d->od_Width * d->od_Height * 4;

	d->od_Bitmap = (UBYTE*)MediaAllocVec(bitmap_bytes);
	if (!d->od_Bitmap)
	{
		MLOG(LOG_ERRORS, "Out of memory for bitmap.");
		goto cleanup;
	}
	d->od_BitmapBytes = bitmap_bytes;

	if (!yuv_to_argb32(&pic, have_alpha ? &apic : NULL, d, d->od_Bitmap))
	{
		MediaFreeVec(d->od_Bitmap);
		d->od_Bitmap = NULL;
		d->od_BitmapBytes = 0;
		goto cleanup;
	}

	ok = TRUE;

cleanup:
	if (have_alpha) dav1d_picture_unref(&apic);
	if (actx) dav1d_close(&actx);
	dav1d_picture_unref(&pic);
	if (ctx) dav1d_close(&ctx);

	if (ok) d->od_DecodeDone = TRUE;

	return ok;
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
		d->od_AlphaPremul = FALSE;
		d->od_Monochrome = FALSE;
		d->od_HaveNclx = FALSE;
		d->od_MatrixCoefficients = 0;
		d->od_FullRange = FALSE;
		d->od_ES = NULL;
		d->od_ESLength = 0;
		d->od_AlphaES = NULL;
		d->od_AlphaESLength = 0;
		d->od_Bitmap = NULL;
		d->od_BitmapBytes = 0;
		d->od_Pos = 0;
		d->od_Loaded = FALSE;
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

	FreeData(d);

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
			 * A picture attribute is a LONG on MorphOS, so the value
			 * is written as a LONG rather than through a UQUAD cast,
			 * which would write past the caller's storage.
			 */
			*msg->opg_Storage = (LONG)1;
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
			{
				ULONG avail, want;

				/* Decode lazily on the first pull of the image. */
				if (!d->od_DecodeDone)
				{
					if (!LoadData(cl, obj)) break;
					if (!DecodeOneFrame(cl, obj)) break;
				}

				if (!d->od_Bitmap || !d->od_BitmapBytes)
				{
					seterr(MMERR_END_OF_DATA);
					break;
				}

				if (d->od_Pos >= d->od_BitmapBytes)
				{
					seterr(MMERR_END_OF_DATA);
					break;
				}

avail = d->od_BitmapBytes - d->od_Pos;

			/*
			 * Only whole pixels are handed out.  A request
			 * that is too short for even one pixel cannot be
			 * satisfied, and forcing four bytes there would
			 * write past the end of the caller's buffer.
			 */
			want = msg->Length & ~3UL;
			if (!want)
			{
				seterr(MMERR_WRONG_ARGUMENTS);
				break;
			}
			if (want > avail) want = avail;

				memcpy(msg->Buffer, d->od_Bitmap + d->od_Pos, want);
				d->od_Pos += want;
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
		/*
		 * Only pull in the attributes here.  Decoding is left to the
		 * first Pull() on the output port, because the input stream is
		 * not guaranteed to be fully set up at Setup() time.
		 */
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
		case MMA_Video_FrameCount:
		case MMA_DataFormat:
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