# Procedural animation and inverse kinematics

TerraEngine layers procedural goals over an immutable evaluated pose. The CPU
reference path supports two-bone IK, arbitrary-chain IK, aim/look, copy
transform, swing/twist limits, twist distribution, weighted targets, foot
placement, and pelvis compensation. GPU Visual mode can run eligible visual-only
records without transferring a complete pose back to the CPU.

## Composition and ordering

`ProceduralGraphNodeInput` separates authored graph controls from solver controls:

- `layerOrder`, then `nodeOrder`, then `stableId` define deterministic execution.
- `controllerWeight * nodeWeight * layerWeight` is the graph contribution.
- Solver weights still describe the contribution inside one operation or goal.
- An `AnimationMaskAsset` multiplies the affected stable-joint weights. A joint
  omitted from the compiled mask contributes zero.
- Override nodes blend their solved pose over the current complete pose.
- Additive nodes solve from their required `additiveReference`, then apply the
  resulting translation, rotation, and scale delta to the current pose.

Every operation stages a complete `GraphPoseContribution`. The evaluator replaces
the caller's output only after all nodes, layers, diagnostics, and requested
snapshots validate. A rejected late goal therefore cannot leave an arm solved and
its hand, contact state, or pelvis from an older generation.

On the GPU, the order is hierarchy propagation, visual procedural adjustment,
optional visual ragdoll blending, morph/bounds work, and palette output. GPU
procedural records that touch the same joint are rejected as one table; lowering
must split such authored dependencies into ordered stages.

## Coordinate spaces

Goals declare one of four spaces:

| Space | Meaning at the graph boundary |
|---|---|
| `Local` | Relative to the target joint's authored local frame. |
| `Parent` | Relative to the copied parent transform supplied with the packet. |
| `Model` | Relative to the animator/model origin. This is the normal IK solver space. |
| `World` | Relative to the captured world frame; graph preparation converts it using copied transforms. |

Worker solvers never query a live hierarchy, scene object, or physics world.
Callers capture the required transforms before scheduling. Copy-transform nodes
perform their declared parent/model/world conversion from those copied values;
two-bone and chain inputs must already be expressed in one consistent solver
space.

Foot placement consumes a `ProceduralQuerySnapshot`. A query hit carries stable
target identity and lifetime generation, point, normal, and optional copied target
transform. Matching identity retains a planted contact; a missing contact releases
it over the configured smoothing interval. Pelvis compensation is clamped and
smoothed independently, so gameplay movement remains authoritative.

## Authority, recording, and rollback

`ProceduralGraphAuthority::CpuAuthoritative` is required whenever procedural
output is consumed by gameplay, physics, networking, recording, rollback, replay,
or deterministic validation. Recording snapshots contain a copied final pose and
ordered node identities. Rollback snapshots additionally retain stateful foot
contacts and pelvis offset. Restore validates the complete generation stamp and
skeleton compatibility fingerprint before replacing either pose or state.

`ProceduralGraphAuthority::VisualOnly` is a reference mode for renderer output.
It cannot satisfy an authoritative consumer or create recording/rollback state.
The compute shader likewise passes through an existing CPU-authoritative or
deterministic model pose. A valid GPU Visual record table is bounded, carries
component/program/skeleton generations, performs no pose readback, and preserves
the hierarchy result if the table is stale, malformed, non-finite, or conflicting.

## Limits and failure behavior

Version-one limits are queryable through `GetAnimationRuntimeLimit`:

- 256 procedural goals per animator;
- 128 chain-IK iterations per request (32 by default);
- 4,096 joints in a skeleton;
- 36,000 retained detailed diagnostic records;
- 64 compact visual procedural records in one GPU table.

Two-bone reach tolerance and chain convergence tolerance are caller supplied and
must be finite and non-negative. The common positional default is 10 mm. Solvers
return finite publishable states for expected geometric cases:

- `Converged` or `Solved`: a usable result was produced;
- `Clamped`: a reach, angle, stretch, joint, or pelvis limit was applied;
- `Unreachable`: the chain used its stable maximum/minimum-reach result;
- `Singular`: a zero-length or ambiguous configuration kept a deterministic
  bounded result;
- `MaxIterations`: chain output is the last finite bounded iteration;
- `NoContribution`: weights or masks reduced the operation to zero;
- `StaleInput`: identity/generation validation failed and the pose was preserved.

Malformed arguments, hard-limit violations, incompatible assets, stale handles,
and allocation failure return an `AnimationStatus` and leave the prior complete
publication unchanged. No solver publishes NaN or infinity. Conflicting goals use
priority first, then stable producer order and goal identity; equal highest-priority
goals normalize their weights, while lower-priority goals remain visible in
diagnostics with zero final contribution.

## Diagnostics

Each copied `ProceduralDiagnosticRecord` reports the requested and observed
generation, solver and state, status, goal/producer identity, positional and
angular target error, input weight, final post-graph contribution, iteration
count/limit, convergence, clamping, singularity, unreachable handling, and stale
input. `ProceduralDiagnosticsBuffer` publishes whole batches into bounded retained
history and records high-water, eviction, publication, and rejection counters.

## Reproducible validation

From a Visual Studio developer environment:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests
$env:TERRAENGINE_TEST_FILTER = 'Animation.Procedural'
& out/build/RelWithDebInfo/Tests/Engine/TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
cmake --build --preset RelWithDebInfo --target install
```

The focused suite covers reachable/unreachable and singular two-bone cases,
bounded chain convergence, mirrored and zero-length chains, conflicting goals,
aim/look clamps, query staleness, contact locking/release, pelvis compensation,
graph order/masks/additive composition, CPU authority, recording/rollback, and
10,000-repeat deterministic fixtures. See `Docs/Testing.md` for the complete CTest
workflow and installed-tree gates.
