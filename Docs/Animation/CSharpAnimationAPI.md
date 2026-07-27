# C# Animation API

TerraEngine exposes animation to gameplay through `TerraEngine.Animation`.
Managed code receives copied, fixed-width values only: an `Animator` stores a
slot and generation, snapshots own their arrays, and no native pointer survives
a P/Invoke or managed callback. ABI v1 starts every native record with an
eight-byte size/version/kind header and rejects an incompatible record before
reading or changing animation state.

## Find an animator

`Animator` is the managed wrapper for the native CRTP `AnimatorComponent`:

```csharp
using TerraEngine;
using TerraEngine.Animation;

Animator? animator = Object.GetComponent<Animator>();
if (animator == null || animator.BindingStatus != AnimationStatus.Ok)
{
	Log.Error("This object has no ready AnimatorComponent.");
	return;
}
```

The wrapper copies the owner UUID, component generation, and animator handle
when it is bound. It never silently rebinds after a component is removed or an
object slot is reused. A later command or read then returns `StaleHandle`.
Acquire a new wrapper after a scene or component replacement.

The authored component properties `DefaultClipStartFrame`, `PlayAutomatically`,
`RequestedExecutionPolicy`, `RootMotionPolicy`, `UpdatePolicy`, `TimePolicy`,
`InitialParametersJson`, `EventPolicy`, `CullingPolicy`, and
`DiagnosticOverrides` use the normal reflected component path. Their setters
are deferred when invoked by a gameplay callback; a same-callback getter is not
evidence that the protected mutation has already become visible.

`DefaultClipStartFrame` persists the initial playback cursor in scene and
exported build data. It uses the engine's fixed 60-frame-per-second timeline.
A direct Animator applies it to the Default Clip, while an Animation Controller
applies it to its resolved entry clip:

```csharp
// Start the authored direct or controller-entry clip half a second in the next
// time the protected component mutation rebuilds its runtime binding.
animator.DefaultClipStartFrame = 30;
```

`PlayAutomatically` persists whether the Animator should advance as soon as
Play Mode/runtime startup binds it. Set it to false to hold the configured
initial pose until an explicit `Play` command:

```csharp
animator.DefaultClipStartFrame = 30;
animator.PlayAutomatically = false;

// Later, start from the authored cursor when gameplay is ready.
animator.Play(stateId, animator.DefaultClipStartFrame);
```

For a one-off runtime change, keep authored data unchanged and use `Seek(tick)`
or `Play(stateId, stateTick)` instead.

ABI v1 does not expose C# setters for authored controller, default-clip, or
skeleton UUIDs because the generic reflected property bridge does not marshal
128-bit UUID values. Author those references in the Inspector, scene/prefab
data, or the typed editor automation surface. `ReadState()` returns the active
controller, clip, and skeleton as typed generation-bearing references.

## Submit commands and inspect receipts

All writes are copied into a bounded deferred command queue. A returned `Queued`
receipt means the request was accepted, not that a new pose is already visible.
The result can be observed at `ObservableTick` and its
`PublicationGeneration`; poll with the producer identity from the receipt when
a durable workflow needs the final outcome.

```csharp
AnimationParameterId speed = new(0x8d45_87fdUL); // Use the cooked controller ID.
AnimationCommandReceipt submitted = animator.SetParameter(speed, 1.25f);

if (!submitted.Accepted)
{
	Log.Error($"Animation write failed: {submitted.Status}");
	return;
}

AnimationCommandReceipt current = animator.ReadReceipt(
	submitted.ProducerId,
	submitted.ProducerSequence,
	submitted.CorrelationId);
```

The facade supplies deterministic non-zero producer sequence, correlation,
idempotency, and cancellation identities when `AnimationCommandOptions` leaves
them at zero. Supply explicit values when a job persists them across retries:

```csharp
AnimationCommandOptions options = new()
{
	TargetTick = state.Tick + 1,
	ProducerId = 42,
	ProducerSequence = durableSequence,
	CorrelationId = durableCorrelation,
	IdempotencyKey = logicalActionId,
	CancellationToken = cancellationId
};

AnimationCommandReceipt play = animator.Play(runStateId, 0, options);
AnimationCommandReceipt cancelled = animator.Cancel(
	play.CancellationToken,
	play.CorrelationId);
```

Available methods cover boolean, integer, float, vector, quaternion, and
transform parameters; triggers; play/stop/pause/resume/seek; loop, direction,
speed, crossfade, and layer requests; typed IK goals; and ragdoll requests.
Not every loaded runtime/provider must support every operation. Call
`QueryCapability` before optional work and treat its status and reason mask as
authoritative instead of inferring support from the presence of a C# method.

## Change controller states without snapping

Use `CrossFade` when gameplay changes between two clip states and the model
should move continuously from its currently published pose into the new
animation. `Play` changes state immediately; `CrossFade` advances both clip
cursors and blends their skeletal pose, morphs, curves, events, and root motion
for the requested duration:

```csharp
private const ulong IdleStateId = 0x1d1e_0001UL; // Use the cooked controller ID.
private const ulong WalkStateId = 0x1d1e_0002UL;

private bool _wasMoving;

public override void OnUpdate()
{
	bool isMoving = ReadMovementSpeed() > 0.1f;
	if (isMoving != _wasMoving)
	{
		ulong destination = isMoving ? WalkStateId : IdleStateId;
		AnimationCommandReceipt receipt =
			animator.CrossFade(destination, transitionSeconds: 0.20f);
		if (!receipt.Accepted)
		{
			Log.Error($"Animation transition failed: {receipt.Status}");
		}
		_wasMoving = isMoving;
	}
}
```

The `stateId` is the stable ID of a state in the assigned Animation Controller,
not a clip asset ID or a frame number. The destination must currently be a
compatible clip state in that controller. A direct-clip Animator has no named
state table, so `CrossFade` returns `NotReady`; assign a controller with at
least two clip states first. Durations are seconds, must be finite and
non-negative, and are converted to the deterministic fixed-tick timeline.
A zero-second crossfade is an immediate state change.

Calling `CrossFade` again while a blend is active captures the pose already
shown on screen and uses it as the next transition's source. This lets rapid
walk/run/idle changes remain continuous instead of jumping back to either old
clip endpoint. During a transition the CPU reference path is authoritative; a
GPU-only visual request reports its capability downgrade and uses the same
deterministic CPU result rather than silently sampling only one clip.

## Read immutable state

`ReadState()` identifies both the animation tick being described and the
boundary at which it was observed:

```csharp
AnimatorStateSnapshot state = animator.ReadState();
if (state.Status == AnimationStatus.Ok)
{
	Log.Info($"state={state.ControllerStateId} tick={state.Tick} " +
		$"age={state.AgeTicks} publication={state.PublicationGeneration}");
	AnimationControllerAsset controller = state.Controller;
	AnimationClipAsset clip = state.Clip;
	SkeletonAsset skeleton = state.Skeleton;
}
```

Parameters, curves, morphs, bones, sockets, retained events, and root-motion
outcomes use a two-call required-count protocol. Each method returns one
`AnimationBufferResult<T>` with status, copied values, source tick, publication
generation, and explicit overflow metadata. No partial semantic array is
returned after an error or undersized copy.

The managed hard limits are 4,096 parameters, morphs, bones, sockets, and
protected event-batch records; 8,192 retained per-animator event or root-motion
records; and 65,536 curves. `HardLimitExceeded`, `Overflowed`, and
`DroppedCountLowerBound` are observable outcomes, not invitations to allocate
an unbounded array.

## Subscribe to events

Subscriptions are disposable and can filter by animator, controller generation,
source state, layer, event ID, ordinal tag, and payload category:

```csharp
private AnimationEventSubscription? _footsteps;

public override void OnStart()
{
	Animator? animator = Object.GetComponent<Animator>();
	if (animator == null) return;

	AnimationEventFilter filter = new(
		stateId: runStateId,
		tag: "footstep",
		payloadCategories: AnimationEventPayloadCategory.String);
	_footsteps = animator.SubscribeEvents(OnFootstep, filter);
}

private static void OnFootstep(AnimationEvent animationEvent)
{
	Log.Info($"{animationEvent.Tag}: {animationEvent.Payload} " +
		$"loop={animationEvent.LoopCount} tick={animationEvent.SourceTick}");
}

public override void OnDestroy()
{
	_footsteps?.Dispose();
	_footsteps = null;
}
```

The scripting protected-commit phase copies the complete native event
publication, rejects malformed batches atomically, orders records
deterministically, and invokes subscribers without holding animation storage.
An exception is logged with its correlation ID and isolated to that callback;
later callbacks and animators still receive the publication. Gameplay assembly
reload and Play Mode stop invalidate every old token before delegates can keep
the collectible context alive.

## Query motion matching

`AnimationMotionMatchQuery` is the versioned, copied gameplay query for a
`MotionDatabaseAsset`. It owns exactly 48 signed quantized feature lanes, 48
unsigned weights, and up to 256 sorted excluded sample IDs. It also carries the
database schema fingerprint, tag/contact/phase/transition filters, continuity
history and penalties, and bounded candidate, sample, and diagnostic budgets.
The constructor copies every span, so later changes to the caller's arrays do
not change a queued query.

```csharp
short[] features = BuildQuantizedMotionFeatures(); // Exactly 48 lanes.
ushort[] weights = BuildMotionFeatureWeights();    // Exactly 48 lanes.
ulong schemaFingerprint = ReadCookedMotionSchemaFingerprint(database);

AnimationMotionMatchQuery query = new(schemaFingerprint,
	features, weights, excludedSampleIds)
{
	RequiredTags = locomotionTagMask,
	ExcludedTags = airborneTagMask,
	RequiredContacts = groundedContactMask,
	MaximumCandidates = 32,
	MaximumSamplesToVisit = 16_384
};

AnimationMotionMatchQueryReceipt queued = animator.QueryMotionMatch(
	database, query, queryId: durableQueryId);
```

The additive request, receipt, and result record kinds do not enlarge the
frozen ABI-v1 motion records. `ValidatedFields` reports fields accepted by the
boundary, while `ScheduledFields` reports fields the installed evaluator
actually consumed. The current evaluator owns and schedules the complete copied
V2 payload. `FullyScheduled` should therefore be true for an accepted request;
`RequiresRuntimeQueryIntegration` remains an additive compatibility flag for an
older runtime that validates a newer record without consuming all of it.

`ReadMotionMatchResults()` returns copied integer diagnostics. Always test
`AnimationMotionMatchResult.Has(...)` before consuming an optional group such
as `SelectedSource`, `FeatureBlockCosts`, `Exclusions`, `Fallback`,
`SelectionSource`, or `GpuStatus`. The nine block costs are returned in the
cooked schema order: trajectory, pose, velocity, facing, contact, phase, tag,
transition, and continuity. `Database` and `SchemaFingerprint` identify the
exact database generation/schema used for a result; selected source clip/tick,
total and per-block costs, exclusion/fallback/source data, and GPU status are
never guessed when a runtime cannot provide them.

The central advanced-command store retains the complete request until protected
capture, runs the deterministic integer CPU scorer, and publishes the result at
the normal animation authority boundary. CPU fallback is reported explicitly
when GPU candidate generation is unavailable; it still preserves exact selected
source, integer block/metadata costs, exclusion counts, schema identity, and the
next-frame authoritative selection. Frozen V1 queries continue to publish their
legacy snapshot format independently.

## Status handling

- `Ok` is a successful immediate read or completed command result.
- `Queued` is accepted deferred work; use the receipt's observation metadata.
- `NotReady` means the instance or a complete publication is not available yet.
- `StaleHandle` means the copied generation no longer names the live instance.
- `MissingAsset` and `IncompatibleAsset` identify dependency failures.
- `Unsupported` is an honest capability/provider result; preflight optional work.
- `Cancelled`, `Rejected`, and `DeterminismViolation` are final semantic outcomes.
- `InvalidEnvelope`, `BufferTooSmall`, and `HardLimitExceeded` are boundary or
  bounded-copy failures; no partial result became visible.

## Build and focused verification

From a Visual Studio developer PowerShell in a clean checkout:

```powershell
cmake --preset RelWithDebInfo
dotnet build Engine/Managed/TerraEngine.csproj -c RelWithDebInfo --no-restore
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests
$env:TERRAENGINE_TEST_FILTER = "Animation.Managed"
& .\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
cmake --build --preset RelWithDebInfo --target install
```

Use `Docs/Testing.md` for the full CTest workflow. Final acceptance also runs
the installed-editor, gameplay-camera, reload, and exported-runtime scenarios;
a build-tree-only run is supporting evidence rather than the install gate.
