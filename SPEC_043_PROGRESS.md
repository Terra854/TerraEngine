# Spec 043 Overall Progress

Recorded: 2026-07-27 (Asia/Singapore; updated after the other-three-model repair and packaged-runtime closure)

## Current completion snapshot

**285 of 289 tasks (98.6%)** are checked. T281 is reclosed after its glTF
node-transform, FBX bind-pose, deformable owner-mesh, and task-payload defects
were repaired and validated with all four real models. T283/T284 remain the
explicitly skipped active-feature and unrelated-engine performance matrices.
T288 still requires passing, comparable, non-deferred T283/T284 reports, and
T289 requires those reports plus T288 before it can close the remaining
performance-dependent acceptance rows.

### Other-three-model repair and sign-off closure

Every final live operation used the exact project with spaces:

`C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project`

The earlier recheck defects are resolved:

1. AnimatedMorphCube now preserves its source node's authored quaternion and
   `100x` scale through a node-bound synthetic skeleton joint and full palette
   influence. Its visibly extreme shape change is intentional: the Khronos
   asset alternates between authored `thin` and `angle` morph targets.
2. Quaternius BaseCharacter now composes Assimp hierarchy transforms in the
   correct order and derives package-space inverse binds as
   `offset * inverse(meshPlacement)`.
3. Active skin/morph rendering no longer submits page-local virtual-geometry
   ranges against an owner mesh that carries the deformation streams. It
   collapses to one complete owner-mesh draw.
4. The task-to-mesh shader handoff now uses an explicit valid-payload flag for
   visible and shadow passes. A 65-meshlet draw consumes indices 0-64 exactly
   once instead of restarting at meshlets 0-31 for each task group.

Fresh Play Mode captures prove AnimatedMorphCube, RiggedFigure, Fox, and
BaseCharacter all advance. BaseCharacter remains complete across changing
front and actual rear views: the back of the head, torso, both arms/hands,
hips, and both legs remain present and deform coherently. Play exit reports a
complete authored-state restoration, and stopped diagnostics publish sample
tick zero.

Live evidence is under:

`out/validation/animation/import-space-fix/live/final/`

The locked Windows input desktop still prevented a fresh native click on the
pending export approval, so the editor-owned `approval-1` boundary was not
bypassed. The user-authorized command-line exporter was used as the alternative
fresh packaging path. Its 89-entry source-free package is under:

`out/validation/spec043-final/spec043-post-backfix-export-20260727/`

Bootstrap, rendered Vulkan parity, and forced headless CPU fallback all pass.
All four animators evaluate 120/120 ticks, each produces 120 distinct state and
pose hashes, every authoritative status/state/pose/event/root-motion field
matches with zero mismatches, and both lanes shut down orderly. The decoded
1280x720 backend PNG visibly contains the complete BaseCharacter rear view.

T281 passed from the installed editor in:

`out/validation/animation/final/real-models/20260726T023029449Z/result.json`

That earlier automation run validated Khronos AnimatedMorphCube, Khronos RiggedFigure,
Khronos Fox with multiple clips, and the Quaternius Ultimate Animated Character
Pack. It completed import/reimport, controller and advanced authoring,
retargeting, motion-database and ragdoll publication, deterministic recording,
isolated previews, Play Mode and camera restoration, and export. All 10
approval-protected operations were accepted through the actual editor UI. The
manual proof contains 25 real Win32 input records bound to 25 distinct
`PrintWindow` screenshots and reported zero defects at that time:

`out/validation/animation/final/real-models/20260726T023029449Z/editor/manual-evidence.json`

T282 also passed in the same run. Its independently published result is:

`out/validation/animation/final/export-runtime/20260726T023029449Z/result.json`

The exported package was launched without source, editor, or build-tree
dependencies. Its rendered Hybrid Authoritative lane produced a non-vacuous
1280x720 Vulkan frame, its headless lane used CPU Deterministic/Fallback, and
all authoritative status, state, pose, event, and root-motion fields matched
for exactly 120 fixed ticks.

The final integrated correctness verification is green:

- `cmake --build --preset RelWithDebInfo --parallel 4` passed.
- The complete project-owned non-performance CTest surface passed 49/49
  registrations in 64.30 seconds after the latest Transform repair; the final
  transcript is
  `out/validation/animation/transform-regression/nonperformance-ctest-final.log`.
- `cmake --build --preset RelWithDebInfo --target install --parallel 4` passed;
  the final post-repair transcript is
  `out/validation/animation/transform-regression/install-transform-final.log`.
- Performance regression workloads were not executed, exactly as requested,
  because power-limited timings are not comparable.

The latest start-frame/Play-transition repair also passed the complete
non-performance surface: the integrated RelWithDebInfo build succeeded, CTest
passed **49/49** registrations in 63.09 seconds, and the mandatory install
target succeeded. Current transcripts are:

- `out/validation/animation/start-frame-no-flash-fix/integrated-build-after-stop-fix.log`
- `out/validation/animation/start-frame-no-flash-fix/nonperformance-ctest-after-stop-fix.log`
- `out/validation/animation/start-frame-no-flash-fix/install-after-stop-fix.log`

### Persistent Inspector start-frame and autoplay controls

The Animator Inspector Basic tab now exposes **Default Clip Start Frame** as a
persistent authored value. The value uses the animation system's fixed 60 Hz
timeline, so frame 30 starts at 0.5 seconds. It applies to both direct Default
Clip playback and the entry clip selected by an Animation Controller. The
editable widget displays `current/max`, where max comes from the resolved
published direct clip or controller entry clip generation. Slider and typed
input are clamped, and a shorter reimport normalizes any formerly valid authored
value so the Inspector and component cannot retain current above max. Runtime
binding also clamps independently for scenes changed outside the Inspector.

The Basic tab also exposes **Play Automatically**. It defaults to enabled for
historical compatibility and generated animated-model prefabs. Disabling it
holds the configured initial pose when Play Mode/runtime starts until gameplay
or tooling explicitly sends Play; changing it off while running rebuilds the
generation-safe binding and returns to that authored pose. Start frame,
automatic play, and **Loop Default Clip** all apply to the active clip regardless
of whether it came from the direct Default Clip field or a controller entry
state.

The component schema is now version 3. Older scenes migrate the start frame to
zero when absent and automatic playback to enabled. Generated animated-model
prefabs author both defaults explicitly, and scene, prefab, and export
serialization preserve edited values. Runtime binding seeks to the authored
frame before optional automatic playback begins. Consequently the first
evaluated pose, the stopped Edit Mode preview pose, and the pose restored after
leaving Play Mode all agree with the configured start frame. Editing either
field causes a protected rebind.

Managed gameplay code can persist the same setting with
`Animator.DefaultClipStartFrame`; one-off runtime positioning remains available
through `Seek(tick)` and `Play(stateId, tick)`. It can persist the startup choice
with `Animator.PlayAutomatically`. The Inspector labels' hover tooltips document
units, `current/max`, clamping, controller behavior, and how explicit Play
commands interact with the switch.

Focused component-binding, serialization, Inspector-metadata, generated-prefab,
and managed-parity checks passed. The complete Engine.Animation and
Editor.Animation test selections passed with performance and soak cases
excluded. The exact integrated tree built successfully, and the broader
49-registration non-performance CTest surface passed 49/49 in 62.61 seconds
with the performance/soak test filter inherited by monolithic registrations.
The mandatory RelWithDebInfo install then passed with managed compilation at
zero warnings and zero errors.

The newly installed editor was reopened on
`C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project` (including
the spaces) using `--project "<project root>"`, not a `project.json` argument.
An installed-process MCP readback of the real Fox entity reported Animator
schema version 3, `defaultClipStartFrame: 0`, and
`playAutomatically: true`. A live `PrintWindow` capture after selecting Fox
confirmed that the Basic tab renders Play Automatically and Default Clip Start
Frame as dedicated controls:

`out/validation/animation/inspector-playback-controls.png`

The exact `current/max` formatting and clamp rule are covered by the
deterministic Inspector range test; runtime behavior is covered by binding,
serialization/export, full non-performance, and installed-editor MCP checks.
The live editor clamp path was also exercised reversibly: MCP submitted
`defaultClipStartFrame: 1000000` while Fox remained selected, the Inspector
normalized it to the published Fox maximum of 69 on its next draw, and the
field was restored to zero before saving the scene with `dirty: false`. No
performance measurement was executed.

### Controller-owned start frame and looping repair

A later live Fox report exposed that the authored start-frame and loop fields
were still being discarded when `controllerAssetId` was assigned. The binding
signature zeroed `defaultClipStartFrame` for controller-owned playback, and the
resolved controller entry clip never received `loopDefaultClip`. This made both
Inspector controls appear editable while the runtime silently used frame zero
and non-looping playback.

`AnimatorComponentBindingCoordinator` now resolves the controller entry leaf
first and then applies the same authored start tick and loop policy used by a
direct clip. The start tick is clamped against the resolved clip duration. The
controller state machine retains unwrapped timing for exit-time and transition
evaluation, so enabling clip looping does not suppress crossfades. Inspector
`current/max` resolution follows the same bounded nested-entry traversal and
uses the actual controller entry clip instead of an unrelated fallback clip.

`Animation.ComponentBinding.ControllerCompilesAndTransitionsProductionClip`
now proves that a controller binds at authored frame 12 while held, a reflected
Inspector-style edit hot-rebinds it at frame 24, enabled looping advances past
the clip end with a positive loop count, and the existing trigger crossfade
still completes. The focused engine and editor tests passed, and the final
non-performance CTest surface passed **49/49** after the live editor released
port 8765. Both the isolated and normal RelWithDebInfo install targets passed.

The updated editor was launched only with:

`--project "C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project"`

MCP confirmed that exact canonical project root. With automatic play disabled,
frozen Game View captures at authored frames 0 and 30 show visibly different
Fox head, body, and foreleg poses:

- `out/validation/animation/controller-start-loop-fix/live/start-frame-0.png`
- `out/validation/animation/controller-start-loop-fix/live/start-frame-30.png`

With automatic play and looping enabled, captures at 2.2 and 2.5 seconds remain
visibly different after multiple Run cycles instead of freezing at the terminal
pose:

- `out/validation/animation/controller-start-loop-fix/live/loop-after-2.2s.png`
- `out/validation/animation/controller-start-loop-fix/live/loop-after-2.5s.png`

Post-test MCP diagnostics reported one active 25-joint animator, zero degraded
animators, zero rejected batches, and zero stale work. Play Mode exited cleanly,
the original frame-zero/loop/autoplay settings were restored, and the exact
scene was saved with `sceneDirty: false`.

A fresh CLI export then used the exact project root and
`BuildProfiles/d1fa3cfe.json` without overwriting the project's existing build
folder. The source-free package and evidence are under
`out/validation/animation/controller-start-loop-fix/export-runtime/`. Export
completed with 89 TEPAK entries and zero errors; bootstrap succeeded, and both
rendered and headless animation parity lanes passed 120/120 ticks. The rendered
Fox produced 120 distinct state hashes and 120 distinct pose hashes, all
authoritative fields matched headless with zero mismatches, and the rendered
lane returned a real 1280x720 Vulkan readback. No performance workload ran.

### Start-frame slider saturation and flash-free Play/Stop boundaries

A later live report exposed two additional boundary defects. The Inspector
temporarily edited `defaultClipStartFrame` through an unsigned ImGui value, so a
leftward drag could underflow before the post-edit clamp ran. Play/Edit clock
changes also invalidated every component-owned animator; on Stop, the authored
scene restoration replaced the component generation and discarded the already
compiled controller. Those gaps could expose a reference/frame-zero pose while
the controller compiled again.

The Inspector now uses a signed 64-bit edit value and saturates it through the
same range helper used by tests before writing the unsigned reflected property.
Negative drag/input values therefore become zero without wrapping through the
maximum. The runtime now requests an in-place playback restart when only the
clock domain changes, retaining the generation-safe handle and last complete
pose until the next protected evaluation. When Stop atomically restores the
authored scene and legitimately replaces the component generation, the
coordinator transfers an unchanged controller's immutable compiled program to
the replacement entry. The new handle can therefore evaluate the configured
start frame immediately instead of publishing `NotReady`.

Regression coverage first reproduced both failures:

- `Animation.Editor.AnimatorInspectorFrameRangeClampsToPublishedClip`
  supplies `-1` and requires `0/max` instead of unsigned wrap.
- `Animation.ComponentBinding.ControllerCompilesAndTransitionsProductionClip`
  replaces the component generation after Stop and requires the first restored
  publication to be complete at the authored start tick.

The focused tests pass after the repair, as do the complete Engine.Animation and
Editor.Animation selections and the 49-registration non-performance CTest run.
The rebuilt installed editor was launched with only:

`--project "C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project"`

The installed Inspector normalized an oversized start frame of 1000 to the
currently resolved Fox controller-entry maximum of 42, then accepted frame 30.
Five consecutive Play/Stop cycles were sampled at every first protected frame:
all 80 samples retained one complete render binding, nonzero pose/palette
generations, and zero safe fallback, degraded animators, fallback counts, stale
work, or rejected batches. One optional detailed-diagnostics snapshot was still
being copied while its actual render binding and pose were already complete; it
was recorded separately and was not a runtime animation failure. Evidence is:

- `out/validation/animation/start-frame-no-flash-fix/live/installed-inspector-clamp.json`
- `out/validation/animation/start-frame-no-flash-fix/live/installed-play-edit-transition-samples.json`
- `out/validation/animation/start-frame-no-flash-fix/live/final-restored-state.json`

The exact project was restored to frame zero with looping and automatic play
enabled, Play Mode stopped, and the scene saved with `sceneDirty: false`. The
installed editor remains open on that exact project. Export correctness remains
covered by the fresh 89-entry exact-project source-free export above and by the
latest passing `Engine.AnimationParityValidator`, `Editor.ExportParity`, and
other export CTest registrations. No performance measurement ran. The checkbox
count remains 285/289; only T283, T284, T288, and T289 remain open.

### Continuous C# and controller state transitions

`Animator.CrossFade(stateId, transitionSeconds)` now performs a production
two-clip transition instead of only changing controller bookkeeping. The
runtime advances independent source and destination cursors, samples both real
clips, and blends the complete local skeletal pose, morph values, curves,
events, and root-motion delta. Completing a transition promotes the already
advanced destination cursor atomically, so the new animation does not rewind.
An interrupted transition captures the pose that was actually published and
uses that frozen blend as the next source; rapid walk/run/idle changes
therefore remain continuous instead of snapping back to an old endpoint.

The state-machine `Next` interruption path now promotes the selected
destination leaf and entry tick before replacement. Immediate `Play` and
zero-duration crossfades also discard any private in-flight destination cursor
before seeking the requested state. These repairs prevent a prior transition
from overwriting an explicit immediate state change.

The current GPU work-packet ABI names one clip cursor, so an active two-clip
crossfade deliberately reports and uses the deterministic CPU-authoritative
result. This is an honest correctness fallback, not a silent one-clip GPU
sample. Normal non-transition playback keeps its existing execution-mode
selection.

Deterministic regressions now cover:

- the exact native ABI called by exported C# code, with the expected
  `0 -> 2.5 -> 5 -> 7.5 -> 10` pose sequence;
- interruption from an already published half blend, with the expected
  `5 -> 1.25 -> -2.5 -> -6.25 -> -10` continuation;
- a real compiled controller and real `ResourceManager` clips;
- `Next`-source state-machine interruption; and
- managed `Animator.CrossFade(ulong, float)` command-kind, state-ID, and
  duration parity.

The full non-performance animation engine selection passed all 1,343
registered cases, the complete project-owned non-performance CTest surface
passed 49/49, the integrated RelWithDebInfo build passed, and the mandatory
install target passed from the same code state.

The installed editor was then opened with the project root containing spaces:

`C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project`

A real Fox controller was authored with Walk state 100, Run state 200, and
15-fixed-tick transitions in both directions. The real Fox entered Play Mode,
accepted forward and reverse trigger batches against its generation-safe
runtime handle, sampled 25 joints, and reported zero degraded animators, zero
rejected batches, and deterministic publication. Exiting Play Mode reported a
complete authored-state restore; the controller assignment, start frame,
looping, and autoplay values remained authored. Re-entering created one clean
animator again.

Locked-screen MCP and desktop captures returned unchanged pixels even while the
runtime frame/fixed-tick counters advanced, so those particular images were
treated as inconclusive rather than claimed as visual interpolation proof.
The exported-runtime lane supplied the independent rendered proof instead.

Export profile `Animation Export Validation` passed preflight with zero errors.
The exact approval request was accepted in Preferences -> MCP Bridge ->
Approvals, and export job `job-1` completed successfully. Its 51-entry TEPAK
contains controller asset `43000000000000000000000000000001`, all three real
Fox clips, the compatible skeleton, and the animation shader/runtime manifest
closure. The source-free launcher passed `--validate-export-bootstrap`, then
both rendered and headless `--validate-export-animation-parity` lanes passed
all 120 ticks. The real animator produced 120 distinct pose hashes and 120
distinct state hashes; rendered and headless results had zero mismatches. The
rendered lane also returned a valid 1280x720 Vulkan frame:

`out/validation/animation/crossfade/export-runtime/rendered-runtime.png`

Finally, the installed-tree acquired-model automation gate was rerun after the
crossfade change. Khronos AnimatedMorphCube, Khronos RiggedFigure, Khronos Fox,
and Quaternius BaseCharacter all passed their exact preview control sequences,
Play/camera checks, and export preflight:

`out/validation/animation/crossfade/real-models/20260726T155047371Z/result.json`

Its overall label remains `partial-automation-only` only because that rerun
intentionally did not repeat already accepted manual clicks and protected
export/recording approvals. The earlier complete T281/T282 run remains the
accepted manual gate. Performance regression workloads remained skipped as
requested. This enhancement does not change the 285/289 task count or the four
performance-dependent unchecked tasks.

### Correct-project audit and multi-animator GPU ABI repair

The final continuation pinned every installed-editor and export operation to the
project root with spaces:

`C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project`

Every launch used `--project "<project root>"`; no launch appended
`project.json`. `project.state` confirmed both the canonical identity and exact
project root before live mutations. A production-source scan found no absolute
user path, `TerraEngine-Projects` path, `New Animated Project` name, or
`project.json` dependency in `Engine`, `Editor`, `Launcher`, `cmake`, or the
root build configuration. The earlier wrong-project openings therefore affected
only provisional live evidence, not production behavior or serialized source
assumptions.

The correct project then exposed a real two-instance GPU defect. Two identical
RiggedFigure animators produced valid packet metadata, but only the first
instance deformed. The host `GpuAnimationWorkPacket` occupied 144 bytes because
of its required 16-byte alignment, while the HLSL `StructuredBuffer` record
declared only 136 bytes. Shader array element 1 was consequently read eight
bytes early and rejected. The ABI now declares matching zeroed `reserved0` and
`reserved1` lanes in C++ and HLSL, validates them in the shader codec, and locks
their offsets at 136 and 140 in the shader-layout regression. Focused GPU layout
and plan tests passed, the current shaders rebuilt, and the mandatory
RelWithDebInfo install passed after temporary palette-hash instrumentation was
removed.

Live installed-editor validation in the exact project covered:

- two simultaneous RiggedFigure instances with equal nonzero GPU palette hashes
  and visibly changing synchronized poses;
- independent position and scale edits on both animated roots during Play, with
  small/large and left/right placement visible in the rendered frame;
- exact authored Transform and dirty-state restoration on `play.exit`;
- Fox, BaseCharacter, RiggedFigure, and AnimatedMorphCube runtime advancement;
  the isolated morph capture changed 2,982 model pixels; and
- a clean editor shutdown without saving temporary validation objects.

The primary live captures are under:

`out/validation/spec043-gpu-debug/`

A fresh CLI export used the exact project root, profile
`BuildProfiles/d1fa3cfe.json`, and run ID
`spec043-correct-project-final-20260727a`. Its source-free inventory contains
only the launcher, TEPAK, bundled native DLLs, NativeAOT managed assembly, and
runtime settings. Package validation accepted 27 animation assets, 89 archive
entries, and 66 dependency edges with zero validation errors:

`out/validation/spec043-final/spec043-correct-project-final-20260727a/evidence/animation-validation.json`

The exported launcher passed bootstrap plus both 120-tick animation parity
lanes. Rendered and headless runs matched every status, state hash, pose hash,
event count, and root-motion field with zero mismatches; the Fox produced 120
distinct state hashes and 120 distinct pose hashes. The rendered run returned a
real 1280x720 Vulkan backend readback with 3,687,468 decoded PNG bytes.

The exact saved Fox now owns the real two-clip Walk/Run crossfade controller.
Detailed live diagnostics correctly attribute its rendered evaluator choice to
`GpuResourcesUnavailable`: the current GPU program contract accepts one
GPU-safe clip, so a multi-clip crossfade deliberately uses the complete
CPU-deterministic evaluator while Vulkan presents the resulting deformation.
This is the documented correctness fallback for continuous arbitrary-pose
transitions, not missing animation or a renderer fallback. The direct-clip
multi-instance RiggedFigure proof independently confirms the repaired GPU path.

After this repair and audit, the complete non-performance CTest surface passed
49/49 in 60.07 seconds from `out/build/RelWithDebInfo`; the four named
performance/soak registrations and monolithic performance cases were excluded
with `TERRAENGINE_TEST_EXCLUDE_FILTER=Performance,Soak`. No performance timing
was collected.

### Animated-model Transform regression repair

A post-acceptance editor report exposed a real placement defect: reflected
`Transform` writes changed component storage but did not publish the
`ObjectManager` transform/spatial revision consumed by the renderer's
world-transform cache. Inspector position, rotation, or scale edits could
therefore leave an animated renderable at its prior model matrix until some
unrelated spatial mutation refreshed the cache. Animation evaluation and shader
skinning were not overwriting the object transform; the stale cache invalidation
was the fault.

The shared reflected component setter now publishes one transform/bounds
revision after a successful `Transform` property write and after releasing the
component write lock. This covers Inspector editing, Inspector undo/redo,
compatibility automation, managed component access, deferred scene mutation,
and installed/exported runtime bridges. Transactional and legacy managed paths
were reconciled so they do not double-increment the same revision.

Two deterministic regressions now prove both sides of the contract:

- `Engine.ComponentAccess.ReflectedTransformWritesPublishSpatialRevisions`
  verifies the shared reflected setter updates the value plus the transform and
  spatial revisions.
- `Scripting.ComponentAccess.ReflectedTransformWritePublishesOneSpatialRevision`
  verifies the managed/export-facing ABI publishes exactly one revision.
- `Renderer.WorldTransformCache.ReusesStableHierarchyAndInvalidatesChangedBranches`
  now drives its animated-renderable-equivalent placement update through the
  reflected setter rather than a manual dirty call.

The focused Transform, renderer-cache, managed scripting, root-motion, scene
transaction, and animation tests passed. The final full non-performance CTest
surface also passed 49/49 in 64.30 seconds, followed by a successful install
from the same code state.

An installed-editor loopback then used a disposable copy of the accepted
Khronos Fox project. Edit Mode moved the skinned Fox from X=0.0 to X=0.75 and
produced a distinct Vulkan capture. Play Mode evaluated one Hybrid
Authoritative animator with zero fallback and zero stale work; changing X from
0.75 to -0.75 moved the visibly animated Fox and produced another distinct
capture. Exiting Play restored X=0.75 and reproduced the exact authored
pre-Play PNG hash. The machine-readable result and five captures are preserved
under:

`out/validation/animation/transform-regression/live/result.json`

No performance measurement was executed for this repair.

Two resume-only validation defects were fixed in
`Tools/Animation/TestValidationModels.ps1`: the disposable isolated runtime is
now cleaned before recopying an approved export, and an exact already-published
T282 result can be reused idempotently after a later T281 manual timeout. Both
paths were exercised by the successful final retry.

The acceptance matrix now records 249 `Accepted`, 18
`Evidence incomplete`, 1 `In progress`, and 0 `Planned` rows. Every open row
depends explicitly on T283, T284, T288, or T289. No mouse, screen, editor,
model, or exported-runtime verification was skipped.

The sections below preserve the implementation history and earlier
intermediate-state evidence; this snapshot is authoritative for current
status.

### Latest continuation attempt

T253 is accepted from the complete installed-editor run:

`out/validation/animation/us11-automation/live-20260726Tcancel-fix-t253/`

The run passed its integrated build, 47.316-second focused non-performance test
suite, required install, all 75 scenario operations, gate-owned native mouse and
keyboard actions, Play Mode restoration, Scene/Game/custom-camera captures,
MCP reconnect, Vulkan-log inspection, and authored-state/project-delta checks.
Six exact protected requests were approved through Preferences → MCP Bridge →
Approvals: animation import, reimport, motion-database build, successful
recording start/stop, and the separately canceled recording start.

The first complete live attempt exposed a paused-Play cancellation defect:
recording cancellation discarded staged output but duplicate-tick handling
returned before the durable job could become `canceled`. Cancellation now
bypasses duplicate-sample processing and publishes the terminal result without
requiring a new fixed tick. The focused regression
`Editor.Animation.Automation.Runtime.RecordingApprovalBindsExactPublishedPose`
passes with 697 tests and zero failures.

The terminal acceptance receipt is:

`out/validation/animation/us11-automation/live-20260726Tcancel-fix-t253-acceptance-commit.json`

It accepted FR-159 through FR-167 and SC-024 and updated the matrix through the
hash-bound compare-and-swap transaction.

T098 then passed its fresh installed Vulkan correctness gate:

`out/validation/animation/us4-gpu/live-20260726Tnative-t098/`

Its gate-owned native stage emitted 13 successful mouse/keyboard receipts and
three owned window captures, then the scripted stage completed 46 operations,
mode/fallback/residency and zero-readback correctness, Scene/Game/custom-camera
coverage, deformable virtual-geometry and renderer-loss recovery checks, Play
restoration, reconnect, and a clean Vulkan log. The acceptance transaction
committed FR-211 through FR-233 and SC-032 through SC-035; SC-028 through SC-030
remain honestly performance-deferred. No unsafe manual device-destruction action
or performance-regression measurement was attempted.

T134 subsequently passed its fresh cumulative-scale/failure gate:

`out/validation/animation/us14-scale/live-20260726Tnative5-t134/`

The accepted run produced four pixel-distinct gate-owned native captures and 15
successful receipts, including a documented non-destructive presentation
extent recovery plus safe-save. It completed 42 operations, cumulative
mode/reload/visibility/cancellation/renderer-loss checks, cameras, Play
restoration, reconnect, clean Vulkan diagnostics, evidence locking, and its
acceptance transaction. Performance and later-capability rows remain blocked
exactly where the manifest declares them dependent on future phases.

T148 then passed its fresh procedural-animation and IK gate:

`out/validation/animation/us6-procedural/live-20260726Tnative-t148/`

The run passed the integrated build, 46.979-second focused non-performance test
suite, required install, scripted moving-goal/singularity/mode-parity operations,
camera and Play restoration checks, and two pixel-distinct gate-owned native
captures backed by nine successful mouse/keyboard receipts. Its evidence lock
and acceptance transaction committed FR-091 through FR-098 and SC-008.

T163 then passed its fresh animation-retargeting gate:

`out/validation/animation/us7-retarget/live-20260726Tnative-t163/`

The run passed its integrated build, 47.002-second focused non-performance test
suite, required install, 26 retarget scenario operations, hot-reload
last-known-good handling, camera and Play restoration checks, and two
pixel-distinct gate-owned native captures backed by nine successful input
receipts. Its acceptance transaction committed FR-099 through FR-112 and SC-007.

T198 then passed its fresh animation/ragdoll physics-blending gate:

`out/validation/animation/us9-ragdoll/live-20260726Tnative-t198/`

The run passed its integrated build, 49.156-second focused non-performance test
suite, required install, 31 activation/impact/partial-blend/motor/recovery/stale
physics operations, camera and Play restoration checks, and two pixel-distinct
native captures backed by nine successful input receipts. Its acceptance
transaction committed FR-122 through FR-134 and SC-011.

T237 then passed its fresh streaming, reload, export, and installed-runtime gate:

`out/validation/animation/us12-export/live-20260726Tnative-t237/`

The run passed its integrated build, 49.093-second focused non-performance test
suite, required install, all 15 scripted residency/reload/TEPAK/mount/launcher
operations, camera and Play restoration checks, and two pixel-distinct native
captures backed by nine successful input receipts. Its acceptance transaction
committed FR-168 through FR-180 and SC-018 through SC-020.

T270 then passed its fresh replication, rollback, replay, and determinism gate:

`out/validation/animation/us13-determinism/live-20260726Tnative-t270/`

The run passed its integrated build, 50.407-second focused non-performance test
suite, required install, correction/late-join/replay/export/camera/Play
operations, and two pixel-distinct native captures backed by nine successful
input receipts. Its acceptance transaction committed FR-181 through FR-193 and
SC-022. SC-021 remains deliberately evidence-incomplete for final
reconciliation, while the retained Windows MSVC and Ubuntu 24.04 GCC 14 reports
were audited: both pass 100,000 ticks at workers 1/4/16/31 with exact aggregate
hash `17254665474189528098`.

T271 then passed its whole-system correctness and lifecycle completion gate:

`out/validation/animation/us14-complete/live-20260726Tnative5-t271/`

The accepted run passed its integrated build, 45.757-second focused
non-performance test suite, required install, all 107 advanced scenario
operations, six pixel-distinct gate-owned captures with 18 successful native
receipts, exact import/reimport/motion-database/recording approvals, successful
and canceled recording jobs, Scene/Game/custom cameras, Play restoration, MCP
reconnect, Vulkan inspection, and evidence locking. The merged scenario also
received one strict-schema repair: its ragdoll blend-weight expectation now uses
JSON number `1.0`, matching the standalone US11 manifest and runtime result.
The acceptance transaction committed the whole-system correctness rows while
leaving future/performance-dependent rows explicitly blocked.

The sequence then completed T282 against the user's acquired Khronos Fox project,
whose `New Animated Project` name contains spaces. The headless exporter now
accepts the editor's canonical `game.json` manifest as well as the legacy
`project.json` name. The source-free TEPAK launcher completed two exact 120-tick
lanes: rendered Hybrid Authoritative mode and headless CPU
Deterministic/Fallback mode. Every animator status, state hash, pose hash, event
count, and root-motion value matched at every tick. The rendered lane returned a
real 1280x720 Vulkan PNG with no deterministic fallback, and visual inspection
confirmed a posed Fox in the captured frame. T282 is accepted from:

`out/validation/animation/final/export-runtime/fox-project-20260726/accepted-parity-comparison.json`

## Executive summary

The animation implementation and its non-performance correctness validation are green. The Spec Kit run is not formally complete because the four remaining tasks require comparable performance measurements or the performance-dependent final acceptance reconciliation.

- Overall task completion: **285 of 289 tasks (98.6%)**.
- Remaining task IDs: **T283, T284, T288, and T289**.
- Final exact-tree correctness: **49 of 49 non-performance CTest registrations passed** in 58.93 seconds after the packaged-runtime and cross-model repairs.
- Required install: passed after the latest code changes; the managed rebuild reported zero warnings and zero errors, the dependency install tree exists, and the staged editor/engine/launcher/compiler/animation-tool artifacts are present.
- Fresh acceptance-eligible native runs now complete T098, T134, T148, T163, T198, T237, and T270.
- T180 is accepted from a complete native-input run with two separately approved motion-database builds, cancellation/retry, 42 operations, Play restoration, three cameras, reconnect, evidence locking, and acceptance-matrix commit.
- Approval-protected T253 and T271 are accepted through fresh production UI approval runs.
- A fresh real-model run exercised four downloaded animated model sets: Khronos AnimatedMorphCube, Khronos RiggedFigure, Khronos Fox, and the Quaternius Ultimate Animated Character Pack.
- T281 is accepted from the complete installed-editor real-model run with all protected production approvals and manual evidence.
- A second installed-editor model-corpus run independently reimported all four sets with stable asset/subasset identities and passed clip-preview playback controls. It exercised morph animation, skeletal skinning, multiple clips, glTF, and a 17-clip FBX character pack.
- The user's project with spaces in its name now exports successfully to a source-free TEPAK. Two packaged-runtime captures show visibly different Fox poses, proving that animation advances in the exported executable.
- T282 is accepted from exact rendered/headless 120-tick authoritative parity plus a non-vacuous real Vulkan capture from that source-free package.
- T285 is complete under the user's explicit interruption disposition after 46,215.88 clean runtime seconds and 77 iterations, with zero failures and bounded resource observations.
- T283 and T284 were not run, per explicit user instruction, because the host power limit makes performance-regression measurements inaccurate. They remain open.

## Phase ledger

| Phase | Task range | Complete | Status |
| --- | ---: | ---: | --- |
| 1. Setup and locked baseline | T001-T008 | 8/8 | Complete |
| 2. Foundational animation infrastructure | T009-T024 | 16/16 | Complete |
| 3. Import complete animated characters | T025-T041 | 17/17 | Complete |
| 4. Play and blend animation reliably | T042-T058 | 17/17 | Complete |
| 5. Author advanced animation logic | T059-T074 | 16/16 | Complete |
| 6. Evaluate and render deformation on the GPU | T075-T098 | 24/24 | Complete; T098 installed native Vulkan gate accepted |
| 7. Control animation from gameplay | T099-T116 | 18/18 | Complete |
| 8. Scale and fail safely foundation | T117-T134 | 18/18 | Complete; T134 installed cumulative-scale gate accepted |
| 9. Procedural animation and IK | T135-T148 | 14/14 | Complete; T148 installed procedural/IK gate accepted |
| 10. Retarget animation between rigs | T149-T163 | 15/15 | Complete; T163 installed retargeting gate accepted |
| 11. Motion matching | T164-T180 | 17/17 | Complete; T180 installed native approval gate accepted |
| 12. Animation/ragdoll physics blending | T181-T198 | 18/18 | Complete; T198 installed animation/ragdoll gate accepted |
| 13. Editor authoring, preview, recording, and diagnostics | T199-T220 | 22/22 | Complete; T220 installed native approval gate accepted |
| 14. Streaming, reload, packaging, and portability | T221-T237 | 17/17 | Complete; T237 installed export/runtime gate accepted |
| 15. Complete workflow automation | T238-T253 | 16/16 | Complete; T253 installed native approval gate accepted |
| 16. Replication, rollback, and replay | T254-T270 | 17/17 | Complete; T270 installed determinism gate accepted, SC-021 retained for final reconciliation |
| 17. Complete scale and failure acceptance | T271 | 1/1 | Complete; T271 whole-system native approval gate accepted |
| 18. Final integration and release hardening | T272-T289 | 14/18 | T281, T282, and T285 accepted; only the performance-dependent chain remains open |

## Latest implementation repairs

### Final approval retries now execute protected operations

Fresh T220, T253, and T271 retries used the unlocked native desktop and successfully
resolved exact in-process approval prompts instead of timing out. This exposed
several scenario/gate defects that the old pending approvals had hidden:

- The phase gate now supports a bounded signed `integerOffset` for integer
  argument bindings, with overflow and the four-hour tick limit enforced and
  covered by the phase-gate self-test.
- Recording `lastTick` values are derived from the live publication tick rather
  than a stale absolute `3600`. US10, US11, and the mirrored US14 operations use
  bounded one-Hertz/four-hour reservation windows so human approval latency does
  not invalidate the requested range.
- US11/US14 advanced retarget edits now request the valid reviewed five-millimeter
  reachable tolerance instead of exceeding the ten-millimeter asset maximum.
- US11/US14 activity checks now require the production `data.activity` and
  `data.count` contract instead of the obsolete `data.records` field.
- The scenario-wide argument-binding cap is now 160 while the per-operation cap
  remains 16; the previous cap of 64 contradicted the already reviewed 98- and
  130-binding US11/US14 manifests.
- Failed-reimport preservation snapshots now require three consecutive identical
  inventories before the corruption probe. An installed compiler reproduction
  proved both generated-output roots byte-identical; only the expected import
  diagnostic sidecar changed, confirming the previous T271 report was a
  file-watcher race in the gate rather than last-known-good corruption.

`Tools/Animation/TestAnimationPhaseGateVisualEvidence.ps1` passes after these
repairs and validates all 16 installed scenario manifests. T220 subsequently
passed its complete approved recording lifecycle after the monotonic publication,
private staging cleanup, and ordered-dictionary evidence defects were repaired.
T253 subsequently passed the corrected manifest and accepted FR-159 through
FR-167 plus SC-024. T271 still requires its fresh corrected-manifest rerun.

### Packaged animation parity now uses the real runtime contracts

T282 exposed and repaired four integration gaps:

- parity capture now permits a bounded asynchronous startup warm-up and begins
  its requested 120 ticks only after the first complete sealed pose;
- headless validation is explicitly propagated into `AnimationSystem`, so Auto
  selects CPU Deterministic/Fallback rather than Hybrid when no renderer exists;
- packaged rendering publishes the authored runtime camera and can render a
  dedicated Vulkan offscreen target for backend readback without depending on
  editor-only swapchain state;
- the headless exporter accepts the canonical editor `game.json` project
  manifest while retaining legacy `project.json` compatibility.

Focused animation parity, execution-mode, capture-queue, Vulkan-readback, and
headless-export tests passed, including new explicit headless-lifecycle and
canonical-manifest regressions. The required install completed after these code
changes, and the subsequent integrated non-performance suite passed all 49
registrations.

### Component-owned default clips now start automatically

Live inspection of the user's imported Khronos Fox found a valid Animator binding,
25 evaluated joints, a healthy Hybrid Authoritative GPU presentation, and no
fallback or stale work, but its playback clock remained `Stopped`. The runtime
component coordinator configured each new or rebound `AnimatorPlayback` without
ever issuing the initial play transition.

`AnimatorPlaybackBinding` now carries an explicit `playOnBind` intent. Authored
`AnimatorComponent` bindings enable it, while direct/runtime API bindings retain
their existing command-driven stopped default. `AnimationSystem` applies the
play transition atomically at the protected binding boundary only after clip and
skeleton dependencies validate. Component rebinds caused by streaming, reimport,
or compatible hot reload also restart active playback rather than replacing it
with a permanently stopped clock.

`Animation.ComponentBinding.AuthoredComponentCreatesAndRetiresRuntimeInstance`
now proves that the authored default clip is `Playing` and advances beyond tick
zero on its first frame, and that a coherent skeleton/clip reimport preserves the
runtime handle while restarting advancing playback. The focused
`Engine.Animation` CTest registration passed, the integrated RelWithDebInfo
build and required install completed, and the dedicated `Editor.Animation`
registration passed. The live editor currently open on port 8765 is the older
`out/build/RelWithDebInfo` process and must be safely restarted after saving to
load this repaired engine binary before the Fox can be visually rechecked.

The non-performance CTest pass completed 49 of 50 registrations. The sole broad
`Editor.ExportUnitTests` registration failed only its 32 MCP listener/safety
cases because the user's already-running editor owns the production loopback
port; all animation cases within that executable passed. The exact rerun log is
`out/validation/animation/final/tests/autoplay-editor-export-unit-rerun.log`.

### Animator Inspector authoring guidance

The Animator component Inspector no longer exposes persisted reflection names or
numeric enum values as its primary authoring UI. All 16 durable fields now have
plain-language labels with hover tooltips that explain what each field controls,
when to use it, and the recommended default. Execution mode, root motion, update
schedule, time source, animation events, and off-screen behavior use named
drop-down choices with per-choice guidance. The editor-managed serialization
version and currently undefined diagnostic bit field are read-only so creators
are not invited to guess at internal values.

Animation asset references now resolve imported package Skeleton and Animation
Clip child names and query the correct typed ResourceManager publication for
controllers, clips, skeletons, retarget profiles, ragdoll profiles, and motion
databases. Empty references describe their fallback behavior, and the authored
summary displays readable asset names rather than raw UUIDs. Its setup feedback
now correctly states that either a controller or default clip can drive playback
and that a Skeleton Override is optional when the model supplies a compatible
skeleton.

`Editor.Animation.Inspector.AnimatorPropertyGuidanceIsComplete` guards the full
metadata inventory, readable labels, practical instructions, named policy
domains, and read-only internal fields. The focused test passed, the
`TerraEngine-Editor` and `TerraEngine-EditorTests` targets compiled, and the
required RelWithDebInfo install completed successfully after this change. The
final install reported zero managed warnings and zero errors; no production C++
compile or link error occurred.

### Vulkan and GPU animation

The invalid earlier T098 evidence was repaired rather than accepted. Deformable virtual geometry now retains its owner skinning stream, meshes without one receive an explicit stride-correct zero record, reflected binding 6 is written unconditionally, and missing ownership fails closed. GPU morph dispatch ordering now respects runtime resource publication and Vulkan command ordering. The gate also rejects Vulkan validation errors from captured editor logs.

The current T098 automated run exercised a supported GPU/Hybrid path without the former descriptor VUID or a new Vulkan validation failure:

`out/validation/animation/us4-gpu/live-20260725Tgpu-backend-order-f/`

### Pause, procedural evaluation, and retarget completion

Paused Play Mode no longer advances animation unless an explicit step is requested. The scripting time snapshot and animation scheduler now preserve the paused boundary, and a regression test covers the behavior.

Retarget command completion is now validated against the matching `FrameWorkItem::priorPublication`, not a later live publication that can legitimately advance before completion processing. This removed the false stale-result rejection. T163 completed all 26 automated operations with a valid retarget publication:

`out/validation/animation/us7-retarget/live-20260725Tautomated-native-exception-t163c/`

### Ragdoll recovery clock correctness

Ragdoll activation previously stamped its generation with the animation/editor frame counter while physics pose publication used the physics fixed-tick counter. A far-ahead editor frame therefore made every valid Jolt pose appear stale forever.

`PhysicsSystem::CurrentFixedTick()` now exposes the protected physics clock, `AnimationSystem::BuildRagdollActivationPoseLocked` stamps activation from that clock, and internal prior-pose velocity ordering remains on the animation clock. The new regression `Animation.Ragdoll.PhysicsClockStampSurvivesFarAheadEditorFrames` and the existing live-Jolt lifecycle regression both pass.

T198 then completed all 31 automated operations, including recovery with a 0.25-second blend, Play restoration, three camera surfaces, reconnect, and a clean Vulkan log:

`out/validation/animation/us9-ragdoll/live-20260725Tautomated-native-exception-t198f/`

### Fail-closed native-input exception

`RunAnimationPhaseGate.ps1` can continue automated correctness after a native-input exception only when explicitly requested. That mode never converts automation into manual proof: it records `acceptanceEligible=false`, skips acceptance-matrix mutation, and terminates with the exact native-input exception after the automated checks finish.

The complete phase-gate self-test passes all 16 scenario contracts and its approval, binding, camera, cleanup, and fail-closed fixtures. Its native-input probe also correctly reports the currently locked desktop instead of claiming a click.

## Installed live evidence

### Automated scenario portions completed cleanly

These runs completed their phase-specific automated operations, camera checks, Play restoration, reconnect checks, and Vulkan-log checks before the deliberate fail-closed native-input exception. T098 and T134 have since been replaced by complete acceptance-eligible native runs:

- T148: `out/validation/animation/us6-procedural/live-20260725Tautomated-native-exception-t148b/`
- T163: `out/validation/animation/us7-retarget/live-20260725Tautomated-native-exception-t163c/`
- T198: `out/validation/animation/us9-ragdoll/live-20260725Tautomated-native-exception-t198f/`
- T237: `out/validation/animation/us12-export/live-20260725Tautomated-native-exception-t237/`
- T270: `out/validation/animation/us13-determinism/live-20260725Tautomated-native-exception-t270/`

T237 and T270 each completed 15 of 15 scenario operations. T270 also retains the exact 100,000-tick Windows MSVC and Ubuntu 24.04 GCC 14 worker-count evidence: workers 1/4/16/31 match hash `17254665474189528098`. The formal task remains open because its entire installed/headless/export/camera acceptance contract was not committed by an eligible live gate.

### Accepted T180 motion-matching gate

T180 passed in:

`out/validation/animation/us8-motion-matching/live-20260725Tfinal-approval-retry3-t180/`

The run approved `approval-1`/`mcp-9` for the deliberately cancelled database build and `approval-2`/`mcp-16` for the successful retry through Preferences → MCP Bridge → Approvals. It then completed 42 operations, retained the cancelled job, published database generation 1, produced gate-owned real mouse/keyboard receipts at 125% display scaling, restored Play Mode, captured all camera surfaces, reconnected MCP, found no Vulkan error, locked the evidence, and committed the acceptance update.

The successful attempt also exposed and repaired a gate validator defect: Windows PowerShell represents a valid UInt64 scene generation above `Int64.MaxValue` as an exact `Decimal`. The validator now accepts only integral Decimal values in the UInt64 overflow range while retaining rejection for negative, fractional, or out-of-range JSON numbers. The phase-gate self-test now exercises `approval-uint64-generation-accepted`.

The final integrity pass also repaired accumulated mojibake in `acceptance-matrix.md`. The gate now reads the BOM-less file with explicit UTF-8 instead of Windows PowerShell 5.1's legacy ANSI default, and the self-test preserves non-ASCII punctuation across prepare, commit, and rollback.

T180 accepted FR-113 through FR-121 and SC-010. Performance-dependent SC-009 and SC-031 remain `Evidence incomplete` for T283.

### Approval-protected gate outcome

- T220 is accepted in `out/validation/animation/us10-editor/live-20260726Tfinal12-t220/`.
- T253 is accepted in `out/validation/animation/us11-automation/live-20260726Tcancel-fix-t253/`.
- T271 is accepted in `out/validation/animation/us14-complete/live-20260726Tnative5-t271/`.

The bridge intentionally resolves these approvals only from the in-process Preferences UI. There is no external MCP approval-write endpoint. T180, T220, T253, T271, and the final T281 run all completed their required approvals through that production UI path.

## Real-model and export state

The fresh acquired-model automation result is:

`out/validation/animation/final/real-models/20260725T134349833Z/result.json`

That historical run exited successfully with the honest status
`partial-automation-only`. Installed-editor import/reimport, exact preview-play
sequences, diagnostics, Manual-policy Play, camera checks, and provenance
validation ran against all four model sets. Its then-missing production
animation-window authoring, advanced operations, recording approvals, and real
input were completed by the later accepted T281 run at
`out/validation/animation/final/real-models/20260726T023029449Z/`.

The independent installed-editor corpus rerun is:

`out/validation/animation-results-other-models/20260725T194937769Z/result.json`

Its machine-validation results are:

- Khronos AnimatedMorphCube: import/reimport succeeded, asset and subasset IDs remained stable, generation advanced, the `Square` clip passed open/loop/play/tick/pause/seek/resume/speed/reverse/stop controls, and animation/material/morph outputs were verified.
- Khronos RiggedFigure: import/reimport succeeded with stable identities and an advanced generation; its generated `Animation_42679ba2` clip passed the same playback sequence and the skeleton/skin/material outputs were verified.
- Khronos Fox: import/reimport succeeded with stable identities and an advanced generation; the generated package contains three clips and both `Walk` and `Run` passed the complete playback-control sequence.
- Quaternius Ultimate Animated Character Pack: import/reimport completed with non-fatal importer warnings, stable identities, and an advanced generation; 17 FBX animation clips were cooked and `CharacterArmature|Victory` passed the complete playback-control sequence.

The result remains honestly labelled `partial-automation-only` because that harness intentionally did not perform protected production approvals, manual window authoring, or export. Those omissions are not model import/playback failures.

The user's live project, `C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project`, was exported with profile `Animation Export Validation` after repairing three export-validation defects:

- generated cooked payload hashes are now checked against the importer hash contract instead of the TEPAK record-hash contract;
- generated prefab manifest hashes are not compared after the export pipeline intentionally rewrites script binding names to runtime IDs;
- optional empty path strings no longer fail deep TEPAK forbidden-reference validation.

The successful export correlation is `spec043-visible-export-20260726`. The output is:

`C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project\Builds\Animation Export Validation`

The packaged executable booted from `export-archive`, mounted the startup scene, loaded the Fox mesh, skeleton, `Run` clip, and animation shaders from the TEPAK, and initialized both animation systems. The two captures:

- `out/validation/animation/final/export-runtime/fox-visible-a.png`
- `out/validation/animation/final/export-runtime/fox-visible-b.png`

have different SHA-256 hashes and visibly different leg, head, and body poses 300 ms apart. This is direct rendered evidence that animation advances in an exported build. The sample project's Main Camera was set to position `[0.0, 0.5, 4.0]` so the model is useful for runtime inspection.

T282 is now complete. A fresh headless export of the same acquired-model project
used the canonical editor `game.json`, profile `d1fa3cfe`, and run ID
`t282-fox-accepted-final-20260726`. The resulting source-free package contains
only the launcher, TEPAK, bundled native DLLs, and NativeAOT managed assembly.
The installed launcher passed bootstrap, rendered-runtime, and genuine
`--headless-cpu-fallback` execution without source, editor, or build-tree
dependencies.

The accepted comparison at
`out/validation/animation/final/export-runtime/fox-project-20260726/accepted-parity-comparison.json`
proves 120/120 fixed ticks for one stable animator with exact equality across
status, state hash, pose hash, event count, and root motion. The rendered lane
used effective mode 1 and returned a real Vulkan backend readback; the headless
lane used effective mode 2. The decoded 1280x720 PNG has 921,600 visible
non-black pixels, is non-uniform, and visibly contains the posed Fox. The runtime
generated `Settings/RenderSettings.json` only after launch; that mutable runtime
state is preserved separately under `runtime-generated-state/accepted/`, outside
the pristine package inventory.

## Authoritative integrated validation

- Full exact-tree build: `cmake --build out/build/Spec043PhaseGate --config RelWithDebInfo --parallel 2` passed after the latest production fixes.
- Focused editor animation suite: `ctest --test-dir out/build/Spec043PhaseGate -C RelWithDebInfo --output-on-failure -R '^Editor\.Animation$'` passed in **4.03 seconds**, including all six `Animation.Export.*` cases.
- Final correctness suite: `ctest --test-dir out/build/Spec043PhaseGate -C RelWithDebInfo --output-on-failure -E '(Performance|Soak)'` passed **49/49** in **58.93 seconds**. The only excluded registrations were the explicitly named performance and soak cases required by the user's power-limit instruction.
- Final suite log: `out/validation/spec043-nonperformance-ctest.stdout.log`.
- Required install: `cmake --build out/build/Spec043PhaseGate --config RelWithDebInfo --target install --parallel 2` passed again after the gate fixes in 10.6 seconds. Managed compilation reported zero warnings and zero errors.
- Install verification: `out/build/Spec043PhaseGate/install/` contains the bundled dependency install tree; `out/build/Spec043PhaseGate/Build/` contains the current staged editor, engine, launcher, compilers, and `TerraEngineAnimationSoakRunner.exe`.
- Phase-gate self-test: the current run passes all scenario/fail-closed fixtures plus the new unsigned-64-bit approval-generation and BOM-less UTF-8 acceptance-matrix regressions.
- Accepted T180 live gate: `out/validation/animation/us8-motion-matching/live-20260725Tfinal-approval-retry3-t180/result.json` passed with baseline unchanged, clean build/tests/install, two approved requests, native input, cameras, reconnect, evidence lock, and committed acceptance receipt.
- Accepted T253 live gate: `out/validation/animation/us11-automation/live-20260726Tcancel-fix-t253/result.json` passed with baseline unchanged, clean build/tests/install, six approved requests, 75 operations, native input, Play restoration, cameras, reconnect, evidence lock, and committed acceptance receipt.
- Accepted T098 live gate: `out/validation/animation/us4-gpu/live-20260726Tnative-t098/result.json` passed with baseline unchanged, clean build/tests/install, 13 gate-owned native inputs, 46 operations, cameras, Play restoration, reconnect, evidence lock, and committed acceptance receipt.
- Accepted T134 live gate: `out/validation/animation/us14-scale/live-20260726Tnative5-t134/result.json` passed with baseline unchanged, clean build/tests/install, 15 gate-owned native inputs, 42 operations, cameras, Play restoration, reconnect, evidence lock, and committed acceptance receipt.
- T285 disposition: `out/validation/animation/final/soak/t285-final-20260725a/interruption-disposition.json` records the user-authorized clean interrupted result.

No performance-regression workload was executed during this final validation.

### Accepted T220 installed-editor recording gate

T220 passed in:

`out/validation/animation/us10-editor/live-20260726Tfinal12-t220/`

The run completed its integrated build, focused non-performance tests, required
install, gate-owned real mouse/keyboard actions, two exact in-process approvals,
Play Mode restoration, camera/profiler/overlay checks, evidence lock, and
acceptance transaction. The terminal acceptance receipt is:

`out/validation/animation/us10-editor/live-20260726Tfinal12-t220-acceptance-commit.json`

The live retries found and repaired two production recording defects:

- paused/editor republishing may repeat a fixed tick or advance to a later tick
  while retaining the same immutable pose generation; only a true tick or
  generation regression now invalidates the recording session;
- the default private `.terraengine-animation-recordings` root is now removed
  non-recursively when its final owned session is cleaned up, so successful or
  cancelled recording does not leave an authored-directory change.

The successful stop published
`Assets/Captured/US10EditorRecording.teanim`, retained the exact approval-time
animator/joint binding, validated the candidate, and left no private staging
directory. FR-135 through FR-153 and SC-023 were accepted. SC-006 remains
intentionally blocked only on the user-deferred timing requirement.

The same run also repaired ordered-dictionary handling in the live recording
output proof so approval completion and operation-artifact provenance are
validated identically during live execution and evidence reconstruction.

## Acceptance and compliance state

The 268 acceptance rows currently parse as:

- 249 `Accepted`
- 1 `In progress`
- 18 `Evidence incomplete`
- 0 `Planned`

The acceptance-matrix header and final reconciliation have been synchronized to
those row values. The remaining rows depend on the intentionally deferred
performance reports; no row was promoted from incomparable timing or from an
acceptance-ineligible run.

Repository-level implementation compliance remains green for C++20, bundled dependencies, shader separation, CRTP components, `SystemsBase`, scheduler declarations, transactional mutation/rollback coverage, documentation, clean reproduction, and install production.

Post-acceptance Inspector playback enhancement: the Animator Basic tab now
authors a fixed-60-Hz Default Clip Start Frame as clamped `current/max` and a
Play Automatically switch that holds the initial pose until explicit playback
when disabled. Start frame and Loop Default Clip apply to direct clips and the
resolved controller entry clip; Inspector range display follows that same
resolution. Schema-v3 migration, generated prefabs, runtime rebinding, Play
Mode restoration, exports, and C# properties preserve the authored choices.
Focused and full non-performance animation validation, the 49/49 broader
non-performance registration surface, both required installs, exact-project
Fox loopback, and fresh source-free rendered/headless parity are green. This
enhancement does not alter the 285/289 task count or the deferred performance
chain.

### Distinct looping start-frame endpoint

A follow-up live report showed that selecting the displayed Fox maximum
`42/42` still appeared to wrap to frame zero. Physical reproduction proved that
the ImGui slider itself was already saturating correctly: over-dragging its
right edge and repeatedly pressing Right left the reflected component value at
42. Runtime inspection exposed the real boundary defect. Looping clip time is
resolved modulo the clip duration, so duration tick 42 is not a distinct final
frame; it is the next loop's frame zero.

The runtime and Inspector now use one shared authored-start clamp:

- looping clips expose distinct ticks `0..duration-1`;
- one-shot clips expose the stable endpoint `0..duration`;
- negative, oversized, and zero-duration requests saturate without underflow or
  wrap.

The binding coordinator applies that rule even when components are authored
through C#, MCP, deserialization, or another non-Inspector path. The Inspector
uses the same rule for its editable `current/max` control and read-only authored
summary, and its tooltip now explains why looping and one-shot maxima differ.

Focused regressions pass for the shared runtime clamp and Inspector range
model. The exact integrated RelWithDebInfo build and mandatory install pass,
and the complete non-performance CTest surface passes **49/49** in **62.64
seconds**. No performance or soak registration ran.

The final live check used only:

`C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project`

With Loop Default Clip enabled, the selected Fox was migrated from the aliased
`42/42` to `41/41`. A real mouse over-drag to the screen edge followed by 12
Right-arrow presses left the component at 41. Disabling Loop exposed the valid
one-shot endpoint `42/42` with normalized time 1.0. Restoring Loop normalized
the value back to `41/41`. A subsequent Play/Stop cycle retained one complete
animated render binding in both states with nonzero pose/palette generations
and zero safe fallback, degraded animators, fallback count, stale work, or
rejected batches.

Evidence is under:

- `out/validation/animation/start-frame-distinct-loop-endpoint/focused-engine-test.log`
- `out/validation/animation/start-frame-distinct-loop-endpoint/focused-editor-inspector-test.log`
- `out/validation/animation/start-frame-distinct-loop-endpoint/integrated-build.log`
- `out/validation/animation/start-frame-distinct-loop-endpoint/install.log`
- `out/validation/animation/start-frame-distinct-loop-endpoint/nonperformance-ctest.log`
- `out/validation/animation/start-frame-distinct-loop-endpoint/live-physical-overdrag.json`
- `out/validation/animation/start-frame-distinct-loop-endpoint/live-loop-max-before-overdrag.png`
- `out/validation/animation/start-frame-distinct-loop-endpoint/live-one-shot-max.png`
- `out/validation/animation/start-frame-distinct-loop-endpoint/live-final-loop-max.png`
- `out/validation/animation/start-frame-distinct-loop-endpoint/live-final-state.json`

The exact project is saved with Loop Default Clip and Play Automatically
enabled, Default Clip Start Frame `41/41`, Play Mode stopped, and
`sceneDirty: false`. The rebuilt editor remains open on that project. This
correctness repair does not change the 285/289 task count; only the explicitly
deferred T283/T284/T288/T289 performance chain remains.

### Final post-BaseCharacter-repair integrated evidence

The final repair set was validated again after the BaseCharacter rear-view
check and packaged-runtime pass:

- `TerraEngine-EditorTests.exe` passed **704/704** tests with port 8765 free.
- The complete project-owned non-performance CTest selection passed **47/47**
  registrations in **51.30 seconds**.
- The mandatory RelWithDebInfo install target passed after restoring the
  repository-local managed dependencies; its managed stages reported zero
  warnings and zero errors.
- The fresh source-free 89-entry export passed bootstrap and both rendered and
  forced-headless 120-tick parity lanes. All four owners produced 120 distinct
  state hashes and 120 distinct pose hashes with zero authoritative-field
  mismatches.

The first refreshed CTest attempt encountered the already-understood port-8765
collision because the correct-project editor had been reopened. It affected
only MCP listener ownership in `Editor.ExportUnitTests`; after the clean,
stopped, non-dirty editor released the port, the exact same 47-test selection
passed completely. No product defect remained.

Current evidence:

- `out/validation/animation/import-space-fix/editor-full-suite-port-free.log`
- `out/validation/animation/import-space-fix/nonperformance-ctest-final-port-free.log`
- `out/validation/animation/import-space-fix/install-final.log`
- `out/validation/animation/import-space-fix/live/final/base-play-actual-back-a.png`
- `out/validation/animation/import-space-fix/live/final/base-play-actual-back-b.png`
- `out/validation/spec043-final/spec043-post-backfix-export-20260727/parity-summary.json`
- `out/validation/spec043-final/spec043-post-backfix-export-20260727/rendered-runtime.png`

T281 remains reclosed. The implementation run's complete non-performance scope
is green, and only the intentionally deferred performance-dependent chain
T283/T284/T288/T289 remains open.

## Remaining blockers and resume order

1. Keep T283/T284 open until comparable non-power-limited hardware is available
   and the user authorizes performance regression testing.
2. T288 cannot pass as written while it requires passing, comparable,
   non-deferred T283/T284 reports.
3. T289 must remain open because its contract requires the T283/T284/T288
   performance evidence before closing the 18 performance-dependent rows and
   SC-026.

The current code, installed-editor workflows, real-model validation,
exported-runtime animation, and automated correctness are green. The only
remaining work is the explicitly deferred performance acceptance chain.
