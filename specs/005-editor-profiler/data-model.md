# Data Model: Editor Profiler

## ProfilingSession

Represents one live or reopened profiling capture.

**Fields**:

- `sessionId`: stable identifier generated when recording starts or a capture is loaded.
- `state`: `Idle`, `Recording`, `Paused`, `Finalizing`, `Saving`, `OpeningSavedCapture`, `Loaded`, `Error`.
- `projectRoot`: canonical path of the opened project that owns this session.
- `profilerDirectory`: canonical `<OpenedProjectRoot>/Profiler/` path.
- `captureSettings`: active capture limits and enabled modules.
- `metadata`: project/session information saved with a capture.
- `frames`: ordered frame records available for inspection.
- `selectedFrameIndex`: optional selected frame index pinned for UI inspection.
- `pendingSavedArtifact`: optional artifact path created when a recording is finalized.
- `lastError`: optional user-facing failure summary.

**Validation rules**:

- `projectRoot` must exist and must match the currently opened project before save/load.
- `profilerDirectory` must be inside `projectRoot`.
- `frames` must stay within configured count or duration limits unless a saved capture is loaded for read-only inspection.

**State transitions**:

- `Idle -> Recording` when the user starts capture.
- `Recording -> Paused` when the user pauses capture.
- `Paused -> Recording` when the user resumes capture.
- `Recording|Paused -> Finalizing -> Saving -> OpeningSavedCapture -> Loaded` when the user ends a capture and the automatic save/open workflow succeeds.
- `Idle|Paused|Loaded -> OpeningSavedCapture -> Loaded` when a saved capture from the in-editor list opens.
- Any active state may enter `Error` after a failed operation while preserving editor stability.

## CaptureSettings

Controls what a session records and how large it can grow.

**Fields**:

- `enabledModules`: CPU/engine and task execution toggles.
- `maxFrames`: maximum live frame count retained in memory.
- `maxDurationSeconds`: optional time-window cap for live capture.
- `captureMode`: `LiveRolling`, `FinalizingToArtifact`, or `LoadedReadOnly`.
- `recordMarkers`: whether named sample ranges are recorded.
- `recordTaskContext`: whether task/thread context is attached to samples.

**Validation rules**:

- At least one module must be enabled while recording.
- `maxFrames` must be greater than zero and should default to a bounded value.
- Completed live captures transition to read-only loaded captures after automatic save/open succeeds.

## FrameRecord

Represents one captured engine frame.

**Fields**:

- `frameIndex`: monotonically increasing index within the session.
- `frameStartTime`: timestamp relative to session start.
- `durationMs`: total frame duration.
- `complete`: whether recording covered the full frame.
- `moduleSummaries`: one summary per profiler module.
- `sampleRoots`: top-level instrumentation samples for this frame.
- `threadTracks`: optional grouping of samples by main thread and worker/task context.

**Validation rules**:

- `durationMs` must be non-negative.
- Partial frames must be marked when capture starts or stops mid-frame.
- A selected frame must remain inspectable even if the rolling live buffer advances.

## ProfilerModule

Describes a user-facing performance area.

**Fields**:

- `moduleId`: `CpuEngine` or `TaskExecution`.
- `displayName`: label shown in the profiler window.
- `available`: whether data exists for the selected frame.
- `summaryMetrics`: scan-friendly metrics for graphs and tables.
- `detailRows`: selected-frame rows sorted by cost or relevance.

**Validation rules**:

- Missing module data must be reported as unavailable rather than zero cost.
- Detail rows should place highest-cost contributors first where cost is meaningful.

## InstrumentationSample

Represents a named operation captured during a frame.

**Fields**:

- `sampleId`: stable per-frame identifier.
- `name`: human-readable marker name.
- `moduleId`: owning profiler module.
- `startTimeMs`: start time relative to frame start.
- `durationMs`: elapsed sample duration.
- `depth`: nesting depth.
- `parentSampleId`: optional parent sample.
- `threadId`: optional main/worker thread identity.
- `taskLabel`: optional task context for worker execution.
- `occurrenceCount`: aggregate count when repeated samples are grouped.

**Validation rules**:

- `durationMs` must be non-negative.
- Child sample time must fit inside the parent sample when both are complete.
- Repeated samples may aggregate for summaries, but individual occurrences must remain inspectable for selected-frame detail.

## CaptureArtifact

Represents a saved capture file under the opened project's `Profiler/` folder.

**Fields**:

- `fileName`: capture file name displayed in the in-editor list.
- `absolutePath`: canonical path under `<OpenedProjectRoot>/Profiler/`.
- `metadata`: readable session summary block.
- `payloadVersion`: capture format version.
- `createdAt`: capture save time.
- `frameCount`: number of saved frames.
- `moduleIds`: modules included in the artifact.

**Validation rules**:

- `absolutePath` must be inside the currently opened project root.
- Captures outside `<OpenedProjectRoot>/Profiler/` must not appear in the selectable list.
- Incompatible or corrupted captures must produce a clear error and must not replace the current session.

## CaptureMetadata

Readable information saved with each capture.

**Fields**:

- `terraEngineVersion`: version or build identifier available at capture time.
- `projectName`: opened project name.
- `projectRootHashOrPathHint`: non-authoritative hint for diagnostics.
- `scenePath`: active scene path when known.
- `captureSettings`: settings used for the capture.
- `startedAt`: session start time.
- `savedAt`: artifact save time.

**Validation rules**:

- Metadata must be sufficient to tell whether the capture belongs to the opened project.
- Metadata is advisory; filesystem path containment remains the source of truth for project-boundary enforcement.
