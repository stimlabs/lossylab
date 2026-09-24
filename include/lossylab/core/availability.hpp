#pragma once

#include <string>
#include <string_view>

namespace lossylab
{
    /// Why an optional piece of per-frame or per-file data is absent, when it
    /// is absent.
    enum class Availability
    {
        /// Looked for and genuinely absent from this file or frame, or not
        /// requested for this call.
        NotPresent,

        /// The linked FFmpeg build, or the decoder it selected for this file,
        /// does not produce this data at all. For example, exporting a
        /// per-block quantization parameter (QP) map is a per-decoder
        /// capability: some decoders can report it, others cannot regardless
        /// of what the file contains.
        NotSupportedByBuild,

        /// Present and valid.
        Present
    };

    std::string to_string(Availability availability);
    Availability availability_from_string(std::string_view name);

    /// For reflect::from_json().
    inline void from_string(const std::string_view name, Availability& value)
    {
        value = availability_from_string(name);
    }
}
