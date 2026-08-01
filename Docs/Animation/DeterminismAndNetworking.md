# Determinism, Networking, and Rollback

Animation authority advances on fixed ticks. A deterministic snapshot is a bounded, copied record of controller, clip, transition, layer, parameter, trigger, event, root-motion, IK, retarget, motion-match, morph, ragdoll, attachment, and physics state. It also carries the animator handle, immutable publication generation, a complete asset/program fingerprint, and frozen worker-count metadata.

No deterministic record contains a GPU address, renderer resource, live component pointer, or physics-world pointer. Rendering is therefore never an authority input. The CPU deterministic evaluator owns semantic results; presentation can consume a matching publication but cannot feed data back into rollback.

## Fingerprints and history

Restore checks the complete fingerprint and publication generation before copying any state. A mismatch returns `StaleHandle`; a checksum mismatch returns `ContentHashMismatch`. Per-animator histories are bounded by the rollback-history limit. The default pressure policy evicts the oldest tick, and an older correction reports `RangeOutOfBounds` rather than being silently approximated. Projects that require strict retention can select reject-new pressure policy.

## Replication ownership

`AnimationReplicationCodec` encodes little-endian, versioned animation records only. The game networking layer owns packet transport, ordering, reliability, authentication, encryption, fragmentation, and connection lifecycle. Animation owns validation, stable ordering, asset/program fingerprints, predicted versus authoritative markers, and copying decoded values to a protected animation boundary.

## Corrections, replay, and events

Rollback first restores a retained snapshot, normalizes inputs by `(tick, producerId, producerSequence)`, rejects conflicting duplicate identities, then resimulates fixed ticks in order. Event watermarks are monotonic: a late join or replay seek resumes after already-delivered one-shot events. Root-motion and attachment/physics effects remain copied correction records, which the owner commits through declared scene and physics transaction boundaries after deterministic evaluation.

## Diagnostics

State and snapshot checksums use stable fixed-width semantic values. When hashes differ, the divergence analyzer reports the earliest animator, tick, category, stable identity, checksums, and expected/actual fingerprints. This keeps retained diagnostics bounded while making graph, asset, pose, event, root-motion, and motion-match mismatches actionable.

## Exact cross-platform correctness lane

The long-stream worker-count matrix is correctness work, not a benchmark. Power
or thermal limits can invalidate performance comparisons, but they do not defer
the fixed-input hash gate. The source fixture is described by
`Tests/Fixtures/Animation/determinism/manifest.json`, and the executable selects
the frozen 100,000-tick profile only when the acceptance environment variable is
present.

On Windows x64, build `TerraEngine-EngineTests` with the supported MSVC
RelWithDebInfo toolchain, then run:

```powershell
$env:TERRAENGINE_TEST_FILTER = 'Animation.CrossPlatformDeterminism'
$env:TERRAENGINE_ANIMATION_DETERMINISM_TICKS = '100000'
out/build/RelWithDebInfo/TerraEngine-EngineTests.exe
```

On Ubuntu 24.04 x86_64, configure explicitly with `gcc-14` and `g++-14`, build
the same target, and run the equivalent command:

```bash
TERRAENGINE_TEST_FILTER=Animation.CrossPlatformDeterminism \
TERRAENGINE_ANIMATION_DETERMINISM_TICKS=100000 \
./TerraEngine-EngineTests
```

Each real platform process reports only the acceptance lanes it actually
executes. Every worker count must match the source-controlled MSVC golden hash;
an unrecognized tick-count value fails closed instead of silently running the
short developer profile. Release validation additionally requires the installed editor/MCP,
exported runtime, correction, late-join, replay-seek, and camera-continuity
evidence, so this native matrix is necessary but is not the whole live gate.
