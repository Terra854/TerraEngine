# Research: Editor Profiler

## Decision: Native C++20 Profiler Core With RAII Instrumentation

**Rationale**: Engine, renderer, and task execution code are native C++ paths. A native profiler core can capture timing scopes with minimal interop overhead, reuse C++20 chrono/atomic/thread-local primitives, and expose immutable snapshots to the editor UI. RAII scope helpers make instrumentation safe across early returns and exceptions.

**Alternatives considered**:

- Managed-only profiler window: rejected because it cannot directly instrument native engine hot paths without extra interop plumbing and would miss task/render internals.
- External profiler integration: rejected for this feature because the spec requires an editor-only TerraEngine workflow with project-scoped saved captures and no new dependency.

## Decision: Bounded Live Capture Buffer With Immutable Selected Frames

**Rationale**: The spec requires long sessions to stay inspectable and selected frames to remain stable while recording continues. A bounded rolling buffer controls memory use, while a selected frame snapshot can be pinned or copied so it is not overwritten during inspection.

**Alternatives considered**:

- Unbounded capture list: rejected because long sessions could consume unbounded memory.
- Pause recording whenever a frame is selected: rejected because users need to inspect one frame while new frames continue to record.

## Decision: Project-Scoped Capture Store Under `<OpenedProjectRoot>/Profiler/`

**Rationale**: The user explicitly requires profiler captures to work only on the opened project and not go outside the project directory. The capture store will derive all save/list/load paths from `ProjectManager::GetProjectRoot() / "Profiler"`, canonicalize paths, and reject anything outside the active project root.

**Alternatives considered**:

- File dialog loading: rejected because the user said a selectable list is enough and arbitrary paths increase boundary risk.
- Global editor capture directory: rejected because captures must belong to the opened project.

## Decision: In-Editor Capture List Instead of File Browser

**Rationale**: A list of valid captures from the active project's `Profiler/` folder satisfies loading requirements and keeps the UI faster and safer. It also avoids path confusion when switching projects.

**Alternatives considered**:

- Reusing ImGuiFileDialog: rejected because it would encourage arbitrary path selection and is unnecessary for the scoped workflow.

## Decision: Use `ThreadPool` for Save/Load/List Refresh and Aggregation

**Rationale**: The constitution requires task-based concurrency, and the editor must not stall during disk I/O or expensive aggregation. Capture serialization, capture loading, directory scanning, and optional sample aggregation should run as background tasks and publish results back to the editor service.

**Alternatives considered**:

- Synchronous save/load in the profiler window: rejected because large captures can stall editor interaction.
- Manual worker threads: rejected because the constitution directs new asynchronous work through `ThreadPool`.

## Decision: No New Third-Party Dependencies

**Rationale**: TerraEngine already vendors JSON, ImGui, Vulkan helpers, and threading support. The profiler can use those existing facilities, keeping dependency management hermetic and avoiding `External/` changes.

**Alternatives considered**:

- Adding Tracy/Remotery/Perfetto: rejected for this feature because it would require dependency approval and does not match the editor-only project-scoped capture workflow.

## Decision: JSON Metadata Plus Compact Capture Payload

**Rationale**: Capture artifacts need project/session metadata, exact current-format validation, and enough recorded data for reopening. JSON metadata keeps error reporting approachable; the payload can remain compact enough for large frame counts.

**Alternatives considered**:

- Full JSON for every sample: rejected for large captures because repeated sample records would be bulky and slower to parse.
- Opaque binary only: rejected because users and maintainers need readable metadata for validation and troubleshooting.
