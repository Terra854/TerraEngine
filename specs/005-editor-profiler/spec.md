# Feature Specification: Editor Profiler

**Feature Branch**: `005-editor-profiler`  
**Created**: 2026-04-25  
**Status**: Draft  
**Input**: User description: "next major feature, a profiler, something like https://docs.unity3d.com/6000.4/Documentation/Manual/profiler-introduction.html. Editor only"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Inspect Editor Performance During a Session (Priority: P1)

As an engine developer using the TerraEngine Editor, I want to open an editor-only profiling tool while running or previewing a scene so that I can see where engine frame time is being spent and identify expensive rendering, scripting, simulation, and engine-system work.

**Why this priority**: This is the minimum useful profiler experience. Without a clear live view of recorded performance areas, users cannot diagnose slow editor sessions or validate optimization work.

**Independent Test**: Can be fully tested by opening the editor profiler during an active editor session, recording performance for several frames, and confirming that frame timing and categorized activity are visible without using any standalone runtime build.

**Acceptance Scenarios**:

1. **Given** the editor is open with a scene loaded, **When** the user opens the profiler and starts recording, **Then** the profiler displays a live stream of captured frames with total frame time and categorized timing data.
2. **Given** the profiler has recorded multiple frames, **When** the user selects a frame, **Then** the profiler shows a detailed breakdown for that frame and preserves the selected frame while new frames continue to record.
3. **Given** recording is active, **When** the user pauses recording, **Then** the profiler stops adding new frames while keeping the captured data available for inspection.

---

### User Story 2 - Compare Performance Across Categories (Priority: P2)

As an engine developer, I want separate profiler modules for major execution areas so that I can quickly narrow a slowdown to main-thread engine CPU work or thread-pool task execution.

**Why this priority**: A profiler that only shows total frame time is too coarse for practical optimization. Category-level views make the data actionable and align with expected profiler workflows.

**Independent Test**: Can be fully tested by recording a session that exercises scene simulation, rendering, and background tasks, then confirming main-thread work appears under CPU/Engine while thread-pool work appears under Task Execution.

**Acceptance Scenarios**:

1. **Given** a recorded session contains activity in multiple performance areas, **When** the user switches profiler modules, **Then** each module shows metrics and detailed records relevant to that area.
2. **Given** a selected frame contains an unusually expensive category, **When** the user opens that category's details, **Then** the profiler highlights the most expensive contributors first.
3. **Given** a category has no data for a selected frame, **When** the user views that module, **Then** the profiler clearly indicates that no samples were recorded instead of showing misleading empty values.

---

### User Story 3 - Use Instrumentation Markers to Find Hotspots (Priority: P3)

As an engine developer, I want named timing markers and nested sample ranges to appear in the profiler so that I can connect visible performance costs to meaningful engine operations.

**Why this priority**: Marker-level detail turns high-level graphs into concrete optimization targets, but the profiler can still deliver value with frame and category views first.

**Independent Test**: Can be fully tested by recording a session containing known named operations and confirming the profiler displays those operation names, nesting, durations, and relative cost within a selected frame.

**Acceptance Scenarios**:

1. **Given** editor and engine operations have named profiler samples, **When** the user selects a frame, **Then** the profiler displays sample names, durations, ordering, and nesting for that frame.
2. **Given** a sample appears many times in a frame, **When** the user inspects the details, **Then** the profiler reports both individual occurrences and aggregated cost.
3. **Given** a sample is recorded from work that occurs outside the main engine flow, **When** the user views task-related data, **Then** the profiler distinguishes that work from main-thread activity.

---

### User Story 4 - Save and Reopen Profiling Evidence (Priority: P4)

As an engine developer, I want each completed profiling capture to be saved immediately and reopened as the active saved capture so that performance investigations always operate on reproducible project-local evidence.

**Why this priority**: Persistence is valuable for collaboration and regression analysis, but it depends on the core capture and inspection experience.

**Independent Test**: Can be fully tested by recording a profiling session, ending the capture, verifying it is immediately saved into the currently opened project's `Profiler/` folder, verifying the profiler opens that newly saved capture automatically, then closing the editor and reopening the same capture from that folder.

**Acceptance Scenarios**:

1. **Given** the profiler is recording captured frames and a project is open, **When** the user ends the capture, **Then** the profiler immediately creates a reusable profiling artifact under `<OpenedProjectRoot>/Profiler/` and opens that newly saved artifact as the active capture.
2. **Given** saved profiling artifacts exist under `<OpenedProjectRoot>/Profiler/`, **When** the user chooses one from the profiler's in-editor capture list, **Then** the profiler restores frame summaries, selected-frame details, categories, markers, and recorded metadata.
3. **Given** a saved profiling artifact is incompatible or corrupted, **When** the user attempts to open it, **Then** the profiler reports a clear error and leaves the current editor session stable.

### Edge Cases

- Recording is started when no scene is loaded or the editor is idle.
- The selected frame is overwritten by a rolling capture buffer while the user is inspecting it.
- A frame contains incomplete samples because recording started or stopped mid-frame.
- Very long sessions exceed the default capture buffer or available memory budget.
- Captured data contains many repeated markers with identical names.
- Background task activity overlaps with main-thread engine activity during the same frame.
- A saved capture was produced by a different TerraEngine version.
- The opened project does not yet have a `Profiler/` folder when the user saves the first capture.
- Automatic save succeeds but automatic reopen fails validation.
- Automatic save fails after recording stops.
- The user expects to inspect captures from another project while a different project is currently open.
- The currently opened project's `Profiler/` folder contains non-capture files or unsupported capture artifacts.
- Profiling itself adds noticeable overhead to the editor session.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST provide an editor-only profiler entry point that is accessible from the TerraEngine Editor and unavailable as an end-user runtime feature.
- **FR-001a**: The editor-only entry point MUST capture engine runtime systems that execute inside the opened project session; the profiler MUST NOT record editor UI or editor window work as profiling samples.
- **FR-002**: Users MUST be able to start, pause, resume, finish, and clear profiling capture during an editor session.
- **FR-003**: The system MUST record frame-based performance data while capture is active, including frame ordering, frame duration, and whether a frame is complete or partial.
- **FR-004**: Users MUST be able to select an individual frame and inspect details for that frame without losing the selection as newer frames are captured.
- **FR-005**: The profiler MUST organize captured data into distinct modules for CPU/engine work and task execution.
- **FR-006**: Each profiler module MUST provide a summary view suitable for scanning trends across captured frames and a detail view suitable for investigating a selected frame.
- **FR-007**: The system MUST display named instrumentation samples with timing, nesting, and ordering where that data is available.
- **FR-008**: The system MUST identify the most expensive contributors within a selected frame or module so users can prioritize investigation.
- **FR-009**: The profiler MUST omit memory as a module until TerraEngine has real allocator or GPU memory telemetry to display.
- **FR-010**: The task execution module MUST distinguish background or worker-task activity from CPU/Engine samples when both are present.
- **FR-011**: Users MUST be able to limit capture size or duration so long sessions do not consume unbounded editor resources.
- **FR-012**: The profiler MUST indicate when data is unavailable, incomplete, unsupported, or outside the current capture scope.
- **FR-013**: Users MUST be able to save profiling captures into `<OpenedProjectRoot>/Profiler/` and reopen captures from that same project folder later from the editor.
- **FR-014**: The system MUST create or prepare the opened project's `Profiler/` folder when saving the first capture if it does not already exist.
- **FR-015**: The profiler MUST only read and write profiling captures within the currently opened project directory.
- **FR-016**: Users MUST open saved captures by selecting from a profiler-provided list of valid captures found under `<OpenedProjectRoot>/Profiler/`; arbitrary file selection is not required for this feature.
- **FR-017**: The profiler MUST ignore or clearly reject paths and capture entries that would require accessing files outside the currently opened project directory.
- **FR-018**: Saved captures MUST include enough metadata for users to understand the project, scene/session context, capture time, TerraEngine version, capture settings, and originating project folder.
- **FR-019**: When the user finishes a recording, the profiler MUST immediately save the completed capture to `<OpenedProjectRoot>/Profiler/`, refresh the in-editor capture list, and open the newly saved capture as the active inspected artifact.
- **FR-020**: If automatic save or automatic reopen fails, the profiler MUST report the failure clearly and preserve as much captured data as possible without accessing files outside the opened project.
- **FR-021**: The profiler MUST preserve editor stability if capture, inspection, saving, or loading fails.
- **FR-022**: The profiler MUST present its information in a way that is usable during active editor work, with controls and labels that support fast diagnosis rather than requiring external documentation.
- **FR-023**: The feature MUST not require users to modify game/runtime builds in order to profile editor-only workflows.

### Key Entities

- **Profiling Session**: A capture period created in the editor; includes capture state, settings, metadata, frame records, automatic save/open status, and saved/reopened artifact identity.
- **Frame Record**: A single captured frame; includes order, duration, completeness, module summaries, and links to detailed samples.
- **Profiler Module**: A user-facing execution area such as CPU/engine work or task execution; includes summary metrics and selected-frame details.
- **Instrumentation Sample**: A named recorded operation; includes start/order information, duration, nesting relationship, category, and optional thread or task context.
- **Capture Artifact**: A saved profiling session stored under `<OpenedProjectRoot>/Profiler/` that can be reopened later from that opened project only; includes current-format metadata and the captured frame/module/sample data.
- **Capture Settings**: User-controlled limits and recording preferences, including buffer size or duration and which modules are active.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can open the profiler, start recording, select a captured frame, and view a timing breakdown in under 30 seconds from an already running editor session.
- **SC-002**: During a representative editor scene, the profiler displays new captured frames within 2 seconds of recording activity for at least 95% of frames.
- **SC-003**: A recorded session of at least 10,000 frames remains inspectable without the editor becoming unresponsive for more than 1 second during common navigation actions.
- **SC-004**: At least 90% of named instrumentation samples in a controlled test scene appear in the selected-frame details with the expected hierarchy and category.
- **SC-005**: A completed capture is saved and opened as the active inspected capture with all frame summaries, module details, and metadata intact in 100% of current-format validation test cases.
- **SC-006**: Profiling overhead remains low enough that a representative editor scene does not exceed a 5% average frame-time increase when recording default modules.
- **SC-007**: In usability validation, at least 80% of engine contributors can identify the top performance contributor in a supplied capture within 2 minutes.

## Assumptions

- The profiler is for TerraEngine Editor workflows only and is not intended as a player/runtime profiling UI for this feature.
- The first version focuses on local editor capture and saved local artifacts under the opened project's `Profiler/` folder; remote device profiling, network streaming, cross-project capture browsing, and arbitrary file-dialog loading are out of scope.
- The profiler should resemble common engine profiler workflows, including live recording, frame selection, modules, timelines or summaries, and marker details.
- Required initial modules are CPU/engine work and task execution; rendering samples that run on the engine thread belong to CPU/Engine, while rendering work dispatched to thread-pool tasks belongs to Task Execution.
- Capture defaults should favor low overhead and bounded memory use over collecting every possible detail.
- Existing editor permissions are sufficient; no new user roles or security model are introduced by this feature.
- Saved captures are intended for debugging and collaboration inside the development team, not as a public analytics or telemetry format.
