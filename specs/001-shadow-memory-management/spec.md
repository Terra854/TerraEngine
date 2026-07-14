# Feature Specification: Shadow Memory Management

**Feature Branch**: `001-shadow-memory-management`  
**Created**: 2026-04-21  
**Status**: Draft  
**Input**: User description: "Shadow caching, atlas management, and memory-safe high shadow counts"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Raise Shadow Capacity Without Memory Spikes (Priority: P1)

As a scene author, I can configure a high maximum number of shadow-capable lights without the renderer reserving worst-case memory for every possible light type up front.

**Why this priority**: The current pain point is huge memory growth when increasing the shadow count, even before the scene visibly benefits from those extra shadows.

**Independent Test**: Configure a scene with a high shadow capacity and a small number of active shadow-casting lights, then verify memory remains proportional to the active shadow workload rather than the configured maximum.

**Acceptance Scenarios**:

1. **Given** a scene with 4 active shadow-casting lights and a maximum shadow capacity of 32, **When** the scene starts, **Then** shadow memory usage remains close to the resources needed for the 4 active lights instead of reserving all 32 slots.
2. **Given** a scene that only uses 2D shadow-casting lights, **When** shadow resources are allocated, **Then** point-light cubemap resources are not reserved solely because the global shadow capacity is high.
3. **Given** a scene where the shadow capacity is increased from 8 to 32 without adding active shadow-casting lights, **When** the frame is rendered, **Then** memory usage does not grow in proportion to the capacity increase.

---

### User Story 2 - Reuse Stable Shadow Results (Priority: P2)

As a scene author, I want stable lights and unchanged shadow casters to reuse valid shadow results so that adding more shadow-capable lights does not force unnecessary shadow redraw work every frame.

**Why this priority**: Once memory allocation is controlled, repeated full shadow rendering becomes the next scalability bottleneck for high shadow counts.

**Independent Test**: Render a scene with static shadow-casting lights and static occluders across multiple frames, then verify the visible shadows remain correct while unnecessary shadow refreshes are avoided.

**Acceptance Scenarios**:

1. **Given** a static light, static receiver view, and unchanged shadow-casting objects, **When** consecutive frames render, **Then** the previous valid shadow result may be reused without visible lighting changes.
2. **Given** a shadow-casting object moves, changes visibility, changes material shadow behavior, or changes geometry, **When** the next frame renders, **Then** all affected cached shadow results are refreshed before being reused.
3. **Given** a light moves, rotates, changes range, changes cone angle, changes shadow strength, or changes shadow bias, **When** the next frame renders, **Then** that light's cached shadow result is treated as invalid.

---

### User Story 3 - Pack 2D Shadows Efficiently (Priority: P3)

As a scene author, I want many spot, area, and directional shadows to share managed shadow storage so memory can scale by actual quality and usage needs instead of by one full-size image per possible shadow.

**Why this priority**: Efficient packing improves memory use for many 2D shadows after allocation is made demand-driven.

**Independent Test**: Render a scene with many mixed-resolution 2D shadows and verify that shadows remain visually correct while storage usage grows by allocated shadow area rather than by full-size per-light images.

**Acceptance Scenarios**:

1. **Given** several small or low-priority 2D shadow-casting lights, **When** shadow storage is assigned, **Then** those shadows can occupy smaller managed regions without forcing every shadow to use maximum resolution.
2. **Given** a high-priority 2D shadow-casting light near the camera, **When** shadow storage is assigned, **Then** it can receive a larger region than lower-priority or distant lights.
3. **Given** managed shadow storage becomes full, **When** additional eligible lights request shadows, **Then** lower-priority shadows are degraded or omitted with diagnostics rather than causing uncontrolled memory growth.

---

### Edge Cases

- A high configured shadow capacity with zero active shadow-casting lights must allocate no unnecessary shadow maps.
- Mixed light types must not cause every shadow slot to allocate resources for every possible type.
- Point-light shadows must account for their higher storage and refresh cost when budgets are applied.
- Fast light or object movement must not reuse stale shadows that visibly lag behind the scene.
- Shadow storage fragmentation must recover over time as lights are removed, downgraded, or become inactive.
- Quality preset changes must resize or release shadow storage without requiring an application restart.
- If memory budget is exceeded or allocation fails, the scene must continue rendering with clear shadow degradation diagnostics.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST allocate shadow storage according to actual active shadow assignments, not solely according to the configured maximum shadow count.
- **FR-002**: System MUST allocate shadow storage by required light category so unused categories do not consume memory for each configured shadow slot.
- **FR-003**: System MUST expose diagnostics showing configured shadow capacity, active shadow count, allocated shadow storage count, memory estimate, cache reuse count, refresh count, and degraded shadow count.
- **FR-004**: System MUST preserve existing light prioritization behavior when choosing which lights receive shadows under constrained budgets.
- **FR-005**: System MUST support reusable shadow results for lights whose shadow inputs have not changed.
- **FR-006**: System MUST invalidate reusable shadow results when a light's shadow-affecting properties change.
- **FR-007**: System MUST invalidate reusable shadow results when any relevant shadow-casting object changes transform, geometry, visibility, or material shadow behavior.
- **FR-008**: System MUST avoid reusing a cached shadow if doing so would produce visibly stale or incorrect shadows.
- **FR-009**: System MUST manage 2D shadow storage so shadows can use variable-sized regions based on quality, importance, and available budget.
- **FR-010**: System MUST support releasing or repurposing shadow storage when lights stop casting shadows, become inactive, are removed, or lose priority.
- **FR-011**: System MUST prevent the configured maximum shadow count from causing unbounded memory growth beyond the active workload and configured quality limits.
- **FR-012**: System MUST degrade shadows predictably when active shadow demand exceeds available storage, preferring higher-priority, more visible, and more stable lights.
- **FR-013**: System MUST keep non-shadowed retained lights contributing direct lighting when they do not receive shadow storage.
- **FR-014**: System MUST provide repeatable validation scenes or tests covering high configured capacity, low active shadow count, many 2D shadows, point shadows, cache reuse, cache invalidation, and graceful degradation.
- **FR-015**: System MUST document how authors can configure shadow capacity, quality, caching expectations, and expected memory behavior.

### Key Entities *(include if feature involves data)*

- **Shadow Assignment**: A per-frame decision that grants or denies shadow rendering to an eligible light, including priority, visibility, and degradation reason.
- **Shadow Storage Region**: A reserved area used by one active shadow result, with category, size, owner, quality level, and lifecycle state.
- **Shadow Cache Entry**: A reusable shadow result tied to a light and its shadow-affecting scene inputs, including validity, age, and refresh reason.
- **Shadow Diagnostics Snapshot**: A report of active shadows, allocated storage, estimated memory, reuse behavior, refresh behavior, and degradation decisions.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Increasing configured shadow capacity from 8 to 32 with no increase in active shadow-casting lights increases shadow memory by no more than 10%.
- **SC-002**: A scene with 4 active 2D shadow-casting lights and a configured capacity of 32 uses at least 70% less shadow memory than reserving full resources for all 32 possible slots.
- **SC-003**: A static scene with stable shadow-casting lights reuses valid shadow results for at least 80% of eligible shadow frames after the initial warm-up.
- **SC-004**: When a shadow-casting object or light changes, affected shadows refresh within the next rendered frame and no stale shadow remains visible for more than one frame.
- **SC-005**: A stress scene with 32 shadow-capable lights remains within the selected quality preset's shadow memory budget while clearly reporting any degraded shadows.
- **SC-006**: Point-light shadows consume memory only for active point shadows and do not increase memory for scenes containing only 2D shadow-casting lights.
- **SC-007**: Users can inspect diagnostics and identify why each shadow-capable light was assigned, cached, refreshed, or degraded in one troubleshooting pass.

## Assumptions

- Existing lighting budgets and author priority values remain the basis for deciding which lights receive shadows.
- Directional, spot, and area lights are treated as 2D shadow users for storage management purposes.
- Point lights remain more expensive than 2D lights because they require omnidirectional coverage.
- Visual correctness is more important than cache reuse; cache entries must be refreshed whenever validity is uncertain.
- The first deliverable may focus on memory-safe allocation and diagnostics before optimizing every possible refresh case.
- Existing quality presets continue to define the expected shadow quality and memory envelope.
