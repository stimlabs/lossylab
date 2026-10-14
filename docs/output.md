# Output philosophy

## Why records exist

Processing records are optional, and serve one purpose: **reproducibility and replayability**. A record describes, deterministically, the stages of processing applied to an input source. Two use cases rely on it:

1. **Dataset audit.** Files are analyzed, and the results (measurements, metrics, scores) are stored. The record also says how each result was obtained, so it can always be reproduced.
2. **Degradation library.** Transformations (resizing, chroma handling, compression, filtering, ...) are applied to an input to produce a derived copy or view of it. The record lets the same transformations be replayed to get the same derived copy again, whatever the reason for making it: augmentation, simulating a capture or platform chain, or building an equalized view.

### Example: equalizing a dataset

The equalized view is a specific use of the degradation library, and it combines both use cases. The audit observes how files, their sources and their processing differ. A common denominator, or target compression distribution, is then decided that every source subset must adhere to. Finally the library is used to record a specific set of processing for each unique sample (each sample may need a different one) that brings it to that target. Replaying a sample's record regenerates its equalized view exactly.

## One source, one record

A `Source` goes in. Everything lossylab does with it is collected in **one `ProcessingRecord`**, which is what you store. The name stays even when the file is only analyzed: decode, measure and compression_history change nothing about the file, but they are still processing.

**Every record is self-contained.** Options repeat from record to record when they are the same (in an audit, for every file). That is a storage concern, not the library's: a record that needs another file to be understood cannot be replayed alone. Handle the repetition when storing, never by leaving something out of a record.

## Parts

- `ProcessingRecord`: the history of one source: the build identity and diagnostics, the schema version, the probe of the file as its origin, the stages, and one configuration per stage.
- **Configuration**: the input to replay, i.e. what each stage was told to do, as resolved after defaults and randomization, never as the caller passed it. It is the stage's own options struct (`MeasureOptions`, `EncodeImageOptions`, ...), with every option in it; a stage whose calls take input beyond its options wraps the two (the video reader's options and the frames it picked, a round trip's encode and decode options). Anything that is not an option, such as a threshold or a filter's arguments, is fixed by the build and not repeated here. In an audit it is the same for every file; in the degradation library it differs per sample.
- `StageRecord`: one operation. Its `evidence` is what the stage found out about this input: what it measured and detected (`measured_as`, the measurements themselves, JPEG tables, traces), and what it derived (`analyzed_as`, the source's hash, an encoder's resolved settings and achieved bits per pixel). Alongside it are the stage's formats, conversions, block grid, per-frame statistics and timing. Replaying a stage should reproduce its evidence, so evidence is also how a replay is checked.

Evidence and configuration have one type per kind of stage, and the type of the evidence is the stage's `kind`. In Python, `stage.evidence` is that type, so `isinstance(stage.evidence, lossylab.MeasureEvidence)` and `stage.kind == lossylab.StageKind.Measure` say the same.

Every operation returns its `record` and its `configuration` side by side, and a result's own fields (`result.evidence`, `result.search`) read the record's evidence. `ProcessingRecord.append(record, configuration)` stores both, refuses a configuration of another kind of stage, and `configurations()[i]` belongs to `stages()[i]`.

## Use

1. Create the `Source`.
2. Run the operations you need: decode (which probes), measure, compression_history.
3. Append each stage with its configuration to one `ProcessingRecord` and store its `to_dict()`.

## Replaying

Replay starts from the same input bytes, so a source is identified by a hash of its bytes, never by its path.

To reproduce stage `i`, apply `configurations()[i]` to the state left by the earlier stages, starting from `origin`. A stage's `input` is only a format description, so the earlier stages that changed the samples have to be replayed.

For a record a `Pipeline` made, `PipelineSpec.from_record(record)` is that replay as a spec: run it on the same file, and the output's `samples_sha256()` equals the record's `output_sha256` on the same build. See [pipeline.md](pipeline.md).

`StageRecord.modifies_state` says which stages those are. `True` (decode, convert, crop, orient, achromatic, resize, encode): the output is the next stage's input. `False` (measure, compare, compression_history): the stage only analyzed, and any conversion in its `conversions` was applied to a copy that was discarded.

## What reproducible means

A replay is expected to give **identical evidence**, not identical bytes. Byte-exact output is out of reach: it depends on FFmpeg, the libraries under it, the hardware and the thread count. Evidence that is a float is compared with a tolerance.

When two records of the same input disagree, the record must say where to look:

- **Build identity** is what can change results: the lossylab code (git commit, whether the tree was modified, compiler, build type, and the lcms2 and zlib it links) and the FFmpeg it runs on (version, configure line, library versions). The record embeds it whole, together with `identity_hash`, a SHA-256 over all of it. Equal hashes mean the same build. Different hashes mean `build_diff()` of the two identities lists exactly what differs.
- **Diagnostics** are facts about the machine (architecture, OS, CPU features). They are recorded so they can be blamed, and are not part of the identity: results are expected to match across machines.

## Rules

- JSON field names are the C++ member names, never renamed. A stage's `"kind"` is the one computed field: it is the type of its evidence.
- Every record states its `schema_version`. A change to the layout, or to what a field means, bumps it. An output that can be loaded (`ProcessingRecord`, `ProbeResult`, `FileError`) upgrades a stored document from any schema from 14 on to the current one when it loads, so `to_dict()` always writes the current schema with data that follows it. A document from a newer build, from before 14, or one whose data a later layout cannot express raises `SchemaVersionError`, a `ConfigError` that names the type, the version and why. A new output that writes `schema_version` declares one migration step per older version in a `SchemaMigrations` (`core/schema_migration.hpp`) and starts its loader with `upgrade()`; a step that has nothing faithful to carry over throws `MigrationRefused`.

What the steps do:

- **14 → 15:** a decode's target pixel format and color became a full `conversion` (a half-set target takes its other half from the stage's recorded output; one the output cannot complete is refused), taking the swscale defaults of schema 14: area kernel down, bilinear up, no ICC handling, alpha dropped. Image encodes gained `chroma_down` (area), conversions gained `icc` and `alpha`, conversion evidence gained `color_transform`, `icc_profile_sha256` and `alpha`, and the record gained a `seed` and an `output_sha256`, both null for a record no pipeline made. The migrated configuration describes what ran; it does not promise that replaying it on this build gives the same pixels, which the build identity already says.
- **15 → 16:** a JPEG encode's block grid became the minimum coded unit (`jpeg_mcu`, 16×16 for 4:2:0), where 15 recorded the 8×8 transform block, and the evidence of a crop that followed such a stage is redone against it. A crop's `whole_blocks` also became true only for a crop that starts on a block corner of a valid grid, where 15 checked its width and height alone; every crop's value is recomputed from its stored phases.
- Values are the resolved ones, never as the caller passed them.
