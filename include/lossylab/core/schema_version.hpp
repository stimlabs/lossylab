#pragma once

namespace lossylab
{
    /// Version of the JSON field layout this library's `to_json()` methods
    /// produce. Increment it whenever a field is added, renamed, removed, or
    /// changes meaning, in a way a stored-output consumer would need to know
    /// about.
    ///
    /// Every output type that writes it must load documents of the versions it still reads: it owns a
    /// `SchemaMigrations` (`core/schema_migration.hpp`) with one step per version below this one, and its
    /// `from_json()` starts with `upgrade()`. Bumping this constant without adding that step to each such type
    /// makes the type's migrations throw the next time it loads. Today these are ProcessingRecord, ProbeResult and
    /// FileError; HeaderInfo, MeasureResult, CompareResult, RecompressionCurve and CompressionHistory write the
    /// version but have no loader yet, and take the same interface when they get one.
    ///
    /// Independent of `BuildInfo::identity_hash` (see `env/build_info.hpp`),
    /// which identifies the code and the FFmpeg build that produced a result
    /// and changes with either; this constant identifies the shape of this
    /// library's own output and changes only when that shape does.
    constexpr int schema_version = 16;
}
