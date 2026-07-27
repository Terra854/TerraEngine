# Animation authoring in the editor

The **Animation** menu exposes Skeleton Editor, Animation Clip Editor,
Animation Controller, Retarget Editor, Motion Database, Ragdoll Editor, and
Animation Diagnostics. Each surface is a temporary editor view: it works from
immutable asset snapshots and never writes a scene component while browsing.

## Asset and edit workflow

Create, import, inspect, duplicate, move, rename, delete, and validate skeletons,
clips, controllers, masks, retarget profiles, motion databases, and ragdoll
profiles from the Project Browser's Animation category. An edit starts a named
authoring transaction. It stages the whole candidate, associates it with the
observed asset generation, and publishes only after validation/compile completes.
If the asset changed, or the task is cancelled, the old published generation stays
available. One grouped edit creates one undo/redo entry.

Creation is available from **Project Browser → Add → Animation** and creates a
typed source document in the selected folder. The Project Browser stages the
file and lets the normal AssetDatabase watcher/importer observe it; it does not
write AssetDatabase records itself. New documents are deliberately marked
*not validated* until their editor validates/compiles them. A failed validation
records diagnostics but leaves the last-good source and published generation
untouched. Worker preparation receives a copied `(asset id, generation, request
id, candidate)` packet, and only the editor boundary can complete it. Canceling
an in-flight packet, editing again, moving/renaming the asset, or reimporting a
new generation makes its eventual result stale and harmless.

Lifecycle actions share inverse history entries: duplicate, move, rename, and
delete can be undone/redone as whole logical operations. The asset authoring
history separately groups multi-field graph, event, mask, retarget, database,
and ragdoll edits into one named undo entry. Use validation before relying on a
newly created asset in a controller or scene; windows inspect immutable checked
snapshots and retain their prior snapshot when a reload is rejected.

## Tools

- Skeleton Editor presents hierarchy, bind/current poses, sockets, limits,
  influences, and compatibility data.
- Clip Editor uses fixed ticks for playback, scrubbing, stepping, trimming,
  events, curves, morphs, root motion, and compression settings.
- Controller, Retarget, Motion Database, and Ragdoll editors author their
  graph/mapping/search/physics profile data through the same transaction path.
- Scene View handles expose IK, sockets, joint limits, and contacts; overlays
  are opt-in and bounded. With no renderer they remain disabled safely.

## Preview, recording, prefabs, and diagnostics

Preview sessions have isolated playback worlds and fixed-tick cursors. They are
cleared on selection or Play Mode changes and never mutate the selected
`AnimatorComponent`. Recording accepts ordered bounded channel samples, supports
cancellation, then creates a complete `.teanim` candidate for atomic publication.

Recording channels are selected explicitly (bone translation/rotation/scale,
morph, curve, root motion, and events), together with a fixed source range and
clock. The capture bridge reads only a sealed, immutable `AnimationSystem`
publication plus a copied stable-joint binding; it never reads live animator
storage. Each sample is chunked under a private staging directory, progress and
diagnostics are copied for the window/automation client, and reduction plus
codec validation occur before a same-volume atomic `.teanim` replacement. A
cancel, stale publication generation, invalid pose binding, or validation failure
leaves the prior source clip byte-for-byte unchanged. Automation recording jobs
retain their start/progress/cancel/final result in the editor job store; polling
must occur at the protected snapshot-capture boundary.

Prefab animation overrides can be applied or reverted. Entering Play Mode captures
the authored animation state and leaving it restores that exact copy, including
connected-instance values. Diagnostics and overlays consume copied snapshots only;
collapsed or disabled views collect no expanded presentation data.

## Reproducible checks

From a configured build tree, build the focused editor test target and run its
Animation filter:

```powershell
cmake --build --preset RelWithDebInfo --target TerraEngine-EditorTests
$env:TERRAENGINE_TEST_FILTER = 'Editor.Animation'
./out/build/RelWithDebInfo/TerraEngine-EditorTests.exe
```

Installed-editor click-through validation is performed separately by the Phase 13
gate. Performance measurement is not implied by these functional checks.
