# Known Limitations

## FFmpeg

Each item states the FFmpeg version it was checked against and how: against the **executable's output** (`ffmpeg` and `ffprobe` on a test file), against the **source code** (the matching checkout), or both.

### WebP: XMP is not read

- **FFmpeg version:** n8.1.3
- **Verified against:** both the executable's output and the source code.
- **Executable:** `ffmpeg -i` on a WebP with an `XMP ` chunk prints `skipping unsupported chunk: XMP`, once per decoder context that opens the file. `ffmpeg -h decoder=webp` lists no private options, and `ffprobe -show_format` reports the format `webp_pipe` with no tags.
- **Source:** `libavcodec/webp.c` groups `XMP ` with `ANIM` and `ANMF` in a case that logs the warning and skips the chunk, and it defines no options. The `webp_pipe` demuxer (`libavformat/img2dec.c`, `ff_img_read_packet`) reads the file into one packet and parses no chunks.

FFmpeg keeps the payload nowhere, so no configuration exposes it. Both `probe()` and `decode_image()` open the file through FFmpeg, so the warning appears in each. It does not fail either stage. lossylab does not parse XMP (see the README's scope notes). `ImageContainerInfo.has_xmp` reports whether the chunk is present, found by lossylab's own RIFF walker, without reading the payload.

### WebP: an EXIF chunk with the `Exif\0\0` prefix is dropped by the decoder

- **FFmpeg version:** n8.1.3
- **Verified against:** both the executable's output and the source code.
- **Executable:** a WebP whose EXIF chunk starts `MM\0*` gives the frame EXIF side data and a display matrix (rotation -90), with no warnings. The same file with `Exif\0\0` in front of the TIFF header prints `invalid TIFF header in EXIF data: Invalid data found when processing input` and `unable to attach EXIF buffer`, and the frame has no EXIF side data and no display matrix.
- **Source:** the `EXIF` case in `libavcodec/webp.c` calls `ff_decode_exif_attach_buffer(..., AV_EXIF_TIFF_HEADER)`, which reaches the error in `av_exif_parse_buffer` (`libavcodec/exif.c`). The header mode is fixed in the call, and no option changes it.

The WebP specification puts the TIFF header first in the chunk. Some writers add the JPEG-style prefix, and FFmpeg rejects those files. lossylab works around the lost orientation: when FFmpeg attaches none to a WebP frame, `decode_image()` reads the EXIF chunk itself, with the reader `probe()` uses, which accepts both forms. The two FFmpeg messages are still logged, not suppressed.

### WebP: animated files do not decode

- **FFmpeg version:** n8.1.3
- **Verified against:** both the executable's output and the source code.
- **Executable:** `ffmpeg -i` on an animated WebP prints `skipping unsupported chunk: ANIM`, `skipping unsupported chunk: ANMF`, then `image data not found` and `Could not find codec parameters for stream 0 (Video: webp, none): unspecified size`.
- **Source:** `libavcodec/webp.c` skips `ANIM` and `ANMF` chunks (the same case as `XMP `), so it never finds image data in an animation.

The stream reports 0x0. `probe()` reads the canvas size, frame count and compression from the RIFF chunks instead.

### JPEG: CMYK is folded to RGB without its profile

- **FFmpeg version:** n8.1.3
- **Verified against:** the source code.
- **Source:** for an Adobe CMYK JPEG (`adobe_transform` 0), `libavcodec/mjpegdec.c` (lines 2813 to 2834) multiplies each of C, M and Y by K and outputs `gbrap` with the alpha plane set to 255. A YCCK JPEG (`adobe_transform` 2) is folded the same way into `yuva444p`. The embedded CMYK profile is never applied, and the CMYK samples are not kept.

A conversion to sRGB rgb24 therefore cannot apply a CMYK profile. It records the profile as `not applied: CMYK folded to RGB by the decoder` and drops it from the frame.

### swscale: 4:2:0 and 4:2:2 to RGB repeat chroma at even sizes

- **FFmpeg version:** n8.1.3
- **Verified against:** the source code.
- **Source:** `libswscale/swscale_unscaled.c` (lines 2414 to 2417) converts yuv420p, yuv422p and yuva420p of even height to RGB with `ff_yuv2rgb_get_func_ptr`, which repeats each chroma sample, unless `SWS_ACCURATE_RND` is set. `handle_jpeg()` in `libswscale/utils.c` turns the yuvj formats into these first. An odd width forces full chroma interpolation instead (`utils.c`, line 1247), and an odd height takes the general scaler. Without the flag, the chroma upsampler depends on the image's size, not on the kernel asked for.
- **Source, bit depth:** a source of more than 8 bits converted to 8 bits gets an 8×8 ordered dither (`libswscale/swscale.c`, lines 291 and 521).

`convert()` always sets `SWS_ACCURATE_RND | SWS_BITEXACT`, and `SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP` when either side is RGB. A conversion to rgb24 goes through 16-bit RGB and rounds to 8 bits itself, without dither.
