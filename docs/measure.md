# Measuring images with `measure()`

`measure()` computes statistics from an image alone, with no original to compare against: signal levels, blockiness, blurriness, noise, and black borders.

## Usage

```python
import lossylab

decoded = lossylab.decode_image(lossylab.Source.from_path("photo.jpg"))
options = lossylab.MeasureOptions()
options.strict = lossylab.Strict.AllowRecorded  # convert formats an analyzer can't measure, and record it

result = lossylab.measure(decoded.frame, [lossylab.Analyzer.Blockiness, lossylab.Analyzer.Noise], options)
result.frames[0].value("blockiness")  # a float, or None when absent
```

- Pass one frame, or a list of frames with the same size, format and color.
- Without `AllowRecorded`, a format an analyzer can't measure raises `ConversionRefused`. RGB PNGs decode as `rgb24`, which only `Letterbox` measures directly.
- For batches, `capture_measure(source, frames, analyzers, options)` returns failures as data instead of raising.
- `Interlacing`, `SpatialTemporalInfo`, `SceneChange` and `DuplicateFrames` are not implemented yet and raise `NotImplemented`.

## Reading the result

- **`frames[i].values`:** a dict of named values for each frame.
- **`pooled`:** `<name>_min`, `<name>_mean` and `<name>_max` across frames.
- **`record`:** how the numbers were produced. `params["measured_as"]` gives the format each analyzer measured, and `conversions` lists any conversion applied.
- **`to_dict()`:** everything as plain dicts, ready to store as JSON.

A value can be absent or NaN when there is nothing to measure: `blurriness` is left out, while `blockiness` is NaN. A NaN also reaches `pooled`, where it makes the mean NaN. Check for both before using a value.

| Analyzer       | Values                                                                                                                                         | Meaning                                                                                                                                                                                   |
|----------------|------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `SignalLevels` | `luma_*`, `u_*`, `v_*`, `saturation_*` (`min`, `low`, `mean`, `high`, `max`); `hue_mean`, `hue_median`; `*_bit_depth`; `outside_limited_range` | Value ranges per channel, the bits actually used, and the share of pixels outside the TV range. Useful for spotting range mislabels, but only when `measured_as` is the file's own format |
| `Blockiness`   | `blockiness`                                                                                                                                   | Strength of a regular block grid. About 1 means none                                                                                                                                      |
| `Blurriness`   | `blurriness`                                                                                                                                   | Average edge width in pixels                                                                                                                                                              |
| `Noise`        | `noise_sigma`                                                                                                                                  | Grain and noise level, in 8-bit code values                                                                                                                                               |
| `Letterbox`    | `letterbox_top`/`bottom`/`left`/`right`, `content_fraction`, and `content_rect` on the frame                                                   | Black bars in pixels, and the content area inside them                                                                                                                                    |

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

**Watch for:**
- **Blank images:** NaN blockiness, no blurriness and zero noise mean "no content". On an all-black image, `Letterbox` gives a `content_fraction` of 0 and no `letterbox_*` values. Flag these images; don't count them as clean.
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

**Comparing:**
- Compare sources with similar content. Portraits and screenshots differ by nature.
- Pool per source, then compare classes, so one big source doesn't dominate.
- Merge runs only when `schema_version` and `record.params["methods"]` match. Store the full `to_dict()`.
