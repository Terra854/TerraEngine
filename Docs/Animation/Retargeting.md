# Animation retargeting

Retargeting transfers a clip or pose from a source skeleton to a target
skeleton through a versioned `RetargetProfileAsset`. The profile is an
authored contract, not a name-matching hint: it records source and target
skeleton fingerprints, compatibility generation, semantic rig family, and
the mapping and tolerances used for every published evaluation.

## Profile schema and mapping

A profile contains:

- required and optional joints, semantic labels, and explicit source-to-target
  joint or chain mappings;
- root and orientation bases, unit/scale policy, rest/reference-pose policy,
  and handedness conversion;
- contact goals (feet, hands, or authored markers), facing and timing policy,
  twist-joint distribution, and optional-limb behavior;
- socket and bone identity remapping, morph/curve policy, root-motion policy,
  validation tolerances, quality metadata, and a fallback chain.

Semantic mapping is attempted first, then explicit joint/chain entries resolve
known exceptions. A mapping must be acyclic and must not assign a target joint
to incompatible or duplicated required roles. Optional source limbs may map to
an authored fallback or be omitted; a missing required joint, ambiguous role,
duplicate assignment, cyclic chain, incompatible fingerprint, or unsupported
mapping rejects compilation before publication.

The editor's Retarget Editor presents source and target hierarchies side by
side, auto-map suggestions, manual corrections, chain/twist controls,
contacts, and quantitative error summaries. Mapping edits are transactional;
the observed generation is rechecked at the protected publication boundary.

## Evaluation and execution modes

Retarget evaluation is deterministic local/model-space work over immutable
pose snapshots. Proportions and rest poses are reconciled using the profile's
scale and orientation bases, while chain solving preserves declared timing,
facing, contacts, and reachable end-effectors. Retargeted poses can then feed
blends, layers, masks, morphs, IK, motion matching, root motion, ragdolls,
recording, and deterministic snapshots.

`Auto` remains the default requested execution policy. The resolver selects:

- **GPU Visual** when the retarget result is visual-only; GPU work may adjust
  the complete visual pose, but it must not become gameplay authority.
- **Hybrid Authoritative** when events, root motion, gameplay curves, selected
  joints/sockets, physics, recording, networking, or rollback consume a
  bounded CPU-authoritative subset while the GPU evaluates the visual pose.
- **CPU Deterministic/Fallback** for fixed-tick deterministic simulation,
  headless or validation runs, unsupported capabilities, renderer loss, stale
  work, or a rejected/unsafe GPU request.

Visual-only IK and procedural retarget adjustments may remain GPU-resident.
Gameplay-, physics-, network-, recording-, and rollback-authoritative results
must be evaluated or retained on the CPU; a full GPU pose readback is not a
substitute for the Hybrid subset. Mode changes occur atomically at an
animation boundary with state time, transitions, event cursors, root motion,
and visible continuity preserved.

## Sockets, morphs, curves, and root motion

Sockets retain stable semantic identity across compatible source reimport,
even if display names change. Runtime socket and bone-space queries resolve
against the source, target, retargeted, or ragdoll generation selected by the
caller and reject stale generations before reading.

Morph channels and custom curves are copied only when their profile policy
declares a compatible identity or explicit conversion. Gameplay curves belong
to the Hybrid CPU closure; visual morph weights and deformation may remain on
the GPU. Root motion is extracted in source space, converted by the profile's
root basis and scale policy, and emitted as a generation-labeled root-motion
packet. Character, scene, physics, network, and rollback owners apply that
packet through their declared transactional/deferred boundary; retargeting
never edits live hierarchy or physics state from worker execution.

## Compatibility generations and safe fallback

Published profiles include source/target fingerprints, profile version, and a
complete compatibility generation. Reimport, profile edits, skeleton changes,
or hot reload compile a complete candidate before swapping it. A compatible
candidate replaces the active generation atomically; malformed, stale,
incompatible, cancelled, or unsupported candidates leave the last-known-good
profile and dependent runtime generation active and expose the rejection
reason. In-flight work carrying another generation is discarded.

If a required mapping or resource becomes unavailable at runtime, the system
does not publish a partial pose. It retains the previous valid generation when
possible, otherwise uses the declared bind/reference-pose or CPU deterministic
fallback and reports a structured diagnostic. Force policies are advanced
overrides and may be rejected when they would violate an authority or
capability requirement.

## Validation metrics and limits

Validation reports missing/ambiguous/cyclic/duplicate/incompatible mappings,
non-finite transforms, source/target fingerprint mismatch, and per-channel
quality metrics. Required metrics include contact position error, facing error,
timing/phase drift, root displacement error, reachable end-effector error,
twist distribution, scale residual, and fallback decisions. Diagnostics carry
the profile and asset generations, mapping identity, sample/tick, and expected
versus measured values.

Version-1 acceptance permits at most **20 mm** retarget contact-position error;
profiles may tighten but never loosen that limit. Reachable IK end-effectors
use the **10 mm** default limit, also allowing reviewed per-profile tightening.
Discrete state, event identity/order, root-motion interval identity, and
generation identity require exact equality. Non-finite or unstable output is a
validation failure even when a positional tolerance would otherwise pass.

The validation fixture and per-profile manifest are the source of truth for
supported proportions, rest poses, optional limbs, twist chains, and tightened
tolerances. The general limits are frozen in
[`runtime-limits-and-validation.md`](../../specs/043-complete-animation-system/contracts/runtime-limits-and-validation.md).

## Runtime and authoring use

Native and C# callers submit bounded retarget commands that identify the
animator, profile generation, source/target assets, and correlation or
idempotency data as applicable. Results are copied snapshots or structured
status packets; managed callbacks do not imply same-callback live mutation.
Editor preview uses isolated worlds and fixed ticks, supports side-by-side
source/target playback and contact overlays, and can save a mapping only after
validation. Automation follows the same permission, approval, expected
generation, and atomic publication rules described in
[`Automation.md`](Automation.md).

For reproducible checks, run the focused animation validation after the
retarget tests and use the installed-editor Phase 10 gate for interactive
mapping, preview, contact/root metrics, runtime queries, and failed hot reload.
Performance timing is reserved for final Phase 18; intermediate checks prove
correctness, parity, bounds, generation safety, and fallback behavior only.
