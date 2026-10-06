# Running a processing chain with `Pipeline`

A `PipelineSpec` lists the stages to run on one image, in order. `Pipeline(spec).run(source)` decodes the file and runs every stage in memory. It returns the final frame and one `ProcessingRecord` holding every stage. The record is enough to run the same chain again.

## Usage

```python
import numpy
import lossylab

to_srgb = lossylab.ConvertOptions()
to_srgb.pixel_format = lossylab.PixelFormat.from_name("rgb24")
to_srgb.color = lossylab.ColorSpec.srgb()

decode = lossylab.DecodeImageOptions()
decode.conversion = to_srgb

crop = lossylab.CropOptions()
crop.x, crop.y, crop.width, crop.height = 16, 16, 1024, 768

orient = lossylab.OrientOptions()
orient.orientation = 6

jpeg_color = lossylab.ColorSpec.jpeg()  # full-range BT.601 YCbCr...
jpeg_color.primaries = lossylab.ColorPrimaries.Bt709  # ...with sRGB's primaries and transfer
jpeg_color.transfer = lossylab.TransferCharacteristic.Srgb

jpeg = lossylab.RoundtripImageConfiguration()
jpeg.encode.codec = lossylab.ImageCodec.Mjpeg
jpeg.encode.pixel_format = lossylab.PixelFormat.from_name("yuvj420p")
jpeg.encode.color = jpeg_color
jpeg.encode.rate_control = lossylab.RateControl.quality(2)
jpeg.encode.strict = lossylab.Strict.AllowRecorded
jpeg.decode.conversion = to_srgb

spec = lossylab.PipelineSpec()
spec.add(decode).add(crop).add(orient).add(lossylab.AchromaticOptions()).add(jpeg)

source = lossylab.Source.from_path("photo.jpg")
spec.source_sha256 = source.sha256()  # optional: refuse any other file
result = lossylab.capture_run(source, lossylab.Pipeline(spec))
if result:
    pixels = result.value().frame.to_numpy()  # (height, width, 3) uint8, C-contiguous
    numpy.save("equalized.npy", pixels)
    record = result.value().record.to_dict()  # plain dicts, ready to store as JSON
```

The array is an ordinary NumPy array, so any writer that takes one (an HDF5 dataset, a memory map) can store it. [examples/equalize.py](../examples/equalize.py) runs a whole steps file on a thread pool.

- `Pipeline(spec)` validates the spec against this build before anything runs. It checks that there are stages, that a decode comes first or not at all, that every stage can run in a pipeline, and that the build has every encoder and backend the spec names.
- `run(source)` needs a decode as the first stage. `run(frame)` starts from a frame and must not have one.
- `capture_run()` returns a failing file as a `FileError` instead of raising. Use it on a thread pool; `run` releases the GIL.
- `PipelineSpec.to_dict()` and `from_dict()` (or `parse()` for JSON text) store and load a spec.

## Stages

Each stage is the options of the operation it runs. These are the same options the record stores as the stage's configuration.

| Options                       | Operation            | What it does                                                                                     |
|-------------------------------|----------------------|--------------------------------------------------------------------------------------------------|
| `DecodeImageOptions`          | `decode_image()`     | Decodes the file; its `conversion` takes the frame to a format and color, such as sRGB rgb24     |
| `ConvertOptions`              | `convert()`          | Changes the pixel format and color                                                               |
| `ChromaRoundtripOptions`      | `chroma_roundtrip()` | Subsamples the chroma and back                                                                   |
| `ReinterpretOptions`          | `reinterpret()`      | Relabels the color without changing a sample                                                     |
| `CropOptions`                 | `crop()`             | Cuts out a rectangle, copying the samples exactly                                                |
| `OrientOptions`               | `orient()`           | Rotates and flips upright from an EXIF orientation, moving the samples exactly                   |
| `AchromaticOptions`           | `achromatic()`       | Sets R, G and B of an rgb24 frame to its BT.601 luma                                             |
| `RoundtripImageConfiguration` | `roundtrip()`        | Encodes the frame in memory and decodes it back                                                  |

`crop()`, `orient()` and `achromatic()` can also be called alone, like `convert()`.

## Order

The order of the stages is part of the result:

- **Crop before an encode.** The new compression's grid then starts where the crop starts. A crop on the prior JPEG grid makes the new grid coincide with it, an aligned double compression.
- **Orient before an encode.** A transposing orientation turns 4:2:2 into 4:4:0, so after an encode it would change the subsampling the encode produced.
- **Crop before orient, when the crop is placed on the prior grid.** The grid is known in the file's stored orientation. A crop whose width and height are whole blocks keeps its grid after any rotation or flip.
- **Colors and alpha before achromatic.** The luma is then computed from the sRGB colors.

## Decoding to sRGB rgb24

A conversion to rgb24 takes one path for every source:

1. **swscale** converts to 16-bit RGB with accurate rounding, bit-exact arithmetic, full chroma interpolation and one thread. The chroma upsampler named in `chroma_up` is the one used at every frame size. Bicubic is swscale's own default and `ConvertOptions`' too. RGB, gray and palette sources of 8 bits are only repacked, then widened exactly (v × 257).
2. **Colors**, with `icc` set to `Convert`, the default. The ICC profile, or for a frame without one its primaries and transfer, describes the source. lcms2 converts it to sRGB with the relative colorimetric intent and black point compensation, evaluating the profile for every pixel. A profile equivalent to sRGB, and a frame tagged sRGB without a profile, are not converted, so they decode alike. The record's `icc_profile` conversion and the evidence's `color_transform` and `icc_profile_sha256` say what was applied. The converted profile is removed from the frame.
3. **Alpha**, with `alpha` set to `OverBlack`, the default. The colors are composited over black in their encoded sRGB values. A frame FFmpeg marks premultiplied already is, and only drops its alpha.
4. **One rounding** to 8 bits, with no dither. An 8-bit RGB, gray or palette source keeps its values exactly.

Not converted:

- **CMYK JPEGs.** FFmpeg's decoder folds CMYK to RGB itself, without the profile. The record says `not applied: CMYK folded to RGB by the decoder`, and the frame drops the profile.
- **HDR transfers** (PQ, HLG) raise `NotImplemented`.

The ICC conversion costs about 1.1 s per 6-megapixel image on one core, against 0.2 s for the whole decode of an image without one.

The four-argument `convert(frame, pixel_format, color, strict)` repackages the samples only. It applies no profile and drops an alpha channel the target lacks.

## Where a crop falls on the prior grid

A decoded JPEG records the grid of its minimum coded units as the decode stage's `block_grid`, of kind `JpegMcu`: 8×8 for 4:4:4 and gray, 16×8 for 4:2:2, 16×16 for 4:2:0. In a pipeline, a crop stage receives that grid. Its evidence holds:

- `block_grid`: the grid in the crop's coordinates. A nonzero `phase_x` or `phase_y` means the crop does not start on a block corner.
- `whole_blocks`: whether the crop's width and height are whole blocks.

Misaligned crops are allowed. The evidence states them.

## Output and replay

- `record.output_sha256` is `Frame.samples_sha256()` of the output: a SHA-256 over its description and its samples, row by row without padding.
- `record.seed` is the seed `run()` was given. No stage draws at random yet.
- `PipelineSpec.from_record(record)` turns a record back into its spec: the configurations the stages ran with, and the source's hash from the decode. Running it on the same file with the same build gives the same `output_sha256`.
- `spec.spec_id()` is a SHA-256 of the spec. Two specs with the same id run the same stages, with the same options, in the same order.
- The build's `identity_hash` includes lcms2 and zlib besides FFmpeg (see [output.md](output.md)). Compare output hashes only between records of the same build.
