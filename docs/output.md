# Output philosophy

## One source, one record

A `Source` goes in. Everything lossylab does with it is collected in **one `ProcessingRecord`**, which is what you store. The name stays even when the file is only analyzed: decode, measure and compression_history change nothing about the file, but they are still processing.

## Parts

- `ProcessingRecord`: the history of one source. It carries the build id, the schema version, the probe of the file as its origin, the stages, and one configuration per stage.
- `StageRecord`: one operation. It holds only **per-file evidence**: what the stage resolved from this file (`params`, e.g. `measured_as`, `analyzed_as`), its formats, conversions, measurements and timing.
- **Configuration**: the options a stage ran with (`Strict` mode, kernels, thresholds, encoder options). It is the same for every file of a run, so it never sits in a `StageRecord`. A run with another build, other external dependencies or another schema version is a new `ProcessingRecord`.

Every operation returns its `record` and its `configuration` side by side. `ProcessingRecord.append(record, configuration)` stores both, and `configurations()[i]` belongs to `stages()[i]`.

## Use

1. Create the `Source`.
2. Run the operations you need: decode (which probes), measure, compression_history.
3. Append each stage with its configuration to one `ProcessingRecord` and store its `to_dict()`.

## Replaying

To reproduce stage `i`, apply `configurations()[i]` plus the stage's `params` to the state left by the earlier stages, starting from `origin`. A stage's `input` is only a format description, so the earlier stages that changed the samples have to be replayed.

`StageRecord.modifies_state` says which stages those are. `True` (decode, convert, resize, encode): the output is the next stage's input. `False` (measure, compare, compression_history): the stage only analyzed, and any conversion in its `conversions` was applied to a copy that was discarded.

## Rules

- JSON field names are the C++ member names, never renamed.
- Every record states its `schema_version`. A change to the layout bumps it.
- Values are the resolved ones, never as the caller passed them.
