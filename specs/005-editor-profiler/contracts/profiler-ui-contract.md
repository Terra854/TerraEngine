# Profiler UI Contract

## Scope

This contract describes the editor-facing behavior between `ProfilerWindow`, `EditorProfilerService`, and `ProfilerCaptureStore`. It is not a public runtime API and the UI/save-load workflow must remain editor-only, while captured samples cover engine systems running for the opened project and exclude editor UI/window work.

## Entry Points

### Open Profiler Window

**Preconditions**:

- TerraEngine Editor is running.
- A project may or may not be open.

**Behavior**:

- If no project is open, the window shows an idle state and does not offer save/load actions.
- If a project is open, the window can record live data, finish a capture into a saved artifact, and list captures from `<OpenedProjectRoot>/Profiler/`.

### Start Recording

**Preconditions**:

- A project is open.
- At least one profiler module is enabled.

**Behavior**:

- Begins collecting frame records.
- Marks the first frame partial if recording begins mid-frame.
- Updates live frame summaries without requiring a runtime/player build.

**Failure behavior**:

- If no project is open, no capture starts and the UI reports that profiling is tied to an opened project.

### Pause and Resume Recording

**Preconditions**:

- A live session exists.

**Behavior**:

- Pause stops appending frames and preserves captured frames.
- Resume appends new frames using the same settings.

### Finish Capture

**Preconditions**:

- A project is open.
- A live or paused session contains captured frames.

**Behavior**:

- Stops appending frames.
- Ensures `<OpenedProjectRoot>/Profiler/` exists.
- Saves the completed capture under that folder through a background task.
- Refreshes the in-editor capture list.
- Opens the newly saved capture as the active inspected artifact after validation succeeds.

**Failure behavior**:

- If saving fails, the UI reports the failure and preserves the in-memory capture when possible.
- If opening the newly saved artifact fails, the UI reports the failure and does not access any fallback path outside the opened project.

### Select Frame

**Preconditions**:

- The session contains at least one frame.

**Behavior**:

- Selecting a frame pins or copies the frame detail needed by the UI.
- The selected frame remains inspectable while live recording continues.

### Save Capture Artifact

**Preconditions**:

- A project is open.
- The session contains captured frames.

**Behavior**:

- Ensures `<OpenedProjectRoot>/Profiler/` exists.
- Saves the capture under that folder.
- Runs disk serialization through a background task.
- Refreshes the in-editor capture list after a successful save.
- When invoked as part of finishing a recording, opens the newly saved artifact automatically.

**Failure behavior**:

- Any failure leaves the current session inspectable and reports a clear error.
- The save path must be rejected if canonicalization would place it outside the opened project root.

### Refresh Capture List

**Preconditions**:

- A project is open.

**Behavior**:

- Scans `<OpenedProjectRoot>/Profiler/`.
- Lists only valid capture artifacts.
- Ignores non-capture files and unsupported artifacts or marks them as unsupported without offering them as loadable captures.
- Runs directory scanning through a background task when it may be expensive.

### Load Capture From List

**Preconditions**:

- A project is open.
- The selected item came from the current profiler capture list.

**Behavior**:

- Loads the selected capture from `<OpenedProjectRoot>/Profiler/`.
- Replaces the current loaded/idle session only after validation succeeds.
- Restores frame summaries, selected-frame detail, categories, markers, and metadata.

**Failure behavior**:

- Corrupted, incompatible, or out-of-project captures do not replace the current session.
- The UI reports the reason and keeps the editor stable.

## Project Boundary Rules

- The profiler must only read and write paths inside the currently opened project directory.
- Capture loading must not expose a file dialog for arbitrary paths.
- The selectable capture list is sourced only from `<OpenedProjectRoot>/Profiler/`.
- Canonical path containment checks are required before save, list, and load operations.
- Project close clears live capture state and invalidates pending capture list results for the old project.

## Minimum Window Layout

- Toolbar: record/pause/resume, finish capture, clear, refresh list.
- Left pane: captured frames or timeline summary.
- Module selector: CPU/engine and task execution.
- Detail pane: selected-frame module data and named samples.
- Capture list pane: saved captures from the active project only.
- Status area: recording state, buffer use, automatic save/open progress, load progress, and errors.
