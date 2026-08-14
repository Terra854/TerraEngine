# Animation Troubleshooting

Use this guide after reproducing a problem with a `RelWithDebInfo` installed tree. Animation failures are generation-safe by design: a rejected packet should leave the previous complete pose or asset generation active. Treat a partial visual result, stale mutation, or silent fallback as a defect rather than repairing live storage by hand.

## Start with the published status

1. Open the Animation Diagnostics window or request `animation.runtime.diagnostics` through the editor automation bridge.
2. Record the animator handle, component generation, asset generations, fixed tick, publication generation, execution mode, and correlation ID.
3. Inspect the first non-`Ok` stage. A later fallback status is usually a consequence, not the root cause.
4. Preserve the editor log, copied diagnostics, and the Scene/Game camera captures from the same publication generation.

The common status meanings are:

| Status | Meaning | First checks |
| --- | --- | --- |
| `NotReady` | A bounded asynchronous result is not published yet. | Poll on a later update; do not block a frame or read worker-owned memory. |
| `StaleHandle` | An object, component, asset, scene, physics-world, or publication generation changed. | Re-resolve the authored object and reacquire copied handles after reload or Play Mode. |
| `IncompatibleAsset` | Skeleton, controller, clip, retarget, motion database, mesh, or ragdoll fingerprints do not agree. | Validate and recook the complete dependency set; do not override the fingerprint. |
| `HardLimitExceeded` | Input cannot be reduced safely within a declared hard limit. | Inspect counts in the asset diagnostic and reduce source complexity deliberately. |
| `QueueFull` | A bounded command, event, job, or diagnostic queue rejected new work. | Drain copied receipts, reduce producer rate, or increase an authored soft budget within the documented hard maximum. |
| `Canceled` | The owning generation or explicit token canceled work. | Submit again only against the current generation. |
| `Rejected` | Validation or an atomic publication boundary rejected the candidate. | Inspect the correlated diagnostic; the previous complete generation should remain active. |

## Asset import and hot reload

- Run validation independently of playback. The diagnostic must identify the source package and responsible node, joint, channel, or target.
- Confirm that generated `.teskel`, `.teanim`, `.teanimctrl`, `.teanimmask`, `.teretarget`, `.temotiondb`, and `.teragdoll` files exist and carry the expected project identity.
- On a failed reimport, verify that the prior generated package remains byte-for-byte available and active. A mix of old and new subassets is never a valid recovery state.
- A source move or rename should preserve semantic subasset IDs. If references break, compare the package manifest before and after the operation.

## Pose, graph, and gameplay symptoms

- A reference-pose fallback usually means a dependency, generation, or finite-value check failed. Inspect the evaluation stage before changing execution mode.
- A command receipt of `Queued` is not completion. Read its terminal receipt after the declared animation publication boundary.
- Triggers are one-shot typed inputs. Reusing a producer sequence or correlation ID returns the original receipt and must not consume the trigger twice.
- Managed wrappers become stale across domain reload. Dispose event subscriptions and reacquire `Animator`, asset, bone, and socket handles.
- Root motion is a request to a typed scene, character, physics, extracted-only, ignored, or deterministic-network consumer. Inspect the applied/adjusted/rejected acknowledgement before comparing transforms.

## Procedural, retarget, motion matching, and ragdoll

- Procedural goals must reference the current immutable query snapshot and fixed tick. Stale scene or physics queries contribute zero and publish a diagnostic.
- Retarget profiles must match both source and target fingerprints. Constraint goals are timed by the sampled source-pose tick, which may differ from the scheduler frame tick when playback loops or changes speed.
- A motion query may complete asynchronously, but authoritative selection always uses the deterministic CPU selector over the same quantized candidates. Inspect exclusions, per-feature scores, database generation, and fallback reason.
- Ragdoll activation requires one complete animation pose and matching skeleton/profile/physics-world generations. Recovery remains blocked while pose error, velocity, contacts, or available space violate the authored policy.

## GPU, renderer, and bounds

- `Auto` may fall back to CPU for capability, residency, visibility, budget, device-loss, or authoritative-consumer reasons. Force modes still fail safely; they do not bypass validation.
- The renderer consumes immutable pose generations. A complete-pose GPU-to-CPU readback or a CPU wait for a visual GPU pose is a contract violation.
- Invalid animated bounds must fail open to visibility. If geometry disappears, capture the pose, bounds, view, residency, and renderer generations together.
- After device loss, verify that CPU authority continues, stale GPU completions are rejected, and retired resources are released only after their fence generation.

## Reproducible validation

Configure, build, test, and install from the repository root:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

Use the asset-pipeline CTest coverage for model import validation,
`Tests/Tools/Animation/RunAnimationSoak.ps1` for long-running stability, and the CTest
workflow in `Docs/Testing.md` for native correctness. Never mark a manual click,
camera capture, performance result, cross-platform run, or soak as passed when
its evidence is unavailable.
