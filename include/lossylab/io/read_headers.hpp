#pragma once

#include "lossylab/core/availability.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/io/source.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lossylab
{
    /// A parameter set parsed out of the bitstream.
    struct ParameterSet
    {
        /// "sps", "pps", "vps", "sequence_header".
        std::string kind;
        int id = 0;

        /// Fields the parser recovered, e.g. profile_idc, level_idc,
        /// chroma_format_idc, bit_depth, VUI color tags.
        json::Value fields;

        [[nodiscard]] json::Value to_json() const;
    };

    /// Per-slice quantization, as coded.
    struct SliceInfo
    {
        int index = 0;

        /// "I", "P", "B", or H.264's "SP" and "SI".
        std::string slice_type;

        /// The quantizer the slice header carries, before any per-block delta:
        /// the QP for H.264 and HEVC, the quantiser_scale_code for MPEG-2.
        std::optional<int> qp;

        /// Coded size, when the slice is the only one in its packet.
        std::optional<std::int64_t> size_bytes;

        [[nodiscard]] json::Value to_json() const;
    };

    /// What `read_headers` recovered.
    ///
    /// The strongest audit evidence available for video and for AVIF, because
    /// it identifies the encoder and the compression strength without decoding
    /// a single pixel. x264 and x265 both write their entire option string into
    /// the stream, which pins down not just the encoder but its exact
    /// configuration.
    struct HeaderInfo
    {
        std::string codec_name;

        std::vector<ParameterSet> parameter_sets;
        std::vector<SliceInfo> slices;

        /// The encoder's own settings string, from an SEI user-data unit
        /// (x264, x265) or an equivalent. Present far more often than people
        /// expect, and close to conclusive when it is.
        std::optional<std::string> embedded_encoder_settings;

        /// States why `embedded_encoder_settings` is empty when it is: this
        /// codec has no such mechanism or this stream did not use it
        /// (`NotPresent`), or the linked FFmpeg build's bitstream parser for
        /// this codec cannot read this data (`NotSupportedByBuild`). Without
        /// this, a build that cannot parse SEI user data for a given codec
        /// would look identical to a stream that genuinely carries none.
        Availability embedded_encoder_settings_availability = Availability::NotPresent;

        /// Parsed out of the settings string above, when it could be parsed.
        std::map<std::string, std::string> encoder_settings;

        /// AV1 and VP9 carry a quantizer index rather than a QP: one entry
        /// per coded frame, in bitstream order.
        std::vector<int> quantizer_indices;

        /// Color as the bitstream declares it, as a ColorSpec's JSON, which can
        /// disagree with what the container tags. A disagreement is itself a
        /// strong signal that the file was remuxed or retagged. Null when the
        /// bitstream declares no color.
        json::Value bitstream_color;

        [[nodiscard]] json::Value to_json() const;
    };

    struct ReadHeadersOptions
    {
        /// Stop after this many slices, or for VP9 and AV1 coded frames.
        /// Header parsing is cheap, but a long clip has a great many slices and
        /// an audit rarely needs them all. The first packet is always read, so
        /// 0 still finds the parameter sets and the embedded settings.
        int max_slices = 256;

        /// Which stream to parse. Negative means the first video stream.
        int stream_index = -1;
    };

    /// Parses bitstream syntax without decoding.
    ///
    /// Reads FFmpeg's trace_headers bitstream filter, which logs every syntax
    /// element its parser reads, through the library's log callback; those
    /// lines never reach a handler set with `set_log_handler`. Interprets
    /// H.264, HEVC, MPEG-2, VP9 and AV1, and throws UnsupportedCapability for
    /// any other codec.
    [[nodiscard]] HeaderInfo read_headers(const Source& source,
                                          const ReadHeadersOptions& options = {});
}
