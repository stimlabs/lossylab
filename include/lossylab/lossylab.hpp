#pragma once

/// lossylab: model, apply and inspect the processing history of images and
/// video frames.
///
/// The umbrella header. Including a specific header is cheaper and states an
/// intent, so prefer that in the library's own sources; this exists for callers
/// who want the whole surface, and as a map of it.
///
/// Three things hold across every operation here:
///
///  - Nothing is converted silently. Each stage declares the pixel format and
///    color specification it produces, and anything converted is recorded.
///  - Every operation returns a ProcessingRecord entry describing what it did,
///    including the coordinate transform from input to output and the block
///    grid a compression stage imposed.
///  - The API covers the full design surface regardless of what the linked
///    FFmpeg provides. `capabilities()` says which parts this build can serve;
///    asking for something absent raises UnsupportedCapability rather than
///    substituting something similar.

// Core types and the record machinery.
#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/kernel.hpp"
#include "lossylab/core/pipeline_spec.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/rng.hpp"
#include "lossylab/core/strict.hpp"

// What this build can do.
#include "lossylab/env/build_info.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/env/log.hpp"

// Probing and decoding.
#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/file_result.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/io/read_headers.hpp"
#include "lossylab/io/source.hpp"
#include "lossylab/io/video_reader.hpp"

// Transformation.
#include "lossylab/convert/convert.hpp"
#include "lossylab/filter/filter_graph.hpp"
#include "lossylab/motion/animate_still.hpp"
#include "lossylab/resample/resize.hpp"

// Compression.
#include "lossylab/codec/encode.hpp"

// Chains and measurement.
#include "lossylab/measure/compression_history.hpp"
#include "lossylab/measure/measure.hpp"
#include "lossylab/pipeline/pipeline.hpp"
