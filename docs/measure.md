# Measuring images with `measure()`

`measure()` computes statistics from an image alone, with no original to compare against: signal levels, blockiness, blurriness, noise, and black borders.

## Usage

```python
import lossylab

decoded = lossylab.decode_image(lossylab.Source.from_path("photo.jpg"))
options = lossylab.MeasureOptions()
options.strict = lossylab.Strict.AllowRecorded  # convert formats an analyzer can't measure, and record it

analyzers = [lossylab.Analyzer.Blockiness, lossylab.Analyzer.Noise]  # more available, see table below
result = lossylab.measure(decoded.frame, analyzers, options)
result.frames[0].blockiness  # a float, or None when the analyzer did not run or found nothing
result.pooled["blockiness"].mean  # summarized across frames; pooled is empty for a single frame
```

- Pass one frame, or a list of frames with the same size, format and color.
- Without `AllowRecorded`, a format an analyzer can't measure raises `ConversionRefused`. RGB PNGs decode as `rgb24`, which only `Letterbox` measures directly.
- For batches, `capture_measure(source, frames, analyzers, options)` returns failures as data instead of raising.
- `Interlacing`, `SpatialTemporalInfo`, `SceneChange` and `DuplicateFrames` are not implemented yet and raise `NotImplemented`.

## Reading the result

- **`frames[i]`:** one typed result per analyzer, listed in the table below. An analyzer that was not run leaves its result `None`.
- **`pooled`:** every number in the frames' results, summarized across the frames that have it. It is a dict keyed by the dotted path of the field, such as `"signal_levels.luma.mean"` or `"blockiness"`, and each value is a `Summary` with `count`, `mean`, `std`, `median`, `minimum` and `maximum`. `std` is the sample standard deviation (n − 1). `count` is the number of frames the number covers, which is fewer than the frame count when some frames have no value. The frame `index` is not pooled. `pooled` is empty for a single frame, whose own values are in `frames[0]`. Report a mean together with its `std`.
- **`record`:** how the numbers were produced. `params["measured_as"]` gives the format each analyzer measured, and `conversions` lists any conversion applied.
- **`configuration`:** the options the run used (analyzers, methods, `Strict` mode), the same for every file. See [output.md](output.md).
- **`to_dict()`:** everything as plain dicts, ready to store as JSON.

A value can be `None` or NaN when there is nothing to measure: a frame without edges has `blurriness` `None`, while a frame without content has `blockiness` NaN. A NaN also reaches `pooled`, where it makes the mean, `std`, `median`, `minimum` and `maximum` NaN. Check for both before using a value.

| Analyzer       | Field on the frame                                                                                                                                                                                              | Meaning                                                                                                                                                                                   |
|----------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `SignalLevels` | `signal_levels`: `luma`, `chroma_u`, `chroma_v`, `saturation` (each with `minimum`, `percentile_10`, `mean`, `percentile_90`, `maximum`); `hue_mean`, `hue_median`; `luma_bit_depth`, `chroma_u_bit_depth`, `chroma_v_bit_depth`; `outside_limited_range` | Value ranges per channel, the bits actually used, and the share of pixels outside the TV range. Useful for spotting range mislabels, but only when `measured_as` is the file's own format |
| `Blockiness`   | `blockiness`                                                                                                                                                                                                    | Strength of a regular block grid. About 1 means none                                                                                                                                      |
| `Blurriness`   | `blurriness`                                                                                                                                                                                                    | Average edge width in pixels                                                                                                                                                              |
| `Noise`        | `noise_sigma`                                                                                                                                                                                                   | Grain and noise level, in 8-bit code values                                                                                                                                               |
| `Letterbox`    | `letterbox`: `content_rect`, `content_fraction`, and `bars` (`top`, `bottom`, `left`, `right`)                                                                                                                  | Black bars in pixels, and the content area inside them. `bars` is `None` for a frame that is black throughout                                                                             |

## What these metrics can't see

Blockiness, blurriness and noise read one channel only.

- **Color-only artifacts are invisible:** chroma banding, color bleeding, chroma-only noise, and blockiness or blur that lives only in the color channels. Subsampled color is blocky and soft by nature, so chroma blockiness and blur would be a weak signal anyway. `compression_history()` detects chroma subsampling.
- **RGB and YUV give different numbers for the same image.** Blockiness and blurriness read the first channel: green for RGB, brightness for YUV. Noise reads brightness, converting RGB to YUV first. **Only compare values within one `measured_as` format.** Across formats, you are comparing two different measurements.
- **Blockiness and blurriness measure 8-bit samples only.** Frames with more bits per sample are converted down to 8 bits first.

## Interpreting the numbers

These ranges come from a calibration run on 1250 real images from 25 sources: photo collections, screenshots, and GAN and diffusion outputs. Each image was labeled by the compression history found in its file.

**Blockiness is reliable.** It separates compressed from clean images.

| Images              | Median | 95th percentile |
|---------------------|--------|-----------------|
| Clean PNGs          | 1.07   | 1.22            |
| JPEG, quality 95+   | 1.20   | 1.62            |
| JPEG, quality 90–94 | 1.71   | 2.16            |
| JPEG, quality 70–79 | 2.79   | 6.35            |

Suggested bands (provisional): **below 1.3** none, **1.3–2** mild, **2–3.5** moderate, **above 3.5** strong.

- About 3% of clean images read above 1.3.
- Content spreads the readings widely at any one JPEG quality. Blockiness measures how visible the artifacts are, not the quality setting; `compression_history()` gives the quality.
- Lossless screenshots and flat graphics are untested. On flat synthetic images, blockiness read up to 114, so treat high values on UI, text or diagrams with suspicion.

**Blurriness is not an absolute score.** The median was 4.6–6.4 in every source and class, compressed or not. The spread reflects content, such as shallow depth of field in portraits. Use it only to compare an image with a processed version of itself, or to compare the median of whole sources that show similar content.

**Noise measures grain and texture, not compression.** Clean sources ranged from a median of 0.3 to 3.0. Strong JPEG compression *lowers* it: quality 70–79 had a median of 0.7, against 2.2 at quality 95+. Set thresholds only per source or relative to a reference.

**Read blurriness and noise together.** A low noise value alone is ambiguous: smoothing, upscaling, denoising, strong compression and naturally smooth content all lower it. Blurriness helps tell these apart:

| Compared with the original, or with similar sources | Likely cause                               |
|-----------------------------------------------------|--------------------------------------------|
| Blurriness higher, noise much lower                 | Blurred or smoothed                        |
| Blurriness about the same, noise much lower         | Upscaled, denoised, or strongly compressed |
| Both about the same                                 | No smoothing or resampling                 |

Blockiness separates the middle row: it is high after strong compression and stays low after upscaling or denoising.

On six test images, a Gaussian blur of radius 1.5 raised blurriness by 2.5–3.5 and cut noise to about a tenth. A 2× bicubic upscale left blurriness within ±1, with no consistent direction, and cut noise to between a quarter and an eighth. Upscaling shows up in noise, not in blurriness.

**SignalLevels tells you what the code values mean, not how compressed the image is.** It was not part of the calibration above, so read it against the file's own declared range rather than against a threshold.

- **Limited or full range:** look at `signal_levels.luma.minimum` and `.maximum`, and at `.percentile_10` and `.percentile_90`, which single stray pixels don't move. Limited range keeps luma in about 16–235 (8 bit) with `outside_limited_range` near 0. Full range reaches close to 0 and 255, so `outside_limited_range` is large.
- **Mislabeled range:** compare the measured levels with the range the file declares. Full-range content tagged as limited is expanded a second time by the decoder, so blacks crush and whites clip; here `outside_limited_range` is large although the tag says limited. Limited-range content tagged as full looks washed out; here luma sits in 16–235 although the tag says full.
- **Bit depth:** a `*_bit_depth` is the format's depth less the low bits that are zero in every sample of the frame. One below the format's depth means the samples use fewer bits than the container holds, as with 8-bit material stored as 10-bit. It also drops for content with few distinct values: a flat frame reads as very few bits, so judge it over real photos. Check all three planes; they can differ.
- **Chroma:** `chroma_u.mean` and `chroma_v.mean` should sit near neutral (128 at 8 bit). A large offset points to a color cast, a wrong matrix, or a chroma-siting problem. `saturation` and the `hue_*` values show whether frames are grayscale, washed out, or oversaturated.
- **One frame is not enough:** a dark scene has a low `luma.maximum` because of its content, not its range. Judge range on percentiles pooled over a clip or a source.
- **Some overshoot is normal:** limited-range material can carry super-blacks and super-whites, so a small nonzero `outside_limited_range` says nothing. A large fraction does.
- **Converted frames:** `SignalLevels` measures planar YUV only. Under `AllowRecorded`, an RGB frame is converted first, and the levels then describe the conversion's output. That is a valid measurement of what the converted data looks like, but it says nothing about a range the file itself declared. Read it against `measured_as`.

**Watch for:**
- **Blank images:** NaN blockiness, no blurriness and zero noise mean "no content". On an all-black image, `Letterbox` gives a `content_fraction` of 0 and no `bars`. Flag these images; don't count them as clean.
- **RGB images:** blockiness and blurriness are measured on the green channel, and noise on brightness after a conversion. Never mix `measured_as` formats in one comparison (see [What these metrics can't see](#what-these-metrics-cant-see)).
- **Differences between sources:** in the calibration set, photo sources were mostly JPEGs (blockiness around 2–3), while many generated-image sources were clean PNGs (about 1.07). A model trained to separate such classes can learn compression instead of content. Compare these distributions between classes before training.

## Measuring a corpus

**Setup:**
- Decode everything with `decode_image()`. Mixing decoders mixes pixels.
- Use `Strict.AllowRecorded` for the whole run, so RGB files don't drop out.
- Run `capture_decode_image()` and `capture_measure()` on a thread pool: a failing file becomes one error, not a stopped run. `examples/pilot_audit.py` is a working template.

**Statistics:**
- Split first: by compression history (`compression_history()`), by `measured_as`, and by image size.
- Use medians and percentiles. Single images reach extreme values; the mean follows them.
- Keep 30–50 images or more per group. Smaller groups give noisy numbers.
- Drop blank images. Flag letterboxed ones (`content_fraction` below 1); black bars skew noise and blurriness. Rows and columns darker than 24/255 count as black, so dark photos with dark edges may be flagged too.

**Auditing levels with `SignalLevels`:**
- Run it first, before other analyzers or anything that depends on absolute code values (`Noise` in code values, the `Letterbox` luma threshold, PSNR or MSE). A range mismatch shifts all of them.
- Keep converted files in. Training data usually ends up as an RGB uint8 matrix anyway, and their levels show what the model will see. Record `measured_as` and compare only within one value of it: a file's own YUV format answers whether its range tag is right, a converted format answers what the converted data looks like.
- Group by declared range and color tags, then look for groups whose measured levels disagree with the tag. Those are the mislabeled ones.
- Pool per source or clip, using the pooled `median` of `signal_levels.luma.percentile_10`, `signal_levels.luma.percentile_90` and `signal_levels.outside_limited_range`. A mislabeled range usually affects a whole source, and a pooled corpus hides it.
- Compare the range distribution between classes. If one class is mostly full range and another mostly limited, a model can learn the range instead of the content.
- Then decide per source: normalize the range explicitly and record it, or drop or flag the source.

**Comparing:**
- Compare sources with similar content. Portraits and screenshots differ by nature.
- Pool per source, then compare classes, so one big source doesn't dominate.
- Merge runs only when `schema_version` and `configuration["methods"]` match. Store the full `to_dict()`.
