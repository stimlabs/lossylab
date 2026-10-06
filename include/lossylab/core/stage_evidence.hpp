#pragma once

#include "lossylab/codec/encode_types.hpp"
#include "lossylab/convert/convert_types.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/stage_kind.hpp"
#include "lossylab/io/decode_image_types.hpp"
#include "lossylab/io/video_reader_types.hpp"
#include "lossylab/measure/compression_history_types.hpp"
#include "lossylab/measure/measure_types.hpp"
#include "lossylab/transform/transform_types.hpp"

#include <variant>

namespace lossylab
{
    /// What one stage found out about its input, one type per stage kind.
    /// Replaying the stage should reproduce it, so it is also how a replay is
    /// checked.
    using StageEvidence =
        std::variant<DecodeImageEvidence, DecodeVideoEvidence, ConvertEvidence, ChromaRoundtripEvidence,
                     ReinterpretEvidence, EncodeImageEvidence, EncodeVideoEvidence, RoundtripImageEvidence,
                     RoundtripVideoEvidence, MeasureEvidence, CompareEvidence, RecompressionCurveEvidence,
                     CompressionHistoryEvidence, CropEvidence, OrientEvidence, AchromaticEvidence>;

    /// What one stage was told to do, resolved: the input to replaying it.
    /// Its alternatives follow StageEvidence's, kind for kind.
    using StageConfiguration =
        std::variant<DecodeImageOptions, DecodeVideoConfiguration, ConvertOptions, ChromaRoundtripOptions,
                     ReinterpretOptions, EncodeImageOptions, EncodeVideoOptions, RoundtripImageConfiguration,
                     RoundtripVideoConfiguration, MeasureOptions, CompareOptions, RecompressionOptions,
                     CompressionHistoryOptions, CropOptions, OrientOptions, AchromaticOptions>;

    template <StageKind Kind>
    struct StageKindIs
    {
        static constexpr StageKind value = Kind;
    };

    /// The stage kind an evidence or configuration type belongs to.
    template <typename T>
    struct StageKindOf;

    template <> struct StageKindOf<DecodeImageEvidence> : StageKindIs<StageKind::DecodeImage> {};
    template <> struct StageKindOf<DecodeVideoEvidence> : StageKindIs<StageKind::DecodeVideo> {};
    template <> struct StageKindOf<ConvertEvidence> : StageKindIs<StageKind::Convert> {};
    template <> struct StageKindOf<ChromaRoundtripEvidence> : StageKindIs<StageKind::ChromaRoundtrip> {};
    template <> struct StageKindOf<ReinterpretEvidence> : StageKindIs<StageKind::Reinterpret> {};
    template <> struct StageKindOf<EncodeImageEvidence> : StageKindIs<StageKind::EncodeImage> {};
    template <> struct StageKindOf<EncodeVideoEvidence> : StageKindIs<StageKind::EncodeVideo> {};
    template <> struct StageKindOf<RoundtripImageEvidence> : StageKindIs<StageKind::RoundtripImage> {};
    template <> struct StageKindOf<RoundtripVideoEvidence> : StageKindIs<StageKind::RoundtripVideo> {};
    template <> struct StageKindOf<MeasureEvidence> : StageKindIs<StageKind::Measure> {};
    template <> struct StageKindOf<CompareEvidence> : StageKindIs<StageKind::Compare> {};
    template <> struct StageKindOf<RecompressionCurveEvidence> : StageKindIs<StageKind::RecompressionCurve> {};
    template <> struct StageKindOf<CompressionHistoryEvidence> : StageKindIs<StageKind::CompressionHistory> {};
    template <> struct StageKindOf<CropEvidence> : StageKindIs<StageKind::Crop> {};
    template <> struct StageKindOf<OrientEvidence> : StageKindIs<StageKind::Orient> {};
    template <> struct StageKindOf<AchromaticEvidence> : StageKindIs<StageKind::Achromatic> {};

    template <> struct StageKindOf<DecodeImageOptions> : StageKindIs<StageKind::DecodeImage> {};
    template <> struct StageKindOf<DecodeVideoConfiguration> : StageKindIs<StageKind::DecodeVideo> {};
    template <> struct StageKindOf<ConvertOptions> : StageKindIs<StageKind::Convert> {};
    template <> struct StageKindOf<ChromaRoundtripOptions> : StageKindIs<StageKind::ChromaRoundtrip> {};
    template <> struct StageKindOf<ReinterpretOptions> : StageKindIs<StageKind::Reinterpret> {};
    template <> struct StageKindOf<EncodeImageOptions> : StageKindIs<StageKind::EncodeImage> {};
    template <> struct StageKindOf<EncodeVideoOptions> : StageKindIs<StageKind::EncodeVideo> {};
    template <> struct StageKindOf<RoundtripImageConfiguration> : StageKindIs<StageKind::RoundtripImage> {};
    template <> struct StageKindOf<RoundtripVideoConfiguration> : StageKindIs<StageKind::RoundtripVideo> {};
    template <> struct StageKindOf<MeasureOptions> : StageKindIs<StageKind::Measure> {};
    template <> struct StageKindOf<CompareOptions> : StageKindIs<StageKind::Compare> {};
    template <> struct StageKindOf<RecompressionOptions> : StageKindIs<StageKind::RecompressionCurve> {};
    template <> struct StageKindOf<CompressionHistoryOptions> : StageKindIs<StageKind::CompressionHistory> {};
    template <> struct StageKindOf<CropOptions> : StageKindIs<StageKind::Crop> {};
    template <> struct StageKindOf<OrientOptions> : StageKindIs<StageKind::Orient> {};
    template <> struct StageKindOf<AchromaticOptions> : StageKindIs<StageKind::Achromatic> {};

    [[nodiscard]] StageKind stage_kind(const StageEvidence& evidence) noexcept;
    [[nodiscard]] StageKind stage_kind(const StageConfiguration& configuration) noexcept;

    /// The evidence or configuration as a JSON object of its fields; the kind
    /// is not part of it.
    [[nodiscard]] json::Value evidence_to_json(const StageEvidence& evidence);
    [[nodiscard]] json::Value configuration_to_json(const StageConfiguration& configuration);

    /// Reads what the functions above wrote, as the type of `kind`. Throws
    /// ConfigError for a kind that has no type yet (resize, filter,
    /// animate_still).
    [[nodiscard]] StageEvidence evidence_from_json(StageKind kind, const json::Value& value);
    [[nodiscard]] StageConfiguration configuration_from_json(StageKind kind, const json::Value& value);
}
