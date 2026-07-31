# Animation Playback

TerraEngine evaluates animation from integer fixed ticks. Seconds and normalized
time are convenience inputs only; the runtime converts them at a checked API
boundary and keeps ticks as the authoritative identity for samples, events,
loops, and root-motion intervals.

## Playback state

An animator can be stopped, playing, or paused. The supported commands are
play, stop, pause, resume, seek, playback-direction change, loop-policy change,
speed change, and time-policy change. Scaled, unscaled, and fixed time all feed
the same fixed-tick advancement code so their sampling behavior is identical.

- `Play` continues from the current cursor. Call `Seek` first when playback
  should begin at an explicit tick; replaying a completed clip starts from the
  appropriate directional edge.
- `Stop` clears transient traversal state and restores tick zero, which samples
  the authored clip-start pose when dependencies are valid and the safe bind
  pose otherwise. It does not rewrite authored component data.
- `Pause` preserves the exact current tick. `Resume` continues from it without
  redelivering events at the pause boundary.
- `Seek` samples the destination directly. Historical event delivery is an
  explicit policy and is disabled by default, so an ordinary seek does not
  replay every crossed event.
- Reverse playback uses the same half-open interval rules as forward playback,
  with interval endpoints exchanged and deterministic descending event order.
- A non-looping clip holds its exact terminal sample. A looping clip wraps only
  after the terminal boundary has been accounted for.

Speed must be finite. Direction is represented explicitly rather than inferred
from an invalid or non-finite speed. A frame may cross several loop boundaries;
the runtime records every full cycle in the loop counter and processes each
eligible event and root-motion interval exactly once.

## Persistent playback start frame

`AnimatorComponent` exposes **Default Clip Start Frame** beside Default Clip,
Loop Default Clip, and Play Automatically on the Inspector's Basic tab. It is
durable scene/prefab authoring data. Direct playback applies it to the Default
Clip, while an Animation Controller applies it to the resolved entry clip.

The field uses TerraEngine's fixed 60-frame-per-second timeline: frame 30 is
0.5 seconds and frame 60 is 1 second. The Inspector displays the editable value
as `current/max`, obtains max from the currently published direct or controller
entry clip, and prevents current from exceeding that duration. If reimport
shortens the clip, the Inspector normalizes the authored value to the new
maximum; the runtime also clamps independently so scenes edited outside the
Inspector remain safe.

A fresh editor or runtime binding seeks to the configured frame before optional
autoplay. Exiting Play Mode therefore restores the configured pose instead of
retaining the last runtime deformation.

This setting is serialized into scenes, generated prefabs, and exported runtime
content. It is separate from transient `Seek` and `Play` commands: those commands
move the current runtime instance only.

**Loop Default Clip** is also a component-level playback choice. For direct
playback it controls the Default Clip; for a controller it follows the active
state clip. Controller transition exit times and crossfade durations continue
to advance on unwrapped time, so enabling looping does not disable transitions.

## Persistent automatic playback

**Play Automatically** controls whether an authored Animator begins advancing
when the runtime scene enters Play Mode. It defaults to enabled for existing
scenes and newly imported animated prefabs. Disable it when the object should
hold its configured initial pose until gameplay or tooling explicitly calls
`Play`.

Changing Play Automatically while the runtime scene is active rebuilds the
generation-safe binding: disabling returns to the authored initial pose, and
enabling starts again from that pose. This behavior applies to direct clips and
Animation Controllers. Runtime `Play`, Pause, Stop, and Seek commands remain
available regardless of this startup choice.

## Clip sampling

Clip channels cover translation, rotation, scale, morph weights, custom curves,
events, and root motion. Sampling first validates the clip view, fixed tick,
channel index, and required resident segment. A missing channel does not require
an unrelated segment to be resident: it resolves immediately to the compatible
skeleton reference value or the channel's declared default.

Step, linear, and smooth cubic interpolation are supported. Rotation results
are normalized, finite, and sign-adjusted to preserve shortest-path continuity.
Non-finite decoded keys or interpolation results are rejected before pose
publication.

Authored key ticks may repeat but must never decrease. Consecutive keys at the
same tick are validated in authored order and the final authored value wins.
Cooking emits one deterministic codec key for that tick and counts earlier
duplicates as removed keys. Zero-duration clips sample their sole boundary
without division, wrapping, or undefined normalized time.

## Poses, blending, and crossfades

Evaluation writes immutable local-pose, model-pose, morph, curve, event, and
root-motion buffers. Scratch storage is range-owned by the worker evaluating
that animator; publication copies a completed generation at the protected
animation boundary.

Compatible poses use stable normalized weights. Non-finite or negative weights
are rejected. If the effective total is zero, the result is the declared
reference/default pose instead of an all-zero transform. Rotation blending
normalizes inputs and output and aligns quaternion signs before accumulation.

Crossfades support fixed-duration and normalized-duration forms, source and
destination offsets, transition curves, synchronization, and explicit
interruption. Non-looping sources and destinations can hold the exact final
tick. Interrupting a fade captures its current blended pose as the new source,
which prevents a one-frame jump back to either original endpoint.

## Events, curves, morphs, and root motion

Events use the stable identity `(animator, state instance, clip, event, source
tick, loop count)`. Traversal uses half-open tick intervals so a boundary event
is delivered once, including during reverse and multi-loop advancement. Events
are sorted deterministically before publication. Bounded queues report overflow
and never silently grow.

Curves and morph weights are sampled with the same boundary and interpolation
rules as transform channels. Their published snapshots are immutable and reject
non-finite values.

Root motion is extracted over an explicit source tick interval. The result
contains translation, yaw, complete-cycle accumulation, direction, and source
generation. Playback Validation only extracts and publishes this value; gameplay, character,
and physics application use typed deferred queues introduced by later phases.

## Residency, generations, and fallback

Clip segments can be unloaded, requested, resident, evictable, or failed.
Unavailable required data selects the configured bind/reference pose or a
generation-matching hold-last pose. Hold-last is never reused across a mesh,
animator object, component, deformable-payload, skeleton generation, or skeleton
compatibility-fingerprint mismatch. A mixed-generation pose is never published.

The CPU reference path is the semantic authority for headless operation and for
later GPU parity tests. Rendering may consume a generation-safe CPU pose binding
or the safe bind/hold fallback, but it cannot access mutable animator storage.

## Lifecycle and inspection

Serialized `AnimatorComponent` data contains only authored references and
policies. Runtime playback state, poses, event watermarks, loop counters, and
scratch memory live in generation-safe runtime storage. Component removal,
object reuse, scene reload, Play Mode exit, or dependency replacement invalidates
the old generation before queued work can publish.

The Animator Inspector and clip preview expose playback state, fixed/normalized
time, direction, loop policy, effective speed, status, and safe-pose reason.
Preview controls are editor-owned and must leave authored scene state unchanged
when Play Mode or the preview session ends.

## Validation

From a Visual Studio developer PowerShell at the repository root, configure and
exercise the CPU reference path with these reproducible commands:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R Engine.Animation --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

The installed editor is then available at
`out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe`. Launch an
isolated project with `--project <absolute-project-directory>`; do not validate
against a source-tree editor binary because the installed tree is part of the
feature contract. `Docs/Testing.md` describes the full CTest workflow.

Focused playback cases are registered under `Animation.Sampling`,
`Animation.Blend`, `Animation.Event`, `Animation.Snapshot`,
`Animation.RootMotion`, `Animation.Playback`, `Animation.CpuDeterminism`, and
`Animation.RenderFallback`. Intermediate gates run correctness and deterministic
stress only; all measured performance execution remains in animation production validation implementation stage.
