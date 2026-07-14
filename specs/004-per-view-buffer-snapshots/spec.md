# Feature Specification: Per-View Buffer Snapshots

**Feature Branch**: `004-per-view-buffer-snapshots`  
**Created**: 2026-04-24  
**Status**: Draft  
**Input**: User description: "spec a transition to per-view/per-pass buffer snapshots instead of queue idling"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Independent Editor View Rendering (Priority: P1)

As an engine developer using both Scene and Game views in the editor, I want each view to render from the lighting, culling, shadow, and draw data captured for that view so one view cannot corrupt or inherit another view's camera-dependent data.

**Why this priority**: This is the correctness goal of the current workaround. The transition is not successful unless Scene and Game views remain visually correct while both are active.

**Independent Test**: Open an editor layout with both Scene and Game views visible, use different camera positions and shadow-relevant lighting, and confirm each view renders with its own expected visible objects, light selection, cluster contents, and shadows.

**Acceptance Scenarios**:

1. **Given** Scene and Game views are both visible with different cameras, **When** a frame renders, **Then** each view uses the view-dependent render data captured for that view.
2. **Given** the Game camera changes while the Scene camera is stationary, **When** consecutive frames render, **Then** the Scene view remains visually stable except for intentional scene changes.
3. **Given** a shadow-casting light is selected differently by each view because of camera position or budget, **When** both views render in the same frame, **Then** each view samples the shadow resources associated with its own selected lights.

---

### User Story 2 - Remove Broad Editor Serialization (Priority: P1)

As an engine developer profiling editor rendering, I want dual-view rendering to avoid forcing the whole graphics workload to finish between views so the editor can keep responsive frame pacing while maintaining correctness.

**Why this priority**: The current workaround protects correctness by serializing editor views. The feature's primary performance value is to remove that broad synchronization point.

**Independent Test**: Run the editor with both Scene and Game views active and confirm frame capture or diagnostics show no mandatory broad synchronization between view recordings while visual correctness remains intact.

**Acceptance Scenarios**:

1. **Given** both editor views are visible, **When** a frame renders, **Then** the engine does not require all previously submitted graphics work to finish before preparing the next view.
2. **Given** one editor view is hidden or disabled, **When** a frame renders, **Then** the engine does not allocate or retain unnecessary view snapshots for the hidden view.
3. **Given** more than one camera or pass contributes to a frame, **When** render work is prepared, **Then** each camera or pass receives an isolated snapshot of the data it needs.

---

### User Story 3 - Diagnose Snapshot Usage and Regressions (Priority: P2)

As an engine developer validating renderer changes, I want diagnostics that show snapshot usage, view isolation, and fallback synchronization so I can prove the transition is working and catch regressions quickly.

**Why this priority**: This change affects correctness and performance in a sensitive render path, so developers need direct evidence that views are isolated and broad serialization is not silently returning.

**Independent Test**: Enable renderer diagnostics during Scene/Game dual-view rendering and confirm the reported data identifies active snapshots, per-view resource use, and any fallback synchronization events.

**Acceptance Scenarios**:

1. **Given** dual-view rendering is active, **When** diagnostics are inspected, **Then** they identify the active snapshot count and which views or passes own those snapshots.
2. **Given** the renderer must fall back to broad synchronization because snapshot resources are unavailable, **When** diagnostics are inspected, **Then** the fallback is clearly reported with a reason.
3. **Given** a validation scene exercises lighting, clustering, shadows, and indirect draws, **When** diagnostics are reviewed, **Then** the developer can confirm each category is isolated per view or pass.

---

### User Story 4 - Prepare Future Multi-Camera Targets (Priority: P2)

As an engine developer adding future multi-camera features such as render-to-texture views, mirrors, minimaps, portals, or camera feeds, I want the snapshot model to support multiple camera outputs in one frame without redesigning the render-data ownership model.

**Why this priority**: The immediate bug is editor dual-view corruption, but the same shared-buffer risk will reappear when more camera outputs are introduced. Accounting for this now avoids a narrow fix that blocks future rendering features.

**Independent Test**: Define a validation scene with a primary camera and at least one additional offscreen camera target, then confirm each camera output uses its own captured visibility, lighting, clustering, shadow, and draw data.

**Acceptance Scenarios**:

1. **Given** a frame includes a main camera and an offscreen camera target, **When** both outputs render, **Then** each output uses the snapshot captured for its own camera and target.
2. **Given** an offscreen camera target renders before or after the main view, **When** frame output is inspected, **Then** neither output inherits camera-dependent data from the other.
3. **Given** multiple camera outputs are active and snapshot budget is constrained, **When** the frame renders, **Then** the engine preserves correctness and reports any fallback or skipped optional output.

### Edge Cases

- Only one editor view is visible, so snapshot allocation should stay minimal.
- Scene and Game views use different viewport sizes, cameras, lighting budgets, and shadow selections in the same frame.
- A view has no renderable objects, no active lights, or no valid camera.
- Offscreen camera outputs are requested for future render-to-texture features such as mirrors, minimaps, portals, camera feeds, or UI previews.
- A camera output targets a texture with a different resolution or aspect ratio than the main view.
- Shadow resources are reused across frames but selected differently across views.
- Multiple game cameras render in sequence during runtime or editor preview.
- Snapshot demand exceeds the configured frame budget and must fail safely without visual corruption.
- The editor resizes or recreates view targets while snapshots from earlier frame work are still in flight.
- Diagnostics or validation capture is enabled and must report the same view ownership as the renderer.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The engine MUST provide isolated render-data snapshots for each active view and pass that can be prepared or executed in the same frame.
- **FR-002**: The engine MUST keep lighting payloads isolated so a later view cannot overwrite the light list used by an earlier view in the same frame.
- **FR-003**: The engine MUST keep clustered-lighting data isolated so cluster contents, light indices, and depth ranges remain associated with the view that generated them.
- **FR-004**: The engine MUST keep shadow-selection and shadow-sampling data isolated so each view samples the shadow assignments selected for that view.
- **FR-005**: The engine MUST keep indirect draw and culling data isolated when those values are generated from view-specific visibility or pass-specific draw lists.
- **FR-006**: The engine MUST preserve correct rendering for editor Scene and Game views when both are visible and use different cameras.
- **FR-007**: The engine MUST avoid mandatory whole-renderer synchronization between editor views during normal dual-view rendering.
- **FR-008**: The engine MUST fail safely if snapshot resources are exhausted by using a correctness-preserving fallback and reporting that fallback through diagnostics.
- **FR-009**: The engine MUST avoid retaining snapshots for inactive or hidden views once they are no longer needed.
- **FR-010**: The engine MUST support snapshot ownership for future multi-camera outputs, including offscreen render targets that are not the main window output.
- **FR-011**: The engine MUST allow camera outputs with different target sizes or aspect ratios to receive independent snapshots.
- **FR-012**: The engine MUST maintain existing behavior for single-view runtime rendering except where snapshot support is needed for correctness or future multi-camera rendering.
- **FR-013**: The engine MUST expose diagnostics showing active snapshot count, owning view, pass, or camera target, fallback synchronization events, and snapshot-resource pressure.
- **FR-014**: The engine MUST include validation coverage demonstrating dual-view correctness for lighting, clustering, shadowing, and view-specific draw data.
- **FR-015**: The engine MUST include at least one validation path that represents a future multi-camera or offscreen camera output, even if the full user-facing render-to-texture feature ships later.
- **FR-016**: The engine MUST document how to configure, build, install, and run the validation scenario from a clean checkout.

### Key Entities

- **Render Snapshot**: A captured set of render data for one view, camera output, or pass, including the data needed for lighting, culling, shadows, and draw submission.
- **View Owner**: The editor view, runtime camera, camera target, or render pass that owns a snapshot for the duration of the work that consumes it.
- **Camera Output**: A main-window or offscreen camera result that can have its own target size, aspect ratio, visibility, lighting, and shadow needs.
- **Snapshot Lifetime**: The period from snapshot capture until all work that can read it has completed.
- **Snapshot Budget**: The bounded amount of snapshot storage available for active frame work.
- **Fallback Synchronization Event**: A correctness-preserving event used when snapshot isolation cannot be provided for a frame.
- **Snapshot Diagnostics**: Developer-facing status that reports snapshot ownership, usage, pressure, and fallback events.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a validation scene with both Scene and Game views visible and using different cameras, 100% of inspected frames render each view with its own expected lighting, clustering, shadows, and visible objects.
- **SC-002**: In normal dual-view editor rendering, broad synchronization between views is absent in at least 99% of frames after warm-up.
- **SC-003**: In a scene with at least 1,000 renderable objects and active shadow-casting lights, median dual-view editor frame time improves compared with the current serialization workaround while preserving visual correctness.
- **SC-004**: Snapshot diagnostics identify active ownership and fallback synchronization reasons clearly enough for a developer to verify behavior in one profiling session.
- **SC-005**: Single-view runtime rendering continues to pass existing render validation without user-authored migration steps.
- **SC-006**: A representative future multi-camera validation path can render at least two camera outputs in one frame with independent snapshot ownership and no cross-output data contamination.
- **SC-007**: Clean-checkout verification instructions allow a developer to configure, build, install, and run the relevant validation in one documented flow.

## Assumptions

- Correct rendering remains higher priority than performance; fallback synchronization is acceptable only as a reported safety path.
- Scene and Game views are the primary initial validation target because they exposed the shared-buffer corruption risk.
- The first implementation milestone may validate offscreen camera outputs through a developer-facing test path before a complete user-facing RenderTexture feature exists.
- The snapshot model should support future multi-camera and multi-pass rendering, but the first shipping behavior remains bounded to current editor and runtime render paths.
- Existing render diagnostics can be extended rather than replaced.
- Existing rendering asset separation, build structure, and install requirements remain in force for later implementation work.
