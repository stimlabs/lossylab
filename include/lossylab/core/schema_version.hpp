#pragma once

namespace lossylab
{
    /// Version of the JSON field layout this library's `to_json()` methods
    /// produce. Increment it whenever a field is added, renamed, removed, or
    /// changes meaning, in a way a stored-output consumer would need to know
    /// about.
    ///
    /// Independent of `BuildInfo::build_id` (see `env/build_info.hpp`), which
    /// identifies the linked FFmpeg build and changes with it; this constant
    /// identifies the shape of this library's own output and changes only
    /// with a release of this library.
    constexpr int schema_version = 6;
}
