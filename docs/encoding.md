# Encoding images with `encode_image()`

`encode_image()` hands a frame to a third-party encoder: libjpeg-turbo, libwebp, libaom, libjxl, or one of FFmpeg's own encoders. Each of these libraries can change the frame on its way in, and each container can describe the result only partly. This page states what lossylab lets a library do, what it does itself, and what it refuses. It then lists the implemented codecs and what each supports.

## Usage

```python
import lossylab

frame = lossylab.decode_image(lossylab.Source.from_path("photo.png")).frame  # rgb24, sRGB

options = lossylab.EncodeImageOptions()
options.codec = lossylab.ImageCodec.Jpeg
options.pixel_format = lossylab.PixelFormat.from_name("yuv420p")
options.color = frame.color()  # keep the primaries and transfer; convert() cannot change them yet
options.color.matrix = lossylab.ColorMatrix.Bt470bg  # what JPEG implies: BT.601, full range, centered chroma
options.color.chroma_location = lossylab.ChromaLocation.Center
options.rate_control = lossylab.RateControl.quality(85)
options.strict = lossylab.Strict.AllowRecorded  # let lossylab convert rgb24 to yuv420p, and record it

encoded = lossylab.encode_image(frame, options)
encoded.bytes  # the file
encoded.record.conversions  # pix_fmt, subsampling, color_matrix, chroma_location, each with its cause
```

- `pixel_format` is required. It is the format the encoder receives, and it must be one the encoder accepts: `lossylab.capabilities().find_encoder("libwebp").pixel_formats` lists them.
- `color` is the color to encode in. Leave it unset to keep the frame's color. It must be set when the frame changes between RGB and YUV. Its primaries and transfer must match the frame's, since `convert()` cannot change them yet.
- Under `Strict.Refuse` (the default), a frame that is not already in `pixel_format` and `color` raises `ConversionRefused`. Under `Strict.AllowRecorded`, lossylab converts it and lists each change in `record.conversions`.

## The rules

### 1. lossylab makes every choice that changes sample values

Converting between RGB and YUV, changing range, resampling chroma, changing bit depth, and dropping alpha all involve choices: a matrix, a kernel, a chroma position, a rounding. lossylab makes these choices itself, in `convert()`, so that they are the same for every encoder and appear in the record. A library is never handed a frame that it would convert itself.

For example, FFmpeg's libwebp wrapper accepts `bgra` for a lossy encode and lets libwebp convert it to 4:2:0 with its own filter, so lossy WebP with `pixel_format = bgra` raises `ConfigError`; ask for `yuv420p` and lossylab converts and records it instead. Likewise, libjpeg-turbo gets planar YCbCr directly, so its own RGB to YCbCr step never runs.

### 2. A codec's own coding transform is allowed

Some formats transform the samples as part of coding them: JPEG XL codes lossy images in XYB, JPEG 2000 defines a reversible and an irreversible component transform, and AV1 codes `gbrp` with the identity matrix. Such a transform is defined by the format and recorded in the bitstream, every decoder undoes it, and the caller has no parameter for it. It is how the codec works, not a conversion, so lossylab lets the library apply it and does not record it.

The test is whether a decoder knows to undo it from the file alone. libwebp's RGB to YUV step fails that test: the file just holds YUV, and nothing in it tells a decoder which filter made it.

### 3. Repacking samples is not a conversion

Changing only how samples are laid out leaves every value unchanged: `rgb24`, `gbrp` and `bgra` hold the same numbers in a different order, as do planar and packed layouts, the two byte orders, and `yuvj420p` and full-range `yuv420p`. lossylab picks the encoder's own layout itself, in either strict mode, rather than asking the caller to know it.

For example, `pixel_format = rgb24` goes to lossless libwebp as `bgra`, to libaom as `gbrp`, and to libjxl unchanged. The repacking is listed in `record.conversions` as a `pix_fmt` change, and `record.output` names the layout the encoder received. An encoder with no layout of the requested format raises `ConfigError` naming the formats it takes.

### 4. The file must not misstate the frame's color

Every color property of the frame (matrix, range, primaries, transfer, chroma location) must either be written to the file or be what the format implies. When neither is true, what happens depends on what the file would say instead.

- **The file would state a different value:** lossylab always refuses with `ConfigError`, in either strict mode. A decoder would trust the wrong value and show wrong colors. For example, JPEG implies BT.601, so a JPEG encode with a BT.709 `color` raises `ConfigError`.

- **The file would say nothing (the value is dropped):** lossylab treats it like an ICC profile or an orientation the output cannot hold. `Strict.Refuse` raises `ConversionRefused`, and `Strict.AllowRecorded` encodes and records the property as `dropped` in `record.conversions`. A decoder then guesses, and the record shows that it had to. For example, AV1 cannot express centered chroma, so a centered 4:2:0 frame encoded to AVIF loses its chroma location.

What was dropped is found by probing the encoded bytes in memory, as for ICC profiles, orientation and sample aspect ratio, rather than from what the plan expected the library to write. No file has to be written. `record.output` states what the bytes hold, and `record.evidence.color` keeps the color the frame was encoded in, which `roundtrip()` assumes for what the bytes leave unstated.

## Implemented codecs

Every lossy image encode, and lossless WebP, takes `RateControl.quality()` only, and its value is checked against the encoder's range before encoding; `quality_range()` returns that range. Lossless PNG, JPEG XL and JPEG 2000 have no quality and ignore the rate control.

### PNG (`png`)

- **Supports:** lossless only. `rgb24`, `rgba`, `rgb48be`, `rgba64be`, `gray`, `ya8`, `gray16be`, `ya16be`, `pal8`, `monob`.
- **Does not support:** lossy encoding, or YUV input; convert to RGB first.

### JPEG (`libjpeg-turbo`)

- **Supports:** lossy, 8-bit planar YCbCr at 4:2:0, 4:2:2, 4:4:0 or 4:4:4 (`yuv*p` or `yuvj*p`). BT.601, full range, centered chroma. Quality is an IJG quality from 1 to 100. ICC profiles are embedded, and the sample aspect ratio is written as the JFIF density.
- **Does not support:** lossless JPEG, RGB input, limited range, any matrix other than BT.601, or chroma that is not centered. libjpeg-turbo's own RGB to YCbCr step is never used (rule 1).

### Motion JPEG (`mjpeg`)

- **Supports:** FFmpeg's JPEG encoder, with the same color rules as libjpeg-turbo. `yuv420p`, `yuv422p`, `yuv444p` and their `yuvj` forms. Quality is a qscale from 1 to 31, lower is better.
- **Does not support:** 4:4:0, lossless, RGB input, or anything libjpeg-turbo refuses.

### WebP (`libwebp`)

- **Supports, lossy:** `yuv420p` and `yuva420p`, BT.601, limited range, centered chroma. Quality from 0 to 100, higher is better.
- **Supports, lossless:** `bgra`. The rate control value is an effort from 0 to 100, higher is slower and smaller.
- **Does not support:** RGB or YUV in the other mode, which libwebp would convert itself (rule 1). Lossy WebP needs the frame's primaries and transfer kept as they are, since converting between primaries is not implemented. Animated WebP is not written.

### AVIF (`libaom-av1`, then `libsvtav1`, then `librav1e`)

- **Supports:** lossy only. libaom takes `yuv420p`, `yuv422p`, `yuv444p`, `gbrp` and `gray` at 8, 10 or 12 bits. Matrix and range are written to the file. Quality is crf from 0 to 63 for libaom, crf from 1 to 63 for libsvtav1 (`yuv420p` and `yuv420p10le` only), and a quantizer from 0 to 255 for librav1e. lossylab uses the first of the three encoders the build has.
- **Does not support:** lossless AVIF, or choosing among the three encoders. `rgb24` is repacked to `gbrp` for libaom (rule 3). RGB whose primaries or transfer are not sRGB raises `ConfigError` unless the frame carries an ICC profile, since FFmpeg's libaom wrapper labels all RGB sRGB.
- **Drops:** the chroma location of 4:2:0 and 4:2:2 frames with libaom, whose FFmpeg wrapper never writes it, and centered chroma with any encoder, since AV1 cannot express it. `Strict.Refuse` raises `ConversionRefused`; `Strict.AllowRecorded` records the drop (rule 4). libsvtav1 and librav1e keep left and top-left chroma.

### JPEG XL (`libjxl`)

- **Supports:** lossy and lossless. RGB and gray only: `rgb24`, `rgba`, `rgb48le`, `rgba64le`, the float formats, `gray`, `ya8`, `gray16le`. Lossy quality is a Butteraugli distance from 0.01 to 15, lower is better. libjxl codes lossy images in XYB (rule 2).
- **Does not support:** YUV input, or limited range.

### JPEG 2000 (`jpeg2000`)

- **Supports:** lossy and lossless (the 5/3 wavelet). RGB, `gbrp`, gray and planar YUV from 4:1:0 to 4:4:4, with or without alpha, from 8 to 16 bits. Lossy quality is FFmpeg's `layer_rates`, a nominal compression ratio from 1 to 1000, lower is better. FFmpeg's encoder applies no component transform.
- **Does not support:** color the JP2 header would misstate, which raises `ConfigError` (rule 4). The header labels every RGB file sRGB, so RGB must be sRGB at full range. It labels every YUV file sYCC, so YUV must be BT.601 at full range on sRGB primaries and transfer, with top-left chroma when subsampled. Gray is not checked. FFmpeg's encoder writes no ICC profile.

### HEIF

- **Does not support:** encoding. `encode_image()` raises `UnsupportedCapability`, since the build has no HEIF muxer.
