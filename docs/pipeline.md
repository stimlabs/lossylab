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
jpeg.encode.rate_control = lossylab.RateControl.quality(2)  # FFmpeg qscale 2, not an IJG quality
jpeg.encode.strict = lossylab.Strict.AllowRecorded
jpeg.decode.conversion = to_srgb

spec = lossylab.PipelineSpec()
spec.add(decode).add(crop).add(orient).add(lossylab.AchromaticOptions()).add(jpeg)

source = lossylab.Source.from_path("photo.jpg")
spec.source_sha256 = source.sha256()  # optional: refuse any other file
result = lossylab.capture_run(source, lossylab.Pipeline(spec), seed=0)
if result:
    pixels = result.value().frame.to_numpy()  # (1024, 768, 3) uint8: orientation 6 swaps width and height
    numpy.save("equalized.npy", pixels)
    record = result.value().record.to_dict()  # plain dicts, ready to store as JSON
else:
    print(result.error().message)  # the file failed; result.error().to_dict() stores it
```

The array is an ordinary NumPy array, so any writer that takes one (an HDF5 dataset, a memory map) can store it. [examples/equalize.py](../examples/equalize.py) runs a whole steps file on a thread pool.

- `Pipeline(spec)` validates the spec against this build before anything runs. It checks that there are stages, that a decode comes first or not at all, that every stage can run in a pipeline, and that the build has every encoder and backend the spec names. `spec.validate()` runs the same check without building a pipeline.
- `run(source, seed=0)` needs a decode as the first stage. `run(frame, seed=0)` starts from a frame and must not have one.
- `capture_run(source, pipeline, seed=0)` returns a failing file as a `FileError` instead of raising. Use it on a thread pool; `run` releases the GIL.
- `PipelineSpec.to_dict()` and `from_dict()` (or `parse()` for JSON text) store and load a spec.
- `add(options, label="")` appends a stage and returns the spec. The label tells apart several stages of the same kind and appears in error messages.

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

Only these eight run in a pipeline. `add()` also accepts the other stage configurations a record can hold (`EncodeImageOptions`, `MeasureOptions`, `CompareOptions`, `CompressionHistoryOptions`, the video stages), but `Pipeline(spec)` rejects them with `ConfigError`. A pipeline always ends in a frame; to keep the encoded bytes, call `encode_image()` on the result.

`crop()`, `orient()` and `achromatic()` can also be called alone, like `convert()`.

### Decode

- `conversion` left unset delivers the frame in the codec's native format and color, for example yuvj420p for most JPEGs. `Frame.to_numpy()` takes only a frame with one plane (rgb24, gray, ...) and raises `ConfigError` for a planar frame; read its planes with `plane()`. A pipeline meant to end in an RGB array sets `conversion` to rgb24 sRGB, as above.
- `orientation` defaults to `Report`: the pixels stay as stored and the frame carries the orientation the file declares. `Apply` turns them upright during the decode instead of in an orient stage. The evidence's `orientation_handling` says which happened: `reported`, `applied`, or `applied_by_decoder` for JPEG XL, whose decoder turns the image upright itself. Do not add an orient stage after `applied` or `applied_by_decoder`.
- `assumed_color` (sRGB by default) fills the color fields the file leaves unspecified. The record states each assumption as a conversion.

### Crop

- `x`, `y`, `width` and `height` are in the input frame's pixels, which for a decode with `Report` is the file's stored orientation.
- The rectangle must fit in the frame. A crop that does not fit fails with `crop() of WxH at (x, y) does not fit in a WxH frame`; nothing is clamped. Every corner must also fall on a whole chroma sample of every plane, which only matters for a subsampled frame, not for rgb24.

### Orient

- `orientation` is the EXIF orientation, 1 to 8, to turn the frame upright from. 1 copies the frame unchanged; values outside 1 to 8 fail. The spec states the orientation itself, since a spec is written before the file is decoded: read it from the file's `probe()` or from an earlier audit.
- The orient evidence's `frame_orientation` is the orientation the frame itself carried, so a spec that disagrees with the file is visible in the record.
- Orientations 5 to 8 transpose: width and height swap, and a 4:2:2 frame becomes 4:4:0. `strict` (default `AllowRecorded`) decides whether that layout change is allowed and recorded or refused. An rgb24 frame has no chroma layout to change.
- The output carries no orientation.

### Achromatic

- Takes rgb24 only. Each pixel becomes `(19595 R + 38470 G + 7471 B + 32768) >> 16` in all three channels, libjpeg's fixed-point BT.601 weights.
- A JPEG encode of the result gets the luma unchanged and both chroma planes at exactly 128, at 4:2:0, 4:2:2 and 4:4:4. Decoded back to rgb24, its three channels are equal.
- The evidence states how far from gray the input was: `channel_spread_mean`, `channel_spread_std` and `channel_spread_max` of the largest minus the smallest channel per pixel.

### Roundtrip

`encode` is an `EncodeImageOptions`, `decode` holds the `conversion` and `strict` of the decode back.

- **Quality.** `RateControl.quality(value)` takes each encoder's own scale. A value outside it fails with the scale in the message.

  | Codec      | `quality(value)`                                                   |
  |------------|--------------------------------------------------------------------|
  | `Mjpeg`    | FFmpeg's `qscale`, an integer from 1 to 31, lower is better        |
  | `WebP`     | quality from 0 to 100, higher is better                            |
  | `Jxl`      | Butteraugli distance from 0.01 to 15, lower is better              |
  | `Avif`     | depends on the AVIF encoder of the build (crf or quantizer)        |
  | `Jpeg2000` | FFmpeg's `layer_rates`, an integer from 1 to 1000, lower is better |

  `Mjpeg` sets FFmpeg's fixed `qscale` (`fixed_qscale`, `qmin` and `qmax` in the encode evidence). There is no setting for an IJG quality (libjpeg's 1 to 100), so a target given as an IJG quality has to be mapped to a `qscale` first.
- **Pixel format.** `Mjpeg` takes `yuvj420p`, `yuvj422p` and `yuvj444p`, and their limited-range names `yuv420p`, `yuv422p` and `yuv444p`. It does not take `gray`, `yuvj440p`, `yuvj411p` or an RGB format, so there is no single-component JPEG; an achromatic stage before the encode gives flat chroma instead. Lossy `WebP` takes `yuv420p` or `yuva420p` only.
- **Color.** `encode.color` must be set when the encode converts from RGB. `ColorSpec.jpeg()` alone has BT.470BG primaries and the SMPTE 170M transfer, and converting sRGB to those is not implemented, so the example keeps sRGB's primaries and transfer and changes only the matrix and range. JPEG needs full range, the BT.601 matrix and centered chroma; lossy WebP needs limited range.
- **`strict`.** The encode converts the rgb24 frame to the pixel format and color to encode in, and the decode back converts again. With `Strict.Refuse`, the default of `EncodeImageOptions`, the encode refuses that conversion with `ConversionRefused`; with `AllowRecorded` it runs it and lists every changed property (`pix_fmt`, `subsampling`, `color_matrix`, `chroma_location`, `color_tags`) in the stage record's `conversions`. The alternative is a `ConvertOptions` stage to the encode's format first. `EncodeImageOptions` is the only stage option that defaults to `Refuse`; `DecodeImageOptions`, `ConvertOptions`, `OrientOptions` and the roundtrip's `decode` default to `AllowRecorded`.

## Order

The order of the stages is part of the result:

- **Crop before an encode.** The new compression's grid then starts where the crop starts. A crop on the prior JPEG grid makes the new grid coincide with it, an aligned double compression.
- **Orient before an encode.** A transposing orientation turns 4:2:2 into 4:4:0, so after an encode it would change the subsampling the encode produced.
- **Crop before orient, when the crop is placed on the prior grid.** The grid is known in the file's stored orientation. A crop whose width and height are whole blocks keeps its grid after any rotation or flip.
- **Colors and alpha before achromatic.** The luma is then computed from the sRGB colors.

## Decoding to sRGB rgb24

A conversion to rgb24 takes one path for every source:

1. **swscale** converts to 16-bit RGB with accurate rounding, bit-exact arithmetic, full chroma interpolation and one thread. The chroma upsampler named in `chroma_up` is the one used at every frame size. Bicubic is swscale's own default and `ConvertOptions`' too. RGB, gray and palette sources of 8 bits are only repacked, then widened exactly (v × 257).
2. **Colors**, with `icc` set to `Convert`, the default. The ICC profile, or for a frame without one its primaries and transfer, describes the source. lcms2 converts it to sRGB with the relative colorimetric intent and black point compensation, evaluating the profile for every pixel. A profile equivalent to sRGB, and a frame tagged sRGB without a profile, are not converted, so they decode alike. The record's `icc_profile` conversion and the evidence's `color_transform` and `icc_profile_sha256` say what was applied. The converted profile is removed from the frame. `Ignore` leaves the profile unapplied.
3. **Alpha**, with `alpha` set to `OverBlack`, the default. The colors are composited over black in their encoded sRGB values. A frame FFmpeg marks premultiplied already is, and only drops its alpha. `Discard` drops the alpha without compositing.
4. **One rounding** to 8 bits, with no dither. An 8-bit RGB, gray or palette source keeps its values exactly.

Not converted:

- **CMYK JPEGs.** FFmpeg's decoder folds CMYK to RGB itself, without the profile. The record says `not applied: CMYK folded to RGB by the decoder`, and the frame drops the profile.
- **HDR transfers** (PQ, HLG) raise `NotImplemented`.

The ICC conversion costs about 1.1 s per 6-megapixel image on one core, against 0.2 s for the whole decode of an image without one.

The four-argument `convert(frame, pixel_format, color, strict)` repackages the samples only. It applies no profile and drops an alpha channel the target lacks.

## Where a crop falls on the prior grid

A decoded JPEG records the grid of its minimum coded units as the decode stage's `block_grid`, of kind `JpegMcu`: 8×8 for 4:4:4 and gray, 16×8 for 4:2:2, 16×16 for 4:2:0. In a pipeline, a crop stage receives that grid. Its evidence holds:

- `block_grid`: the grid in the crop's coordinates, with `kind`, `block_width`, `block_height`, `phase_x` and `phase_y`. A nonzero `phase_x` or `phase_y` means the crop does not start on a block corner. It is `None` when no grid was known, for example for a PNG source.
- `whole_blocks`: whether the crop's width and height are whole blocks.

Misaligned crops are allowed. The evidence states them. To find the origin of an aligned crop, round `x` and `y` to multiples of the block size (16 for 4:2:0) before building the spec.

```python
record = result.value().record
crop_evidence = record.stages()[1].evidence  # stages() follow the spec's order
print(crop_evidence.block_grid.phase_x, crop_evidence.block_grid.phase_y, crop_evidence.whole_blocks)
```

Each entry of `record.stages()` also holds the stage's `kind`, its `input` and `output` frame descriptions and the `conversions` it made. `record.to_dict()` holds the same under `stages`, beside `configurations`.

## Errors

| Raised by        | Error                   | When                                                                                                                                                                                                                                                   |
|------------------|-------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `Pipeline(spec)` | `ConfigError`           | No stages, a decode after the first stage, a stage kind that does not run in a pipeline                                                                                                                                                                |
| `Pipeline(spec)` | `UnsupportedCapability` | The build lacks a named encoder or resize backend                                                                                                                                                                                                      |
| `run()`          | `ConfigError`           | The spec's `source_sha256` is not the source's; a decode missing for a source or present for a frame; options a stage rejects (crop out of the frame, orientation outside 1 to 8, quality outside the scale, a pixel format the encoder does not take) |
| `run()`          | `ConversionRefused`     | A stage with `Strict.Refuse` would have to convert                                                                                                                                                                                                     |
| `run()`          | `NotImplemented`        | An HDR transfer, or a color conversion lossylab does not implement                                                                                                                                                                                     |
| `run()`          | `FFmpegError`           | FFmpeg fails to decode or encode                                                                                                                                                                                                                       |

All of them derive from `lossylab.Error`. `capture_run()` returns every error `run()` raises as a `FileError` with `kind` (a `FileErrorKind`: `Config`, `UnsupportedCapability`, `ConversionRefused`, `FFmpeg`, `NotImplemented`, `Library`, `OutOfMemory` or `Internal`), `operation` (`pipeline`), `source`, `message` and `details`; `result.log()` holds what FFmpeg logged meanwhile, as entries with `level`, `component` and `text`. Errors from `Pipeline(spec)` are raised in either case, before any file is read, so build the pipeline once and reuse it for every file with the same spec.

## Stored specs

`spec.to_dict()` gives `source_sha256` and the stages in order, each with its `kind`, its full `configuration` (defaults included) and its `label` when set. Shortened, with `...` for the fields left out:

```json
{
  "source_sha256": null,
  "stages": [
    {"kind": "decode_image", "configuration": {"conversion": {"pixel_format": "rgb24", "color": {"matrix": "gbr", "range": "pc", "primaries": "bt709", "transfer": "iec61966-2-1", "chroma_location": "unspecified"}, "chroma_up": {"kernel": "bicubic", "params": {"param_a": null, "param_b": null}}, "icc": "convert", "alpha": "over_black", "...": "..."}, "assumed_color": {"...": "..."}, "orientation": "report", "strict": "allow_recorded"}},
    {"kind": "crop", "configuration": {"x": 16, "y": 16, "width": 1024, "height": 768}, "label": "center"}
  ]
}
```

`from_dict()` and `parse()` accept the same document. `source_sha256` and `label` may be left out; every configuration field must be present, and a missing one raises `RuntimeError` naming the key (`key 'y' not found`). Write a spec with `to_dict()` rather than by hand.

## Output and replay

- `record.output_sha256` is `Frame.samples_sha256()` of the output: a SHA-256 over its description and its samples, row by row without padding.
- `record.seed` is the seed `run()` was given. A stage that draws at random will derive its own stream from the seed and its position, so one seed reproduces the whole run. No stage draws at random yet.
- `PipelineSpec.from_record(record)` turns a record back into its spec: the configurations the stages ran with, and the source's hash from the decode. Running it on the same file with the same build gives the same `output_sha256`.
- `spec.spec_id()` is a SHA-256 of the spec. Two specs with the same id run the same stages, with the same options, in the same order. Two specs compare equal with `==` when their `to_dict()` is equal.
- The build's `identity_hash` includes lcms2 and zlib besides FFmpeg (see [output.md](output.md)). Compare output hashes only between records of the same build.
