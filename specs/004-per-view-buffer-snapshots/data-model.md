# Data Model: Per-View Buffer Snapshots

## Render Snapshot

Represents one isolated render-data capture for a view, camera output, or pass.

**Fields**:

- `snapshotId`: Stable identifier unique within the active frame.
- `owner`: View, camera output, or pass that owns the snapshot.
- `frameIndex`: Frame-in-flight index that scopes GPU lifetime.
- `viewProjection`: Camera view/projection data used to build culling and lighting data.
- `viewportSize`: Target width and height used for cluster and culling payloads.
- `lightingPayloadRange`: Snapshot-owned light payload storage or range.
- `frameCullingRange`: Snapshot-owned frame culling storage or range.
- `clusterDataRange`: Snapshot-owned clustered-lighting storage or range.
- `indirectDrawRange`: Snapshot-owned indirect draw storage or range.
- `shadowAssignments`: Shadow resource assignments and sampling metadata valid for this snapshot.
- `diagnosticsSlot`: Diagnostics storage associated with this snapshot.
- `state`: `Acquired`, `Prepared`, `Recorded`, `Submitted`, `Completed`, or `Fallback`.

**Validation rules**:

- A snapshot cannot be recorded until all required payload ranges are valid.
- A later snapshot must not overwrite storage still owned by an earlier in-flight snapshot.
- Completed snapshots return their storage to the frame's reusable pool only after all readers are finished.

## Camera Output

Represents a main-window or offscreen camera result.

**Fields**:

- `outputId`: Stable output identifier for diagnostics and validation.
- `outputKind`: `SceneView`, `GameView`, `MainRuntimeView`, `OffscreenTexture`, or `ValidationOnly`.
- `priority`: Required or optional output handling when snapshot budget is constrained.
- `targetSize`: Output width and height.
- `targetAspectRatio`: Aspect ratio used for projection and validation.
- `cameraWorldPosition`: Camera position used by lighting, shadows, and culling.
- `viewMatrix`: Camera view transform.
- `projectionMatrix`: Camera projection transform.
- `renderSettings`: Quality and lighting settings used by the output.

**Validation rules**:

- Required outputs must either receive isolated snapshots or trigger a correctness-preserving fallback.
- Optional outputs may be skipped when snapshot budget is exhausted, but the skip must be reported.
- Different target sizes or aspect ratios must produce independent culling and cluster payloads.

## Snapshot Budget

Defines bounded per-frame snapshot capacity.

**Fields**:

- `maxSnapshotsPerFrame`: Maximum active snapshots allowed for one frame.
- `requiredSnapshotCount`: Number of required outputs/passes in the current frame.
- `optionalSnapshotCount`: Number of optional outputs requested in the current frame.
- `allocatedSnapshotCount`: Number of snapshots successfully acquired.
- `pressureLevel`: `None`, `NearLimit`, `Exhausted`, or `FallbackUsed`.

**Validation rules**:

- Budget exhaustion must never cause cross-output data contamination.
- Required output fallback must be visible in diagnostics.
- Optional output skipping must be deterministic and reported.

## Snapshot Diagnostics

Reports snapshot ownership and safety status for developers, validation tools, and editor diagnostics.

**Fields**:

- `activeSnapshotCount`: Number of snapshots active in the latest frame.
- `completedSnapshotCount`: Number of snapshots completed in the latest frame.
- `ownerSummaries`: Per-owner snapshot status, target kind, and target size.
- `fallbackEvents`: Fallback synchronization or skipped-output events.
- `resourcePressure`: Snapshot budget pressure and peak usage.
- `lightingIsolationValid`: Whether lighting payloads were isolated for inspected outputs.
- `clusterIsolationValid`: Whether clustered-lighting payloads were isolated for inspected outputs.
- `shadowIsolationValid`: Whether shadow assignments were isolated for inspected outputs.
- `indirectDrawIsolationValid`: Whether indirect draw payloads were isolated for inspected outputs.

**Validation rules**:

- Diagnostics must identify the owner of every active snapshot.
- Any fallback must include the owner and reason.
- Dual-view and offscreen validation must be able to compare expected owner counts against diagnostics.

## Fallback Synchronization Event

Represents a correctness-preserving fallback used when snapshot isolation cannot be provided.

**Fields**:

- `owner`: Output or pass that triggered fallback.
- `reason`: `BudgetExhausted`, `AllocationFailure`, `UnsupportedTarget`, or `Recovery`.
- `required`: Whether the affected output was required.
- `result`: `SerializedSafely`, `SkippedOptionalOutput`, or `FrameRecovery`.
- `message`: Developer-facing explanation.

**Validation rules**:

- Required outputs must not silently skip rendering.
- Optional outputs may skip only when clearly reported.
- Fallback events should be rare in warmed-up normal dual-view rendering.

## Snapshot State Transitions

```text
Requested -> Acquired -> Prepared -> Recorded -> Submitted -> Completed -> Released
              |             |           |
              |             |           `-> Fallback
              |             `-> Fallback
              `-> Fallback
```

**Rules**:

- `Requested` to `Acquired`: capacity and target compatibility are checked.
- `Acquired` to `Prepared`: payloads are uploaded or assigned to snapshot-owned ranges.
- `Prepared` to `Recorded`: command buffers bind the snapshot-owned resources.
- `Recorded` to `Submitted`: work is queued without relying on a later output's uploads.
- `Submitted` to `Completed`: GPU work has finished reading snapshot-owned resources.
- `Completed` to `Released`: resources become available for reuse.
