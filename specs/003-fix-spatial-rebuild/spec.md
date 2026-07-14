# Feature Specification: Fix Spatial Rebuild

**Feature Branch**: `003-fix-spatial-rebuild`  
**Created**: 2026-04-22  
**Status**: Draft  
**Input**: User description: "Fix the spatial index so it does not rebuild the entire index every update; use incremental updates or change detection so only changed spatial data is refreshed while preserving correct culling results."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Stable Scenes Avoid Full Spatial Refresh (Priority: P1)

As a developer running a scene where most renderable objects are unchanged, I want the engine to avoid refreshing every spatial entry on every update so large scenes remain responsive.

**Why this priority**: This directly addresses the reported defect and removes the primary source of unnecessary per-frame work.

**Independent Test**: Load a scene with many static renderable objects, run several updates without moving or adding objects, and confirm the spatial organization remains correct while unchanged objects are not fully refreshed each update.

**Acceptance Scenarios**:

1. **Given** a scene with many static renderable objects and no object changes, **When** several updates run, **Then** the engine reuses the existing spatial organization and does not perform a full refresh every update.
2. **Given** a scene with one changed object among many unchanged objects, **When** an update runs, **Then** only the changed spatial data is refreshed and all unchanged objects keep their existing spatial placement.

---

### User Story 2 - Culling Results Stay Correct After Changes (Priority: P1)

As a developer editing or simulating a scene, I want moved, added, removed, hidden, or resource-changed objects to appear or disappear correctly in rendering and diagnostics.

**Why this priority**: Performance improvements are not acceptable if culling accuracy regresses or visible objects are lost.

**Independent Test**: Move, add, remove, hide, and change representative renderable objects across camera views, then verify visible and culled results match the expected scene contents.

**Acceptance Scenarios**:

1. **Given** an object moves from outside the camera view into view, **When** the next update completes, **Then** the object is included in visible rendering candidates.
2. **Given** an object moves from inside the camera view outside view, **When** the next update completes, **Then** the object is removed from visible rendering candidates.
3. **Given** a renderable object is added, removed, disabled, or has its renderable asset changed, **When** the next update completes, **Then** culling and diagnostics reflect the new scene state.

---

### User Story 3 - Diagnostics Reveal Spatial Refresh Cost (Priority: P2)

As an engine developer profiling large scenes, I want diagnostics that show whether spatial data was fully refreshed or incrementally updated so I can verify the performance behavior.

**Why this priority**: The feature is performance-sensitive, and developers need evidence that the unnecessary full refresh has been removed.

**Independent Test**: Run diagnostic views over stable and changing scenes and confirm the reported refresh counts distinguish unchanged, partially changed, and fully refreshed frames.

**Acceptance Scenarios**:

1. **Given** a stable scene, **When** diagnostics are inspected after several updates, **Then** they show no repeated full refreshes.
2. **Given** a scene with a bounded number of object changes, **When** diagnostics are inspected, **Then** they show a bounded number of refreshed spatial entries.

### Edge Cases

- A scene is loaded, unloaded, or replaced entirely and requires a complete spatial refresh once.
- A renderable has missing or invalid bounds and must remain queryable through the existing fallback behavior.
- Multiple objects change in the same update, including parent transform changes that affect child world bounds.
- Objects are created and destroyed rapidly across consecutive updates.
- A mesh or material resource is reloaded and changes the spatial data needed for culling.
- Editor and runtime camera views query the same spatial data during the same update.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The engine MUST avoid full spatial refreshes on updates where the set of renderable objects and their spatially relevant data have not changed.
- **FR-002**: The engine MUST refresh spatial data for renderable objects whose transform, active state, static/dynamic classification, renderable asset, bounds, or spatial category changes.
- **FR-003**: The engine MUST add newly created renderable objects to spatial queries no later than the first update after they become renderable.
- **FR-004**: The engine MUST remove destroyed or non-renderable objects from spatial queries no later than the first update after they stop being renderable.
- **FR-005**: The engine MUST preserve correct visible-object results for all existing camera views after incremental spatial changes.
- **FR-006**: The engine MUST perform a complete spatial refresh when the scene is loaded, replaced, cleared, or when change tracking cannot safely determine the affected objects.
- **FR-007**: The engine MUST preserve fallback handling for objects with unavailable or unusable bounds.
- **FR-008**: The engine MUST expose diagnostics that distinguish full spatial refreshes from partial spatial refreshes and report how many spatial entries changed in the latest update.
- **FR-009**: The engine MUST maintain stable behavior for both editor workflows and packaged runtime sessions.
- **FR-010**: The engine MUST document how developers can verify that stable scenes no longer trigger repeated full spatial refreshes.

### Key Entities

- **Spatial Entry**: A renderable object's queryable scene presence, including identity, active state, classification, category, bounds, and change version.
- **Spatial Change**: A detected difference that can affect culling, visibility, diagnostics, or query membership.
- **Spatial Refresh Summary**: Per-update diagnostic information describing whether spatial data was unchanged, partially refreshed, or fully refreshed.
- **Scene Refresh Boundary**: A scene lifecycle event where a complete spatial refresh is expected and acceptable.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a stable scene with at least 10,000 renderable objects, 95% of updates after warm-up avoid a full spatial refresh.
- **SC-002**: In a scene where 1% or fewer renderable objects change per update, spatial refresh work scales with the changed objects rather than the total renderable count.
- **SC-003**: Across representative camera movement, object movement, object creation, object deletion, and resource-change scenarios, visible-object results match expected results in 100% of validation cases.
- **SC-004**: Diagnostics identify full refreshes and partial refreshes clearly enough that a developer can confirm the feature behavior within one profiling run.
- **SC-005**: Existing scenes continue to render without user-authored migration steps.

## Assumptions

- Static scenes should be the common case optimized by this feature.
- A complete refresh is acceptable during scene load, scene replacement, or when correctness cannot be guaranteed from available change data.
- Correctness is more important than avoiding a full refresh in rare ambiguous cases.
- Existing culling, diagnostics, and fallback-bound behavior should remain functionally compatible.
