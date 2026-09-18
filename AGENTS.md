# AVIF demuxer/decoder work (Reggae)

## Project goal

Add AV1 Image File Format (AVIF, `avif.demuxer` + `avif.decoder`) support to
MorphOS/Reggae as a pair of classic two-port multimedia plugins, modeled on the
official `example/deep_demuxer/` + `example/deep_decoder/` skeletons:

- `avif.demuxer` - parses the AVIF / ISO BMFF container, exposes image info
  (width, height, bit depth, alpha, metadata) and emits the AV1 elementary
  stream to the output port.
- `avif.decoder` - consumes the AV1 elementary stream and decodes it to the
  Reggae common video format `MMFC_VIDEO_ARGB32` using a bundled, vendored
  copy of the open-source AV1 decoder **dav1d**.

Reggae has no system AV1 decoder, so dav1d is vendored into the decoder repo
(deliverable includes its source + merged static build). The demuxer is plain
C; the decoder is plain C with an internal C99 dav1d; both use the standard
Reggae class skeleton.

## Files

- `example/deep_demuxer/` - **primary template for avif.demuxer**: clean SUPPORTED
  two-port demuxer (input port 0 = `MMF_STREAM`, output port 1 = codified
  format), `GetHeader()` signature parse, Pull passthrough after header,
  `recognize.c` `.dtcode` recognition blob, `class_version.h`, makefile.
- `example/deep_decoder/` - **primary template for avif.decoder**: input port 0
  = the demuxer's output format, output port 1 = `MMFC_VIDEO_ARGB32`; a
  `data.c` `.dtcode` blob (decoders carry no `Recognize`, only
  `ClassAttributes()`); `mybuffer.{h,cpp}` optional.
- The `sid_demuxer/` + `sid_decoder/` pair in this repo: the octamed-style
  sibling repos showing a custom fleet format tag + decoder pair end to end.
- Reggae SDK headers (reference, not for editing): the developer SDK lives in
  `/tmp/opencode/reggae/Reggae/developer/include` (`classes/multimedia/
  multimedia.h`, `video.h`, `sound.h`, `streams.h`). The stubbed host-build
  headers used by octamed live in `/tmp/opencode/medhdr/`.

## Design decisions

- **Two plugins, not one**: avif.demuxer demultiplexes only; avif.decoder does
  the AV1 decode. Chosen to match the classic `sid.demuxer`/`sid.decoder`
  split and Reggae's class taxonomy (demuxers get `Recognize`, decoders do not).
- **Formats** (new constants, defined in each plugin's public header):
  - `MMF_VIDEO_AVIF` - AVIF container / the AV1 elementary stream as pulled
    from the input (analogous to `MMF_VIDEO_IFFDEEP`), used as the demuxer's
    output-format ID and the decoder's input-format ID.
  - `MMF_VIDEO_AV1` - bare AV1 elementary stream (optional; exposed via
    `MMA_ExtraData` of the demuxer when a decoder wants the raw OBU stream).
  - Output of avif.decoder is `MMFC_VIDEO_ARGB32` (the only Reggae common video
    format; `MMFC_VIDEO_*` constants are already defined in multimedia.h).
  - `MMA_Video_Width/Height/BitsPerPixel/UseAlpha` all exist in MSV video.h;
    `MMA_MetaData` exists in multimedia.h. Do NOT redefine existing MMA tags;
    extend only with private `MM_TAG(0x[block], id)` tags if a specific piece
    of AVIF metadata (e.g. NCLX colour info, mastering display, content light
    level) must be exposed.
- **Decoder engine**: dav1d (BSD-2, VideoLAN) release 1.5.4 vendored into
  `avif_decoder/`. Only the portable scalar C sources are pulled in; no
  compiler, no asm, no vector code paths (PPC32 target, 604e, no build-time
  SIMD). dav1d is C99 and needs libm. Any generated meson headers
  (`config.h`, `version.h`, `cpu.h`) are hand-written equivalents.
- **Media/model type**: AVIF still images are `MMT_PICTURE`; AVIF animation
  sequences are `MMT_VIDEO`. Use `MMT_PICTURE` unless the demuxer finds >1
  frame in the movie/frame sequence, then use `MMT_VIDEO`.
- **Versioning**: `avif.demuxer`/`avif.decoder` start at VERSION 1, REVISION 1
  in their own `class_version.h` (pattern identical to the deep examples).

## Reggae skeleton conventions (memorized for both files)

- `ClassBase` = `{LibNode, LibClass/MyClass, Seglist, BaseLock, InitFlag,
  Attributes}`; `LibInit` via `NewCreateLibraryTags` with
  `LIBTAG_FUNCTIONINIT` JumpTable (5 base funcs + `GetClass`), `MACHINE_PPC`,
  `LIBTAG_PUBLIC TRUE`; `ROMTag` Resident + `RTags`
  (`QUERYINFOATTR_*`, `MMA_MediaType`, `MMA_SupportedFormats`).
- `init_class` = `MakeClass(CLASSNAME, "multimedia.class", NULL,
  sizeof(struct ObjData), 0)`, dispatcher through `EmulLibEntry` gate
  (`TRAP_LIB`), `AddClass`.
- Dispatcher handles `OM_NEW`, `OM_DISPOSE`, `OM_GET`, `MMM_Pull`, `MMM_Setup`,
  `MMM_GetPort`; default → `DoSuperMethodA`.
- Two-port `New`: port 0 `MDP_TYPE_INPUT` / `MMF_STREAM`, port 1
  `MDP_TYPE_OUTPUT` / plugin format; `InputFormats[]`/`OutputFormats[]` are
  ULONG arrays terminated by 0.
- `Setup` on port 0 runs `GetHeader`; Pull on port 0 is `DoSuperMethodA`
  (stream passthrough), Pull on port 1 produces plugin data; `GetPort` mirrors
  attributes onto the port.
- `.dtcode` blob: demuxer `recognize.c` exports `Recognize()` +
  `ClassAttributes()`; decoder `data.c` exports only `ClassAttributes()` with
  `MMA_ClassType MMCLASS_DEMUXER|MMCLASS_DECODER`, `MMA_MediaType`,
  (decoder) `MMA_SupportedFormats`. Build with objcopy `--add-section
  .dtcode=...`; `-nostdlib -labox`.
- Makefile: `ppc-morphos-gcc -noixemul -nostartfiles -O2 -mcpu=604e`,
  `-labox -lm -lmath`; `.db` then strip → install to
  `/SYS/MorphOS/Classes/Multimedia/`, `flushlib`.

## AVIF / ISO BMFF parsing rules (from libavif/ISOBMFF)

Big-endian box parser. Box header: `u32 size` + 4-cc type; `size==0` = box
extends to EOF, `size==1` = 64-bit `largesize` follows, `size==8` = uuid box
(skip). Read helpers `rd_be16/rd_be24/rd_be32/rd_be64`; always bounds-check
against the known stream length (`MediaGetPort64(obj, 0, MMA_StreamLength)`).

Boxes to walk:

- `ftyp` (full box, version 0): `major_brand` 4cc (usually `avif`), minor
  version u32, `compatible_brands[]`. Recognition also accepts `avis` /
  `avio` / `mif1` majors and `avif`/`avis` in compatible brands.
- `meta` (full box): children include `hdlr` (handler subtype `pict`),
  `pitm` (primary item id: u16 for v0/v1, u32 for v2),
  `iloc`, `iinf`, `iprp`; may be at top level or inside `moov`.
- `iloc` (item location): after full-box header - offset_size(4b),
  length_size(4b), base_offset_size(4b), index_size(4b) in the top byte;
  `item_count` u16; then per item: item_ID u16, (`data_reference_index` u16
  if version>0 - v2 adds a 4-byte zero + u16), construction_method u4 +
  data_reference_index u12 (v1/v2), base_offset u(size)
  (default 0 for version 0... use construction_method 0 = file offset),
  extent_count u16, each extent {index u(index_size) when index_size>0,
  offset u(offset_size), length u(length_size)}. Absolute file offsets =
  base_offset + sum of preceding extent lengths (+ construction_method
  0 uses absolute offsets, 1 = relative to extended type, 2 = iloc-relative).
  AVIF files use method 0.
- `iinf` (version 0: item_count u16; v1: u32), `infe` items with
  `item_type` 4cc (`av01` = AV1 item, `Grid`, `hvc1`, etc.).
- `iprp` → `ipco` (property container) + `ipma` (item property association).
  Properties of interest: `ispe` (FullBox version 0: u32 width, u32 height),
  `pixi` (version 0: channel count u8, per-channel bit depths), `auxC`
  (aux type string: `urn:mpeg:mpegB:cicp:systems:auxiliary:alpha` =
  separate alpha item, `...:premult` = alpha already premultiplied in the
  colour item), `av1C` (AV1CodecConfigurationRecord: marker(1b)=1,
  version(7b)=1, seq_profile(3b), seq_level_idx_0(5b), seq_tier_0(1b),
  high_bitdepth(1b), twelve_bit(1b), monochrome(1b), chroma_subsampling_x(1b),
  chroma_subsampling_y(1b), chroma_sample_position(2b), then for version>1
  reserved(3b) + initial_presentation_delay_present(1b) + bits per sample +
  OBUs (sequence header OBU if present).
- `mdat`: media data. Demuxer's output = the `av01` primary item's bytes
  located via `iloc` extents (seeking through `MMM_Seek`), with the av1C OBU
  sequence header prepended when the store does not already start with one.

Demuxer behaviour: on Setup port 0 parse the whole box tree into `struct
ObjData` (dimensions, bit depth, alpha flags, extents, av1C OBU). Pull on port
1 serves the primary image item's AV1 elementary stream via MMM_Seek +
MMM_Pull extents (contiguous reassembly as needed), then EOF. Alpha item is
NOT emitted on port 1 (decoder handles it as separate input, or it is
premultiplied); single-plane AVIF only. Attributes on Get/GetPort:
`MMA_Video_Width`, `MMA_Video_Height`, `MMA_Video_BitsPerPixel`,
`MMA_Video_UseAlpha`, `MMA_DataFormat` ("AVIF"), `MMA_MediaType`,
`MMA_MetaData` (title/copyright/description from `mdta`/`iinf` if present).

## Decoder design (dav1d glue)

- Input = AVIF/AV1 elementary stream. On Setup port 0 (or first port-1 Pull
  if lazy) allocate a `Dav1dContext` with `dav1d_open` (default params,
  `n_threads` per hardware; keep framethreads=1, tilethreads=1 for
  determinism), fetch width/height/bits from the demuxer via
  `MediaGetPortFwd(obj, 0, MMA_Video_*)`, and cache the first decoded picture.
- Decode loop: `dav1d_send_data` for each input chunk until `EAGAIN`,
  `dav1d_get_picture` per completed frame. AVIF = one frame normally, but
  animate (multi-frame) AVIF = feed frames sequentially; report additional
  frames via `MMA_Video_FrameCount`.
- YUV→ARGB32 conversion (own code, scalar): the picture provides planes,
  strides and `seq_hdr` info (bitdepth 8/10/12, chroma subsampling 4:2:0 /
  4:2:2 / 4:4:4, matrix coefficients/colour primaries/transfer from seq_hdr).
  Convert to 8-bit ARGB32 by shifting high-bitdepth samples down; apply
  standard BT.601/BT.709 conversion per matrix coefficient. Alpha: if the
  demuxer signals a separate alpha item, the decoder pulls that same
  (width,height) AV1 stream and decodes it to an alpha plane (decoded channels
  are one grey plane) as an interleaved second input; if premultiplied alpha,
  take the high byte of the RGB result.
- Pull on port 1 serves decoded rows in ARGB32 (4 bytes/pixel) like
  deep.decoder; consumers pull whole frames or row slices; bytes are rounded
  down to whole pixels.

## Build notes

- avif.demuxer: files `avif.demuxer.c`, `avif.demuxer.h`, `class_version.h`,
  `recognize.c`, `Makefile` (ppc-morphos-gcc cross build).
- avif.decoder: `avif.decoder.c`, `avif.decoder.h`, `class_version.h`,
  `data.c`, `Makefile`, plus `dav1d/` (vendored sources, BSD-2 licence
  COPYING, hand-written config), built as a static archive `libdav1d.a` and
  linked into the plugin. Host verification via stub headers under
  `/tmp/opencode/medhdr/` (as used by octamed) + an integration test that
  parses a tiny AVIF and decodes its frames.

## Work state / next steps

- [x] Confirm scope: demuxer (box parse + AV1 emit) + decoder (bundled dav1d
      → ARGB32)
- [x] Document conventions, formats, and AVIF/ISOBMFF parse rules in this file
- [ ] Write `avif.demuxer.c` (+ header, recognize.c, class_version.h, Makefile)
- [ ] Vendor dav1d 1.5.4 sources; trim to scalar C + hand-written config
- [ ] Write `avif.decoder.c` (skeleton + dav1d glue + YUV→ARGB32 + alpha)
- [ ] Host integration test (stub SDK headers, small generated AVIF), repro
      any parser/colour bugs
- [ ] Verify on real MorphOS hardware (needs ppc-morphos-gcc + Reggae SDK)