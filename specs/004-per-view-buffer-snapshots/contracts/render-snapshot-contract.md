# Contract: Render Snapshot Ownership

This contract describes the renderer-facing behavior required by the per-view/per-pass snapshot feature. It is an internal engine contract, not a public web or process API.

## Render Output Request

A render output request represents one view, camera, pass, or future offscreen target that needs isolated camera-dependent data.

**Required inputs**:

- Output identity and kind.
- Required or optional priority.
- Target size and aspect ratio.
- Camera view matrix, projection matrix, and world position.
- Render settings and quality preset.
- Visible draw commands for the output.
- Per-light shadow draw lists for the output.
- Light candidates available to the output.
- UI batch if the output consumes UI rendering.

**Behavioral guarantees**:

- Required outputs receive an isolated snapshot or a reported correctness-preserving fallback.
- Optional outputs may be skipped when capacity is exhausted, but the skip is reported.
- Outputs with different target sizes receive distinct culling and cluster payloads.

## Snapshot Acquisition

The renderer acquires snapshot storage before uploading view-dependent data.

**Preconditions**:

- Frame work has begun.
- The output request has valid owner identity and target dimensions.
- Snapshot budget is known for the active frame.

**Postconditions**:

- The returned snapshot identifies the owning output.
- Lighting, culling, cluster, indirect draw, shadow assignment, and diagnostics storage cannot be overwritten by later outputs until released.
- Failure returns a fallback reason rather than silently sharing unsafe state.

## Command Recording

Command recording binds or references snapshot-owned data.

**Preconditions**:

- Snapshot state is `Prepared`.
- Required payload ranges and shadow assignment metadata are valid.

**Postconditions**:

- Recorded commands do not depend on mutable frame-global uploads that later outputs can overwrite.
- Diagnostics can map recorded work back to the snapshot owner.
- Shadow sampling uses the assignment metadata captured for the same snapshot.

## Snapshot Release

Snapshot storage is released after all GPU work that can read it has completed.

**Preconditions**:

- Work associated with the snapshot has completed, or the snapshot entered a safe fallback state that did not record commands using snapshot-owned data.

**Postconditions**:

- Snapshot storage is available for reuse by a later frame.
- Diagnostics retain a copy of the latest owner/status summary for inspection.

## Diagnostics Contract

Renderer diagnostics expose snapshot behavior without requiring frame-capture tools.

**Required fields**:

- Active snapshot count.
- Peak snapshot count for the latest frame.
- Owner summaries with output kind and target size.
- Fallback events with owner, reason, and result.
- Isolation status for lighting, clustering, shadow assignments, and indirect draw data.

**Acceptance rules**:

- Normal warmed-up dual-view editor rendering reports no broad fallback synchronization in at least 99% of frames.
- Dual-view validation reports separate Scene and Game owners.
- Future multi-camera validation reports at least two camera outputs with independent snapshot ownership.
