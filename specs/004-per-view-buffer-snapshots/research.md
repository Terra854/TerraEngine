# Research: Per-View Buffer Snapshots

## Decision: Use per-output/per-pass snapshot slots

Allocate one snapshot slot for each active editor view, runtime camera output, or render pass that owns camera-dependent data in a frame. The slot owns or references the lighting payload, frame culling payload, clustered-lighting buffers, indirect draw commands, diagnostic counters, and command recording metadata needed by that output.

**Rationale**: The current corruption risk comes from multiple outputs sharing `currentFrame` GPU uploads while command buffers are submitted later. Per-output slots preserve correctness without forcing one view to finish before the next view can be prepared.

**Alternatives considered**:

- Continue queue idling between editor views: correct but keeps the performance bottleneck and does not scale to future multi-camera outputs.
- Duplicate the entire renderer per view: isolates state but wastes memory, duplicates caches, and makes shared resources harder to manage.
- Only split light buffers: insufficient because culling, cluster bins, shadow assignment metadata, indirect draws, and diagnostics can also be view-dependent.

## Decision: Treat camera outputs as the future-facing owner abstraction

Model Scene view, Game view, runtime main camera, and future offscreen targets as camera outputs with an owner, target kind, target size, matrices, and priority.

**Rationale**: Render-to-texture features such as mirrors, minimaps, portals, and camera feeds need the same isolation as editor views. Planning around camera outputs avoids a narrow Scene/Game-only API that would need another redesign.

**Alternatives considered**:

- Keep a fixed `Game`/`Scene` enum as the only owner: simple for the current bug, but not enough for arbitrary future camera targets.
- Add a separate RenderTexture path later: likely to duplicate snapshot, diagnostics, and fallback behavior.

## Decision: Use bounded snapshot budgets with explicit fallback reporting

Reserve a bounded number of active snapshot slots per frame and report pressure. If a required output cannot receive an isolated slot, use a correctness-preserving fallback and emit diagnostics. Optional future outputs may be skipped if the caller marks them optional.

**Rationale**: Snapshot storage must be predictable for editor and runtime use. Correct rendering is mandatory, but failures should be visible instead of silently reintroducing corruption or broad serialization.

**Alternatives considered**:

- Allocate unbounded snapshots on demand: easier to integrate but risks unpredictable memory growth.
- Hard-fail when capacity is exceeded: clear but too brittle for editor workflows and validation.

## Decision: Isolate shadow assignment metadata while sharing reusable shadow images where safe

Keep per-output shadow selection, resource indices, region metadata, and cache signatures in the snapshot. Long-lived shadow images may remain shared if the snapshot records exactly which assignments and sampled resources are valid for that output.

**Rationale**: Shadow images are expensive, but the observed corruption involves a view sampling another view's shadow payload. Isolating assignment metadata addresses the correctness issue without requiring every view to duplicate all shadow images.

**Alternatives considered**:

- Duplicate all shadow images per output: strongest isolation but too expensive and unnecessary for the first milestone.
- Keep current global shadow assignment state: preserves the risk that later outputs overwrite earlier sampled metadata.

## Decision: Keep diagnostics per snapshot and aggregate by view/output

Record active snapshot count, owner, target kind, resource pressure, fallback synchronization events, and per-output lighting/shadow/culling diagnostics. Existing Game/Scene diagnostics remain available, while future output diagnostics can use the same snapshot ownership records.

**Rationale**: The feature is both correctness-sensitive and performance-sensitive. Developers need evidence that snapshots are isolated and that broad synchronization is not silently returning.

**Alternatives considered**:

- Only rely on frame captures: useful but too manual for regression testing.
- Only expose a global snapshot count: insufficient to diagnose which view, pass, or camera target consumed capacity.

## Decision: Validate with dual-view and synthetic offscreen camera-output coverage

Add validation that renders Scene/Game-style different cameras and at least one future-facing offscreen camera output. The offscreen target can be developer-facing validation infrastructure before a user-facing RenderTexture feature exists.

**Rationale**: The immediate bug must stay fixed, and the requested future multi-camera provision needs concrete coverage to prevent accidental API narrowing during implementation.

**Alternatives considered**:

- Test only editor Scene/Game views: sufficient for the current bug but not for future RenderTexture provisioning.
- Wait for full RenderTexture feature before testing: delays detection of snapshot model limitations.
