# Animation Controllers and Graphs

This document describes the authored animation graph used by TerraEngine. It is
the guide for technical animators and reviewers; it describes durable asset data
and observable evaluation rules, rather than a particular editor layout or
unstable programming API.

## Assets and authored schema

An animation controller is a versioned `.teanimctrl` asset. It contains an
authored graph document and an immutable compiled `EvaluationProgram`. The
document may contain:

- typed parameters and triggers;
- states, ordered transitions, and nested state machines;
- clip, 1D, 2D, nested blend-tree, subgraph, procedural, and IK nodes;
- ordered override or additive layers, masks, reference poses, and root-motion
  contribution rules;
- synchronization groups and named markers;
- subgraph references, variables, bindings, and output declarations.

The controller references compatible skeletons, clips, masks, and optional
retarget or motion-database assets by stable identity and generation. Runtime
handles, current state, transition progress, parameters after input capture,
poses, event watermarks, and scratch buffers are not serialized in the asset or
in a scene. An `AnimatorComponent` stores the authored controller reference and
policy/override fields; its runtime instance stores the current graph state.

### Parameters, triggers, variables, and bindings

Parameters are explicitly typed (`float`, integer, Boolean, trigger, vector,
transform, or tag), with a declared default and validation range or policy.
Triggers are event-like parameters: a transition consumes them according to the
graph's declared single- or multi-consumer policy and reset behavior. A failed
transition, rollback, or seek follows that policy rather than silently
consuming input.

Subgraphs expose named inputs, outputs, parameters, and variables through
bindings. A binding records its declared type and source; compilation rejects
missing, cyclic, ambiguous, or incompatible bindings. Subgraph calls retain
their own local variables while sharing only explicitly declared values. Keep
gameplay values at the controller boundary so a subgraph remains reusable.

## Deterministic composition and evaluation order

The compiled program evaluates in a fixed order. This order is part of the
asset's behavior and is used by CPU reference, preview, deterministic replay,
and GPU lowering:

1. Capture an immutable frame input: fixed tick/delta, component and asset
   generations, parameter writes, triggers, commands, and relevant snapshots.
2. Select states and transitions using the ordered rules below.
3. Advance state/clip cursors and synchronization groups in integer fixed ticks.
4. Evaluate the selected state's motion source (clip, blend tree, subgraph, or
   procedural node), including missing-channel fallback to the compatible
   reference pose or declared default.
5. Evaluate ordered layers and masks, then apply additive/reference-pose rules.
6. Apply declared procedural and IK nodes, morph and curve channels, events, and
   root-motion extraction.
7. Propagate local transforms through the hierarchy and produce the requested
   outputs. Non-finite or stale-generation results are rejected before publish.

Workers produce immutable evaluation packets. Publication occurs only at the
declared animation protected boundary. Scene, object, component, and gameplay
changes requested by evaluation use typed deferred mutation queues; worker code
does not edit live scene storage.

## States, transitions, and interruption

Each state names a motion source and may contain nested state machines. A
transition declares its source and destination, conditions, priority, exit
time, duration, offset, transition curve, interruption source, and whether
normalized or fixed timing is used. Entry, exit, and any-state transitions are
evaluated with the same ordering rules.

At a decision point, eligible transitions are sorted deterministically by:

1. explicit priority (higher priority first);
2. source specificity (a state-specific rule before a broader any-state rule);
3. authored order as the stable final tie-break.

An ambiguous tie that cannot be resolved by these rules is a validation error;
it is never decided by hash-map or thread scheduling order. Conditions are
evaluated from the captured parameter snapshot, so one update cannot observe a
partially written parameter set.

An interruption follows the transition's declared policy. Interrupting a
crossfade captures the current blended pose as the new source, avoiding a jump
back to an old endpoint. Source and destination offsets, normalized/fixed
durations, and transition curves are retained in the runtime snapshot. Reversing
or seeking uses the playback interval rules in [Playback](Playback.md), including
half-open event intervals and exact clip boundaries.

## Blend trees

Blend trees combine compatible evaluated poses with stable normalized weights.
Weights must be finite and non-negative. A zero effective total selects the
declared reference/default pose. Rotation inputs and outputs are normalized and
quaternion signs are aligned before accumulation.

- **1D trees** sort thresholds in authored order and interpolate between the
  neighboring samples, clamping or extrapolating only as explicitly authored.
- **2D trees** use the authored coordinate space and deterministic triangulation
  or cell selection. Equal-distance or boundary cases use stable authored node
  order, not floating-point iteration order.
- **Nested trees** evaluate children first and pass their immutable pose/sample
  results to the parent. A child may be a clip, subgraph, procedural output, or
  another blend tree when skeleton and channel compatibility is proven.

Blend-tree outputs must preserve clip events, curves, morphs, and root-motion
contribution metadata. A node that cannot provide a compatible output is
rejected or selects the graph's documented safe fallback; it is not silently
mixed with another skeleton generation.

## Layers, masks, and reference poses

Layers are evaluated in authored order. An override layer replaces selected
channels according to its effective weight. An additive layer applies a delta
relative to its declared reference pose; it does not treat an absolute pose as
an additive delta. Per-node, per-layer, and controller weights are combined
with finite, normalized rules.

An `.teanimmask` asset stores the compatible skeleton fingerprint, stable joint
or semantic selectors, descendant expansion rules, per-joint weights, and
whether curves or morph channels are included. The compiler expands selectors
into a dense mask table and records the result in the compiled program. Missing
optional joints may resolve through the documented selector fallback; an
ambiguous required selector or incompatible skeleton rejects the mask.

Masked additive actions affect only selected joints/channels. Ancestor closure
needed to produce valid model transforms is included for evaluation, while
unselected authored channels remain unchanged.

## Synchronization groups and markers

A synchronization group names a leader, followers, and named markers. The
leader's fixed-tick phase is authoritative; followers map equivalent markers
and apply the declared correction policy. Marker order and duplicate handling
are deterministic. If a follower lacks a marker, the graph uses its documented
incomplete-marker fallback (for example, phase-preserving normalized time) and
reports the reduced-fidelity diagnostic. It must not invent a phase from wall
clock time.

## Validation and atomic publication

Controller compilation validates graph cycles, state-machine and subgraph call
depth, parameter and binding types, transition ambiguity, clip/skeleton/mask
compatibility, output dependencies, determinism constraints, finite weights,
and every serialized count before allocation or GPU upload. Diagnostics identify
the graph path, node/parameter/transition, reason, and correction where
possible.

The version-1 hard limits are fixed by
`Docs/Animation/README.md`:

| Quantity | Operational default | Hard limit |
|---|---:|---:|
| Controller nodes | 8,192 | 65,536 |
| State-machine nesting depth | 16 | 64 |
| Reusable subgraph call depth | 8 | 32 |
| Ordered controller layers | 16 | 32 |
| Controller parameters | 1,024 | 4,096 |
| Transitions from one state | 256 | 4,096 |

The same contract also limits clip duration/channels/keys, events, procedural
goals, and rollback or diagnostic history used by a controller. Checked 64-bit
arithmetic is required for serialized counts and byte ranges. Exceeding a hard
limit is a structured rejection.

Compilation and publication are transactional. A worker compiles into a new
generation; the protected publisher verifies source/dependency generations and
publishes the complete `EvaluationProgram` atomically. Any invalid, cancelled,
stale, or conflicting result publishes nothing and leaves the last-known-good
program and its references intact. A reload or hot-reload therefore cannot
expose a partially compiled graph.

## Preview, runtime, and authority boundaries

Editor preview and runtime use the same compiled semantics, fixed-tick sampling,
transition ordering, blend rules, masks, synchronization, events, and root
motion. Preview state is editor-owned and isolated: scrubbing or changing a
preview cursor must not serialize runtime state into a scene or source asset.
Leaving preview or Play Mode restores the authored state without transient
parameters, graph state, morphs, root motion, attachment offsets, or ragdoll
transforms leaking into the scene.

`Auto` selects an effective execution mode at a safe frame boundary:

- **GPU Visual** when no consumer needs authoritative CPU pose, events, curves,
  root motion, physics, networking, rollback, attachment, or headless output.
- **Hybrid Authoritative** when CPU needs only a declared subset; CPU evaluates
  that dependency closure while the GPU evaluates the full visual pose.
- **CPU Deterministic/Fallback** for deterministic simulation, rollback,
  headless or validation runs, missing GPU capability, renderer loss, or budget
  failure.

The CPU reference path is the semantic authority. GPU evaluation must match its
discrete state/transition/event/root-motion identities exactly and remain within
the frozen visual tolerances (0.5 mm translation, 0.1 degrees rotation, 0.05%
scale, and 0.0005 absolute morph weight unless a stricter profile applies).
Rendering consumes generation-safe pose bindings; it never reads mutable
animator storage. Physics and gameplay consume typed snapshots or deferred
receipts at their declared scheduler boundaries.

## Reproducible review checklist

When reviewing a controller, confirm that:

1. Every parameter, trigger, binding, state, transition, layer, mask, marker,
   and output has a stable authored identity and declared type/policy.
2. The graph compiles without cycles, ambiguous transitions, incompatible
   generations, non-finite weights, or hard-limit violations.
3. Repeated fixed-tick evaluation produces identical state choices, events,
   root-motion intervals, and checksums on the CPU reference path.
4. Preview and Play Mode leave authored files unchanged except for an explicit
   successful authoring transaction.
5. Failed or cancelled compilation leaves the previous program usable and
   records actionable diagnostics.
6. GPU, hybrid, and CPU fallback paths use the same program semantics and never
   publish a mixed-generation or non-finite pose.

