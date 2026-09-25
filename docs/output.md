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
- **Configuration**: the input to replay, i.e. what each stage was told to do (`Strict` mode, kernels, thresholds, encoder options), as resolved after defaults and randomization, never as the caller passed it. In an audit it is the same for every file; in the degradation library it differs per sample.
- `StageRecord`: one operation. Its `params` are **evidence**: what the stage observed or derived from this input (`measured_as`, `analyzed_as`, achieved bits per pixel), together with its formats, conversions, measurements and timing. Replaying a stage should reproduce its evidence, so evidence is also how a replay is checked.

Every operation returns its `record` and its `configuration` side by side. `ProcessingRecord.append(record, configuration)` stores both, and `configurations()[i]` belongs to `stages()[i]`.

## Use

1. Create the `Source`.
2. Run the operations you need: decode (which probes), measure, compression_history.
3. Append each stage with its configuration to one `ProcessingRecord` and store its `to_dict()`.

## Replaying

Replay starts from the same input bytes, so a source is identified by a hash of its bytes, never by its path.

To reproduce stage `i`, apply `configurations()[i]` to the state left by the earlier stages, starting from `origin`. A stage's `input` is only a format description, so the earlier stages that changed the samples have to be replayed.

`StageRecord.modifies_state` says which stages those are. `True` (decode, convert, resize, encode): the output is the next stage's input. `False` (measure, compare, compression_history): the stage only analyzed, and any conversion in its `conversions` was applied to a copy that was discarded.

## What reproducible means

A replay is expected to give **identical evidence**, not identical bytes. Byte-exact output is out of reach: it depends on FFmpeg, the libraries under it, the hardware and the thread count. Evidence that is a float is compared with a tolerance.

When two records of the same input disagree, the record must say where to look:

- **Build identity** is what can change results: the lossylab code (git commit, whether the tree was modified, compiler, build type) and the FFmpeg it runs on (version, configure line, library versions). The record embeds it whole, together with `identity_hash`, a SHA-256 over all of it. Equal hashes mean the same build. Different hashes mean `build_diff()` of the two identities lists exactly what differs.
- **Diagnostics** are facts about the machine (architecture, OS, CPU features). They are recorded so they can be blamed, and are not part of the identity: results are expected to match across machines.

## Rules

- JSON field names are the C++ member names, never renamed.
- Every record states its `schema_version`. A change to the layout bumps it.
- Values are the resolved ones, never as the caller passed them.
