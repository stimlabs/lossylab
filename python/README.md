# Python bindings

A nanobind extension module, `lossylab._lossylab`, re-exported whole from the `lossylab` package. It covers the
part of the C++ API that is implemented today; the scaffolded operations are not bound yet.

## Building

The package builds through scikit-build-core, which turns `BUILD_PYTHON_BINDINGS` on and the C++ tests and tools
off (see `pyproject.toml`). From the repository root:

```sh
uv sync --extra test                                # first build
uv sync --extra test --reinstall-package lossylab   # after any C++ change
uv run pytest python/tests
```

There is no editable rebuild-on-import. It needs a build directory that persists across builds, and uv's
build-isolation environment, where cmake, ninja and nanobind live during the build, is torn down afterward, leaving
stale tool paths behind. Re-run `uv sync` after editing C++ sources.

The build also generates the type stub `_lossylab.pyi` with `nanobind_add_stub` and installs it next to the compiled
extension, and `py.typed` marks the package as typed. `lossylab/__init__.py` star-imports from `_lossylab`, so type
checkers resolve every public name through the stub.

## What is bound

| Module file             | Surface                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
|-------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `bind_env.cpp`          | `build_info()`, `capabilities()`, `hardware_device_usable()`, `permits_proprietary_distribution()`, `set_log_handler()`, `mute_log()`, `log_level()`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| `bind_io.cpp`           | `Source.from_path` / `from_bytes` / `from_memory`, `probe()` with `ProbeResult`, `StreamInfo`, `ImageContainerInfo`, `JpegInfo`, `TileGrid`, `decode_image()` with `DecodeImageOptions`, `OrientationHandling`, `DecodedImage`, `DecodeImageEvidence`, `read_headers()` with `ReadHeadersOptions`, `HeaderInfo`, `ParameterSet`, `SliceInfo`                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `bind_video_reader.cpp` | `VideoReader` with `VideoReaderOptions`, `FrameSelector`, `VideoFrame`, `MotionVector`, and its `DecodeVideoConfiguration` / `DecodeVideoEvidence`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `bind_measure.cpp`      | `measure()` (frames or one frame, with `MeasureOptions` or a list of `Analyzer`) returning `MeasureResult`, with `MeasureEvidence`, `FrameMeasurement`, `SignalLevels`, `Levels`, `Letterbox`, `LetterboxBars`, `Summary`; `compare()` (with `CompareOptions` or a list of metrics) returning `CompareResult`, with `CompareEvidence`; `compression_history()` with `CompressionHistoryOptions`, `CompressionHistory`, `CompressionHistoryEvidence`, `CompressionTrace`, `TraceEvidence`, `JpegQuantizationEvidence`, `QuantizationEstimate`, `JpegTableAgreement`, `JpegPixelAgreement`, `ChromaSubsamplingEvidence`, `ChromaUpsampling`, `RecompressionOutcome`, and the `RecompressionSweep` / `RecompressionCurveEvidence` / `RecompressionPoint` sweeps it returns; `RecompressionCurve` |
| `bind_encode.cpp`       | `encode_image()`, `encode_video()`, `roundtrip()` (image or clip), `encode_to_target()` with `RateControl`, `GopStructure`, `EncodeImageOptions`, `EncodeVideoOptions`, `DecodeSpec`, `EncodeTarget`, `EncodedResult` (`bytes` as Python bytes), `FramesResult`, `EncodeToTargetResult` (its `search` as `EncodeSearch` / `EncodeAttempt`), `EncodeImageEvidence`, `EncodeVideoEvidence`, `RoundtripImageConfiguration` / `RoundtripImageEvidence`, `RoundtripVideoConfiguration` / `RoundtripVideoEvidence`                                                                                                                                                                                                                                                                                  |
| `bind_file_result.cpp`  | `capture_probe()`, `capture_decode_image()`, `capture_read_headers()`, `capture_video_frames()`, `capture_measure()`, `capture_compare()`, `capture_compression_history()` returning `ProbeFileResult` / `DecodeImageFileResult` / `ReadHeadersFileResult` / `VideoFramesFileResult` / `MeasureFileResult` / `CompareFileResult` / `CompressionHistoryFileResult`, `VideoFramesResult`, `FileError`, `FileErrorKind`                                                                                                                                                                                                                                                                                                                                                                          |
| `bind_convert.cpp`      | `convert()` (both overloads) with `ConvertOptions`, `chroma_roundtrip()` with `ChromaRoundtripOptions`, `reinterpret()` with `ReinterpretOptions` or a `ColorSpec`, and `ConvertEvidence`, `ChromaRoundtripEvidence`, `ReinterpretEvidence`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `bind_frame.cpp`        | `Frame`, `QpMap`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| `bind_record.cpp`       | `StageRecord`, `StageKind`, `ProcessingRecord`, `FrameResult`, `FormatDescription`, `FrameStats`, `PictureType`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `bind_core_types.cpp`   | `ColorSpec` and its enums, `PixelFormat`, `Subsampling`, `Strict`, `ConversionEvent`, `Kernel` / `KernelSpec`, `Rational`, `Point`, `Rect`, `CoordinateTransform`, `BlockGrid`, codec and metric enums, `Availability`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `bind_errors.cpp`       | The `Error` hierarchy as Python exceptions, with the extra fields each C++ subclass carries                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |

Records, probe results and decoded images have `to_dict()`, which converts the C++ JSON document into plain Python dicts and lists.

A `StageRecord`'s `evidence` is one of the `*Evidence` classes above, and a stage's configuration is its `*Options` class or a `*Configuration` class; both come across as the concrete Python class, so `isinstance()` tells the kinds apart, and `stage.kind` is the `StageKind` of its evidence. A result's `evidence` (and `EncodeToTargetResult.search`) reads its record's evidence. See [docs/output.md](../docs/output.md).

## Design points

- **Zero-copy planes.** `Frame.plane(i)` returns a read-only NumPy view onto the frame's buffer, built from the
  plane's byte stride rather than its width, since FFmpeg pads rows for alignment. Packed formats get a trailing
  component axis; planar ones stay 2-D. Samples are `uint8` or `uint16`. The view keeps the `Frame` alive.
- **Writing requires detaching.** `Frame` is reference-counted and copies share buffers. `Frame.writable_plane(i)`
  calls `make_writable()` first, so writing never reaches another frame's samples.
- **GIL release.** `probe`, `decode_image`, `read_headers`, `measure`, `compare`, `compression_history`, the encoders,
  `roundtrip`, `encode_to_target`, `convert`, `chroma_roundtrip`, `reinterpret`, the `VideoReader`
  constructor, `VideoReader.frames`, `VideoReader.for_each` and the `capture_*` functions release the GIL while they
  run.
- **Python callbacks.** nanobind's `std::function` caster takes the GIL on every call and keeps the Python callable
  alive. That covers `set_log_handler`, whose callback FFmpeg calls from its own threads, a `FrameSelector.where`
  predicate, and a `VideoReader.for_each` callback. An exception raised in a predicate or callback propagates out of
  the read. `set_log_handler(None)` restores FFmpeg's default output.
- **Per-file logs.** Each `capture_*` result's `log()` holds what FFmpeg logged on the calling thread while the
  operation ran, at Info and above, so files audited on a thread pool keep their warnings apart. `LogCapture` itself
  is not bound: it must be destroyed on the thread that created it, which Python's garbage collector does not
  guarantee.
- **Borrowed buffers.** A `VideoReader` rereads its `Source` on every call, so it keeps the Python `Source` alive,
  and with it any buffer passed to `Source.from_memory`.
- **Fork safety.** Importing the module starts no threads and mutates no global state. `build_info()` and
  `capabilities()` are cached, pure computations. `hardware_device_usable()` is the exception: it opens a device and
  may load vendor drivers, so call it in the parent before forking DataLoader workers.

## Not bound yet

- The scaffolded operations (`resize`, `FilterGraph`, `animate_still`, `Pipeline::run`). Each gets bound when its
  C++ implementation lands.
- Batch entry points are deferred. Python callers parallelize across files themselves, which the GIL release above
  makes possible with threads, and use the `capture_*` variants to get a `FileError` per failed file instead of an
  exception.
