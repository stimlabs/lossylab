#pragma once

namespace lossylab
{
    /// Version of the JSON field layout this library's `to_json()` methods
    /// produce. Increment it whenever a field is added, renamed, removed, or
    /// changes meaning, in a way a stored-output consumer would need to know
    /// about.
    ///
    /// Independent of `BuildInfo::identity_hash` (see `env/build_info.hpp`),
    /// which identifies the code and the FFmpeg build that produced a result
    /// and changes with either; this constant identifies the shape of this
    /// library's own output and changes only when that shape does.
    constexpr int schema_version = 12;
}
