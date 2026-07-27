# Animation MCP automation

The versioned `animation` provider exposes bounded asset, controller,
advanced-authoring, preview/playback, runtime-parameter, residency, profiler,
diagnostic, capture, and durable-job operations. Clients should discover
capabilities first; descriptors carry schemas, permissions, approval policy, and
worker/protected visibility declarations.

Mutations require an `idempotencyKey`, `expectedGeneration`, and normally a
`correlationId`. Validation happens before publication. Stale, invalid, denied,
or unapproved requests leave authored state unchanged; a retry returns the
retained receipt. `dryRun` validates without advancing the generation.

Long imports/reimports/recording/validation work return a durable `jobId`. Poll
`animation.job.get` and cancel through `animation.job.cancel`. Retention is
bounded for the editor session, so expired IDs produce a structured error.

Recording sessions use the `animation.recording.finalize` job capability. The
editor captures at most one immutable animator snapshot per protected boundary,
copies its selected channels into bounded recorder chunks, and mirrors progress,
diagnostics, cancellation, and the final candidate/publication result into that
job. A record job cannot mutate a live animator or source clip; final output is
published only after the checked `.teanim` candidate validates and an atomic
same-volume replacement succeeds.

Start a session with `animation.recording.start`; it returns a durable job ID
whose progress/result is read through `animation.job.get` and whose capture is
cancelled through `animation.job.cancel`. Start requires a live animator target,
the expected published generation, and an explicit copied `jointStableIds`
array. The runtime pose exposes dense joint indices only, so the endpoint fails
closed if a client omits the stable-ID binding or it no longer matches the
published pose. `destinationPath` is relative to the open project's Assets root
and must end in `.teanim`; callers also declare a positive recording segment
duration, preventing an unbounded or ambiguous clip-cooking request.

Runtime diagnostics are immutable snapshots. `animation.runtime.residency` and
`animation.runtime.profiler` read only those copied snapshots; even an
open-but-collapsed profiler request cannot enable detailed collection. Scene,
Game, camera, and custom-camera capture requires a renderer and reports
`Unavailable` rather than stale pixels. The provider never exposes
project/application lifecycle authority.

Run the `Editor.Animation` test filter for functional checks; installed-editor
MCP validation supplies the separate real transport/UI evidence.
