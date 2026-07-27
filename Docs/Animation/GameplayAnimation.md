# Gameplay Animation Boundaries

This guide describes when native and C# gameplay animation work becomes visible
and how to keep scripts correct across asynchronous evaluation, reload, and
provider differences. The API reference and complete examples are in
`Docs/Animation/CSharpAnimationAPI.md`.

## One frame of gameplay control

1. A gameplay callback submits a copied command. Validation either returns a
   rejection or places the complete command in the bounded animation queue.
2. The animation scheduler merges producers by target tick, producer identity,
   producer sequence, and correlation metadata. Worker jobs consume immutable
   scene/resource inputs and build owned pose results.
3. The protected animation commit publishes only a complete generation.
4. The protected scripting phase copies the published event batch before
   invoking managed subscribers. It does not expose the live pose, queue, or
   native container.
5. Later reads return the immutable state and array publications with source
   tick, observation tick/age, and generation metadata.

`Queued` therefore means “accepted for a later boundary.” It never promises
same-callback state, transform, physics, attachment, or render visibility.
Reflected `Animator` property setters use the same deferred component-mutation
rule as other managed components.

## Choosing an execution mode

Every authored animator requests `AnimationExecutionPolicy.Auto` by default.
The runtime may select GPU visual, Hybrid authoritative, or CPU deterministic
fallback according to active consumers and platform capability. Gameplay must
read `RequestedExecutionPolicy` and `EffectiveExecutionMode` separately; a
manual request is not proof that its requested mode was usable.

Use `QueryCapability` before optional retargeting, IK, motion matching,
root-motion, ragdoll, bone, socket, or event work. The returned status,
`Supported` value, and `AnimationCapabilityReason` flags describe the current
animator and provider. A stable method name is intentionally present even when
the current provider returns `Unsupported`, allowing scripts to preserve one
code path across configurations without pretending the work ran.

## Reading without races

`AnimatorStateSnapshot` is one value copy. It includes active controller, clip,
and skeleton UUID/generation pairs, policy values, counts, playback state,
`Tick`, `ObservationTick`, `AgeTicks`, and `PublicationGeneration`. Array reads
copy a complete parameter, curve, morph, bone, socket, event, or root-motion
publication through a count probe followed by an exact bounded copy.

Treat the generation as part of identity. Do not combine a bone transform from
one publication with a skeleton or pose from another. A `NotReady` result has no
usable semantic payload, while `StaleHandle` requires acquiring a new
`Animator` wrapper from the current object/component instance.

## Events and rollback

Event identity carries animator generation, controller and clip generations,
controller/source state, event ID, source tick, loop count, layer, direction,
historical flag, producer order, correlation ID, and publication generation.
Forward, reverse, loop-boundary, and historical events therefore retain enough
information for gameplay to de-duplicate or reconcile them.

Managed subscription delivery is deterministic and bounded. Filters compare
tag text with ordinal semantics. A throwing delegate is contained and reported
without aborting unrelated delivery. Queue overflow is explicit through the
overflow flag and a dropped-count lower bound; scripts must not assume silence
means that no event occurred after an overflow diagnostic.

Dispose subscription tokens in `OnDestroy`. Gameplay reload and Play Mode stop
also invalidate all tokens, but explicit disposal makes object lifetime clear
and avoids callbacks after an individual script no longer needs them.

## Root motion and attachments

Root-motion reads report the requested and actually applied translation/yaw,
source interval, disposition, correlation, and publication generation. Check
`Applied`, `Adjusted`, or `Rejected`; never infer movement success from command
acceptance. Scene, character, and physics writes become visible only through
their typed deferred/transactional flush boundaries.

Bone and socket reads are copied snapshots with stable IDs and pose generation.
They may return `NotReady` before the first complete pose and `StaleHandle`
after component/object replacement. Game code that drives an attachment should
enqueue its transform mutation rather than retaining the snapshot or mutating
live scene storage from a worker callback.

## Authored references and reload

The current C# ABI reads active controller, clip, and skeleton references from
`ReadState()`. It deliberately has no reflected C# setter for their 128-bit
authored UUID properties. Bind or replace those references through the
Inspector, serialized scene/prefab data, or typed editor automation, where the
whole authoring transaction can be validated and preserved atomically.

Asset reload changes generation. Script assembly reload invalidates wrappers
and subscriptions. Scene unload, object-slot reuse, and component replacement
also invalidate the old generation. Never cache an `Animator`, asset reference,
bone/socket result, or event token across those boundaries without reacquiring
and validating it.

## Export status

Animation C# sources are part of the managed core source glob and are published
and installed with `TerraEngine.CSharp.dll`, so copied commands and polling reads
use the same ABI in hosted and NativeAOT runtimes. Managed vtable v3 appends an
animation ABI query and event-batch callback after the pinned v1/v2 prefixes.
The engine checks both advertised version and table byte size before reading the
tail, then checks `AnimationAbi.Version` before copying a packet. Current
exports therefore use the same `AnimationEvents` filtering, ordering, disposal,
overflow, reload-safe token, and callback-containment implementation as the
hosted editor runtime. Older v1/v2 NativeAOT libraries still load for binary
compatibility but do not advertise automatic animation-event delivery; rebuild
them with the current managed SDK for full animation parity.

## Validation checklist

- Confirm default `Auto` and the reported effective mode.
- Check immediate rejection, queued receipt, observable tick, cancellation,
  idempotent retry, and duplicate receipt behavior.
- Remove and recreate an animator/object and prove the old wrapper returns
  `StaleHandle` without affecting the new instance.
- Reload gameplay code and prove old subscriptions remain inert.
- Exercise forward, reverse, loop, filtered, throwing-callback, and overflow
  event cases.
- Compare native and C# snapshots, events, morphs, curves, bones/sockets, and
  root-motion outcomes by tick and generation.
- Repeat against the installed editor and a v3 exported runtime, including
  automatic subscriptions and explicit `ReadEvents()` polling parity.
