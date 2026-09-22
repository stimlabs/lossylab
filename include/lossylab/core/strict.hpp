#pragma once

#include "lossylab/core/json.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    /// What a stage does when it discovers it would have to convert pixel
    /// format or color properties that the caller did not ask for.
    ///
    /// `Refuse` is the default everywhere. It is the mechanism behind the
    /// library's central promise: nothing is converted silently, so two classes
    /// of data cannot pick up different histories by accident.
    enum class Strict
    {
        /// Throw ConversionRefused rather than convert.
        Refuse,

        /// Perform the conversion and append it to the stage record.
        AllowRecorded
    };

    std::string to_string(Strict mode);
    Strict strict_from_string(std::string_view name);

    /// Why a conversion happened. Distinguishing these matters when auditing:
    /// a conversion the caller requested is part of the experiment, one a
    /// codec forced is a property of the codec.
    enum class ConversionCause
    {
        /// The caller asked for it.
        Requested,

        /// An encoder or filter accepts only a subset of formats.
        CodecConstraint,

        /// A filter graph negotiated it between two links.
        GraphNegotiation
    };

    std::string to_string(ConversionCause cause);
    ConversionCause conversion_cause_from_string(std::string_view name);

    /// One conversion that actually took place, as recorded on a stage.
    struct ConversionEvent
    {
        /// "pix_fmt", "color_matrix", "color_range", "primaries", "transfer",
        /// "chroma_location", "bit_depth".
        std::string property;
        std::string from;
        std::string to;
        ConversionCause cause = ConversionCause::Requested;

        /// The scaler, filter or encoder that performed it.
        std::string performed_by;

        [[nodiscard]] json::Value to_json() const;
        static ConversionEvent from_json(const json::Value& value);
    };

    bool operator==(const ConversionEvent& left, const ConversionEvent& right) noexcept;
    inline bool operator!=(const ConversionEvent& left, const ConversionEvent& right) noexcept
    {
        return !(left == right);
    }

    using ConversionList = std::vector<ConversionEvent>;

    /// Applies the strict-mode policy to a conversion a stage is about to make.
    ///
    /// Under `Refuse` this throws ConversionRefused. Under `AllowRecorded` it
    /// appends the event to `into` and returns, leaving the caller to perform
    /// the conversion. Funnelling every implicit conversion through one
    /// function is what keeps the guarantee from depending on each stage
    /// remembering to check.
    void record_or_refuse(Strict mode,
                          ConversionList& into,
                          std::string property,
                          std::string from,
                          std::string to,
                          ConversionCause cause,
                          std::string performed_by,
                          std::string_view context);
}
