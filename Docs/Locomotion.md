# Character Locomotion (Native MVP)

Character Locomotion lets native gameplay request bounded ground movement for an
object that already has a supported `CharacterControllerComponent`. Locomotion
owns the request and movement authority; the character controller remains the
only source of collision-aware displacement. The MVP does not require an
animation or navigation component.

This guide describes native manual/scripted intent, optional Navigation Agent
following, and the implemented native US3 animation/root-motion integration.
The native phase gate passes 24 integrated regression suites and the required
install build. Managed C#, editor/export workflows, and complete Feature 060
acceptance/performance gates remain separate work.

## Setup and limits

Add an active `CharacterLocomotionComponent` to the same object as its active,
enabled character controller. The controller binding is `Self`. Leave navigation
and animator bindings unset for manual or scripted movement. Unsupported or
missing controllers prevent a binding from being published. Disabling or
removing either required component, unloading the scene, or stopping the Physics
world revokes the attachment and its movement authority.

The default locomotion limits are:

| Setting | Default | Meaning |
|---|---:|---|
| `maximumSpeed` | 5 m/s | Maximum requested planar speed |
| `acceleration` | 20 m/s² | Maximum speed change while accelerating |
| `braking` | 25 m/s² | Maximum speed change while slowing or reversing |
| `turnRate` | 6.283185 rad/s | Maximum turn about controller up |
| `facing` | Movement | Face the requested planar direction; Hold retains facing |

The component stores durable settings only. Runtime owner tokens, intent,
authority epochs, pending commands, and observations are not authored or copied
with the component. All values use metres, seconds, and radians. Directions must
be finite; they are projected onto the plane perpendicular to controller up and
normalized. Diagonal input therefore does not increase speed. A nonfinite or
negative speed is rejected; a speed above `maximumSpeed` is clamped and may
produce an `Adjusted` receipt. Zero or near-zero planar direction requests
stopping through the normal braking path. Gravity, collision response, slope and
step behavior remain controlled by Physics.

## Navigation guidance integration

Native navigation following consumes `NavigationAgentSystem::ReadGuidance()` from the
existing agent; that agent remains the sole owner of destination, route,
replanning, and agent Stop state. Locomotion owns the controller lease and
movement intent only. Do not submit a second route/query or keep a separate
locomotion destination when consuming guidance. Set the locomotion component's
navigation binding to Self, configure the existing agent's destination, acquire
the controller lease, and submit `FollowNavigation` with the current owner token.
Waiting or invalid guidance clears the physical contribution; it never reuses
the last direction. Immutable evidence is checked again before Physics steps.
Arrival comes from the agent's observed position, not from a zero desired speed.

The navigation path requires a conservative geometry match: an enabled,
physics-enabled upright +Y capsule with ground-point owner origin and local
center at half its total height, and no enabled redundant owner `ColliderSet`.
The current provider uses unscaled authored capsule geometry, so only effective
unit world scale (within 0.001 per axis) is supported. The navigation
profile must have radius at least the controller radius, height at least its
total capsule height, clearance at least controller contact padding, and step
and slope limits no greater than the controller's. Reject unsupported shape or
scale configurations instead of approximating clearance. For the frozen
fixture, the 0.35 m radius / 1.8 m total-height / 0.4 m step / 50° slope / 0.02 m
padding controller pairs with the 0.4 m radius / 2.0 m height / 0.35 m step /
45° slope / 0.1 m clearance profile. Revalidate after controller geometry,
owner collider, or profile changes. Compare dimensions with a 0.001 m linear
tolerance and slopes with a separate 0.001° tolerance; do not reuse a
dimensioned tolerance for angles.

The native navigation suite exercises observed arrival at 30/60/120 updates per
second, moving destinations, terrain and attachment invalidation, and stale
guidance at the physical boundary. A native scripting-ABI test also proves legacy
controller commands cannot move a leased character while unleased commands still
work. Actual CLR/editor/export qualification remains part of the final gate.

Stop timing also depends on which system owns the request. A
`NavigationAgentSystem::QueueStop()` issued from Scripting (phase 40) is too late
for the current Navigation command boundary (phase 35): its receipt remains
`Queued`, and the previously published guidance may still be consumed at
Locomotion phase 42 in that frame. The agent Stop applies at the next Navigation
35 boundary, after which nonusable guidance is available to the following
Locomotion 42 publication. By contrast, a Locomotion `Stop` submitted from
Scripting can be admitted at Locomotion 42 in the same frame and immediately
clear that controller's locomotion contribution there. Do not treat a queued
agent Stop receipt as an applied movement Stop.

## Native command flow

Use `TerraEngine::Locomotion::LocomotionSystem::Instance()` from native gameplay.
Resolve a currently published attachment with `ResolvePublished(entity)` or
`ResolvePublished(objectId)`. A handle identifies one exact scene, object,
component attachment, and locomotion-store slot lifetime. Retain it only while
that attachment remains active; resolve again after a lifecycle change.

Before acquiring control, find the matching entry in the immutable observation
root returned by `CopyObservations()`. Match the full `LocomotionHandle`, then
copy `authority.configurationRevision` and `authority.authorityEpoch`. An idle
observation supplies the current configuration revision and epoch for the
acquire envelope. If the handle or observation is not published yet, wait for
the next protected Locomotion publication instead of guessing a revision.

Build an `Acquire` `LocomotionCommand` with that handle and copied configuration
and authority epochs, plus a nonzero producer ID, a monotonically increasing
producer sequence, and a correlation ID. `SubmitBatch()` is thread-safe and
returns admission. A queued receipt means the request was accepted for a later
Locomotion boundary; it does not mean ownership or physical movement has already
changed. Read the retained result with `ReadReceipt(handle, producerId,
sequence, correlationId)`. Only an `Applied` or `Adjusted` acquire receipt grants
the opaque `ownerToken`. Never invent or persist that token across a release,
transfer, attachment replacement, or scene change.

The following abbreviated C++ example shows stages in a native command state
machine. The application owns producer IDs, sequence counters, correlation IDs,
and the pending receipt identity; it submits each stage once and polls from its
normal gameplay flow rather than blocking:

```cpp
#include <algorithm>
#include <span>

using namespace TerraEngine::Locomotion;

// The gameplay object ID, producer ID, and persistent counters are supplied by the caller.
LocomotionSystem& locomotion = LocomotionSystem::Instance();
const auto handle = locomotion.ResolvePublished(characterEntity);
if (!handle) return; // Resolve again after the next protected publication.
const auto observations = locomotion.CopyObservations();
if (!observations) return;
const auto row = std::find_if(observations->entries.begin(), observations->entries.end(),
	[&](const LocomotionObservation& value) { return value.handle == *handle; });
if (row == observations->entries.end()) return;
const LocomotionObservation& observation = *row;

LocomotionCommand acquire;
acquire.kind = CommandKind::Acquire;
acquire.handle = *handle;
acquire.expectedConfigurationRevision = observation.authority.configurationRevision;
acquire.expectedAuthorityEpoch = observation.authority.authorityEpoch;
acquire.producerId = playerInputProducer;
acquire.sequence = ++playerInputSequence;
acquire.correlationId = NextCorrelationId();
acquire.mode = MotionMode::Velocity;
const CommandBatchAdmission admitted = locomotion.SubmitBatch(std::span{&acquire, 1U});
if (!admitted.queued) return;
// Save the command identity and resume at ReadReceipt() on later updates.
// Do not submit another Acquire or allocate another sequence while it is pending.
const CommandReceipt acquired = locomotion.ReadReceipt(*handle, acquire.producerId,
	acquire.sequence, acquire.correlationId);
if (acquired.state != ReceiptState::Applied && acquired.state != ReceiptState::Adjusted) return;

// Refresh the row after the acquire receipt; configuration can change while a
// command is pending, and SetIntent must use the current revision.
const auto currentObservations = locomotion.CopyObservations();
if (!currentObservations) return;
const auto currentRow = std::find_if(currentObservations->entries.begin(), currentObservations->entries.end(),
	[&](const LocomotionObservation& value) { return value.handle == *handle; });
if (currentRow == currentObservations->entries.end()) return;
const LocomotionObservation& currentObservation = *currentRow;

LocomotionCommand intent;
intent.kind = CommandKind::SetIntent;
intent.handle = *handle;
intent.expectedConfigurationRevision = currentObservation.authority.configurationRevision;
intent.expectedAuthorityEpoch = acquired.authorityEpoch;
intent.ownerToken = acquired.ownerToken;
intent.producerId = playerInputProducer;
intent.sequence = ++playerInputSequence;
intent.correlationId = NextCorrelationId();
intent.intentKind = IntentKind::Player;
intent.directionSpace = DirectionSpace::World;
intent.direction = {0.0, 0.0, 1.0};
intent.requestedSpeed = 3.0;
intent.lifetime = IntentLifetime::OneUpdate;
const CommandBatchAdmission input = locomotion.SubmitBatch(std::span{&intent, 1U});
if (!input.queued) return; // Retain the reason and handle rejection in caller code.
```

In a real caller, retain the command identity and a small stage value across
updates so the Acquire and SetIntent submission blocks each run once; this
abbreviated listing places their stages together for readability. An Idle observation
is published for that handle with a nonzero configuration revision and the
current authority epoch, so native code does not need private store access or a
guessed default revision. `CopyObservations()` may be empty before the first
Locomotion publication; wait for the handle and its Idle row, then submit. The
`Queued` admission receipt is not the terminal result: retain its identity tuple
and call `ReadReceipt()` on later gameplay updates until it becomes terminal.
Do not block the gameplay thread waiting for a protected phase.

After acquisition, submit a `SetIntent` command using the granted owner token,
the latest authority epoch, and the same configuration revision. Choose
`IntentKind::Player` with `IntentLifetime::OneUpdate` for refreshed manual input.
Player input expires to zero at the next locomotion update without a refresh, so
the character brakes instead of retaining stale input. Choose
`IntentKind::Script` with `IntentLifetime::UntilStopped` for a persistent native
request; explicitly send `Stop` or `Release` when that request ends.
`UntilStopped` does not expire automatically. `Stop` keeps the owner token
available for a later intent, while `Release` relinquishes ownership and makes
that token unusable. Both clear locomotion's requested contribution immediately
at their protected boundary by advancing the physical lease epoch; they do not
simulate gradual braking or teleport the character. Normal braking applies when
an intent expires or a zero/tiny planar direction is requested. Use the latest
applied receipt's authority epoch for each subsequent command (and the current
observation's configuration revision), rather than reusing epochs captured
before a lifecycle or ownership change.

Set `directionSpace` to `World` when the input vector is in world axes, or
`ControllerLocal` when it is relative to the captured controller orientation.
Set `direction` and `requestedSpeed` independently: vector length is not a speed
multiplier. A zero direction with a valid owner is a stationary request.
Movement is shaped at fixed Physics steps by the configured acceleration,
braking, and speed limits; do not move the character by writing its transform.

Each producer must use unique increasing sequence numbers. An exact retry with
the same producer, sequence, correlation, and payload returns the retained
receipt; changing a payload while reusing a sequence is rejected. A receipt's
`Queued` state records admission only. `Applied` or `Adjusted` reports the
protected update epoch and resulting authority epoch; `Rejected` reports a
reason such as stale handle/configuration, ownership conflict, unsupported
controller, or capacity. Use the latest receipt/observation epochs when
submitting the next command.

## Stop, transfer, and release

`Stop` clears the current locomotion intent and its requested physical
contribution at the protected Locomotion boundary, while retaining its owner
token. This is an immediate contribution clear, not gradual braking or a
teleport. After the Stop receipt applies, a newer `SetIntent` from the same
owner can resume movement. Stop does not disable gravity, collision response,
or unrelated physical effects.

`Transfer` explicitly changes the accepted producer. Submit it from the current
owner with the current token, configuration revision, and authority epoch, and
set `transferProducerId` to the new producer. Wait for its applied receipt and
give the new owner the returned token and epoch. The transfer clears the old
intent as one protected change. A competing producer that does not own the
current token is rejected without replacing the accepted owner.

`Release` returns the character to idle and invalidates the owner token. It does
not restore legacy controller velocity that existed before acquisition. Acquire
again with a fresh receipt before sending more intent. Physics can also retain
an `ExternalExclusive` exclusion for a separate movement owner; ordinary
locomotion Release cannot clear another owner's lease. A pending or active
ragdoll owns an `ExternalExclusive` Physics lease; a failed Activate does not
silently release it. The caller must explicitly request Deactivate to abort
failed admission, or request recovery and wait for the provider's typed handoff
grant. Until that grant releases the external owner, locomotion remains excluded
and hot rebinding the animator returns `NotReady`. Removing an animator component
queues teardown while preserving the external exclusion; after Physics accepts
teardown and hands off the current controller pose, the lease becomes unleased.
Locomotion does not reacquire automatically: submit a fresh Acquire.

## Observation and scheduler timing

The scheduler order is Scene (30), Scripting (40), Locomotion (42), Animation
(45), then Physics (50). Locomotion reconciles live component membership and
controller lifetimes at its protected phase. Worker preparation receives copied
values; it does not read live component storage or write transforms. Commands
submitted after the current admission cutoff are eligible at the next Locomotion
update. A receipt becoming `Applied` records the authority boundary, not a
promise that the character has already moved; Physics applies accepted targets
on fixed ticks.

Read immutable results from `CopyObservations()`. `desiredVelocity` is the
requested locomotion contribution. `observedPlanarSpeed` and
`observedLocalDirection` come from a completed physical observation, not the
requested intent. Check `physicalTick`, `frame`, and `motionHistoryAvailable`
before comparing observations; the first observation or a teleport may not have
a valid movement derivative. A character blocked by a wall can have nonzero
desired velocity and near-zero observed speed. These values answer different
questions and should not be substituted for one another.

## Native animation parameters and root motion

The native mapping reads a completed Physics observation, never requested
locomotion intent. Configure the optional `CharacterLocomotionComponent`
parameter names `speedParameter`, `localDirectionXParameter`,
`localDirectionZParameter`, `groundedParameter`, and `movingParameter`. The first
three must resolve to uniquely named Float parameters; the last two must resolve
to uniquely named Boolean parameters. Empty names disable individual outputs.
The mapping uses the controller's authored stable parameter IDs, which must fit
the native command ID width, and validates duplicate names/IDs, missing names,
wrong types, or unsupported IDs before publishing any writes. It caches the
mapping against the controller asset generation and authored names. A reload
creates a gap while Animation publishes the replacement generation: the old
mapping must not be treated as current, and output is unavailable until the new
generation is copied and resolved. Native regressions verify rejection of queued
old-generation writes and fresh observed-value mapping after replacement.

For a valid mapping, Locomotion prepares the five-or-fewer typed parameter
commands as one bounded batch. It uses observed planar speed and local X/Z
direction only when `motionHistoryAvailable` and a positive observation duration
are present; otherwise those derivative values and `Moving` are zero/false.
`Grounded` can still reflect the observed support state. These values carry the
source `physicalTick`; they are not a same-tick prediction from an intent receipt.
Because Locomotion runs before Animation and consumes the last completed Physics
observation, parameter visibility follows that scheduler boundary rather than
being instantaneous. A wall may therefore leave requested velocity nonzero while
the next observed speed is near zero. Fixture state selection is currently
explicit (`StartMove` or `StartRootMotion` trigger actions); changing the five
observed values does not itself select an authored state or BlendTree sample.

Root motion requires a live root-ready binding: the selected clip has enabled
root data, the animator is not forced to GPU-visual-only execution, and the
captured owner/animator relative basis remains valid. In root mode, Locomotion
publishes an overlay only for the leased, controlled, root-ready animator and
registers it with Animation's root-motion consumer registry. This overlay is
additive to unrelated consumer declarations and is removed when root ownership
ends or becomes invalid. For that authorized binding, Animation applies the
root delta to the character even when the authored default is `ExtractOnly`;
ragdoll lifecycle suppresses physical root application back to extraction-only.
Otherwise, `ExtractOnly` extracts data without applying it to the controller.
This keeps root translation and yaw as the exclusive physical producer; do not
also apply procedural facing yaw or write a transform from a root curve.
The frozen workload bounds pending root data to 8 intervals and 0.25 seconds.
With zero Physics steps, valid samples remain bounded and unconsumed; with
multiple steps, matching fragments are consumed no more than once and only for
the fixed duration each step covers. A sample shorter than a fixed tick
contributes only its covered fragment. Collision or provider limits report the
authoritative adjusted result and discard unrealized spatial remainder—there is
no late catch-up. Switching between root motion and velocity, releasing, or
transferring authority must clear the outgoing contribution at an epoch
boundary so both producers never influence the same controller concurrently.

Root receipts distinguish interval totals from whole-tick evidence. The scene
retains at most 256 detailed tick samples; older details are explicitly reported
as omitted/expired while scalar totals and tick counts remain available. A tick
shared by multiple intervals is identified as shared, not falsely attributed as
independent realized motion for each contributor. Physics uses a separate bounded
65,536-record transport ring with reserved terminal capacity. The current Windows
x64 test build measures 328 bytes per record (21,495,808 bytes for that ring);
this is a measured build layout, not a portable ABI size guarantee.

Source and provenance status for redistributable locomotion workload assets is
recorded in `Tests/Fixtures/Locomotion/workloads.json`. Those source assets are
not interchangeable proof of a complete generated/installed fixture: the idle,
moving, translation/yaw, controller, and ragdoll cases must remain cohesive and
deterministically generated by later T070 tooling before their generated paths,
UUIDs, or hashes can be claimed. In-memory native test assets do not establish
CLR, editor, export, or installed-product behavior. The native US3 phase gate
passes; US4 editor/authoring and remaining managed/export work are later phases.

## Managed C# authoring and automation

Managed scripts use the generation-pinned `TerraEngine.Locomotion.CharacterLocomotion`
component facade. Add an active `CharacterLocomotionComponent` and supported
character controller to the same object; retrieve them with
`GetComponent<CharacterLocomotion>()`. The first `Identity` read resolves and
pins the exact scene/object/component attachment. A default or invalid identity
means the protected native binding is not published yet. If the attachment is
removed/replaced or its scene ends, discard that wrapper and retrieve a fresh
component wrapper only after the replacement is active; an old wrapper never
silently binds to a replacement.

Commands are admitted asynchronously. `Queued` means accepted for a future
Locomotion boundary, not applied movement or granted ownership. Keep the
receipt's `(ProducerId, Sequence, CorrelationId)` tuple and poll
`ReadReceipt()` during ordinary script updates; do not block or spin until a
phase runs. Continue only after `Applied` or `Adjusted`. Treat rejection,
expiration, stale identity/configuration, and ownership conflict as explicit
states to handle; do not resubmit a command every frame while its receipt is
still queued. The wrapper caches the owner token and authority epoch only after
observing an applied receipt from its own producer.

The compilable `PlayerLocomotionInput` reference script is maintained at
[ManagedLocomotionAuthoringSample.cs](../Tests/Tools/Locomotion/ManagedLocomotionAuthoringSample.cs)
and is included in the bundled-SDK probe build. It retries an unbound resolve
across startup publication, then pins that exact identity; after a stale
generation or terminal failure it stops instead of rebinding or resubmitting.
It submits one explicit Player `OneUpdate` command at a time and polls the exact
receipt tuple before sending another.

The example keeps one command in flight. A real input script should replace the
fixed direction/speed with its sampled input and keep refreshing Player intent;
`Player` is valid only with `OneUpdate`, so a missing refresh expires to zero and
the controller brakes. By contrast, `CharacterLocomotion.SetIntent(...)` is the
convenience path for a persistent Script request: it emits
`IntentKind.Script` with `UntilStopped`. Poll its receipt the same way, and
submit `Stop()` or `Release()` when the request ends. Do not combine the
convenience methods with caller-managed sequence numbers on the same wrapper;
each producer sequence must remain unique and increasing.

For destination following, put `NavigationAgentComponent`,
`CharacterLocomotionComponent`, and the supported controller on the same
character object, and set the locomotion component's navigation binding to
`Self`. The agent remains the sole owner of destination and route state; do not
also feed manual direction while following. `SetDestination(Object)` returns a
receipt with a correlation ID; poll the terminal receipt before starting the
locomotion acquisition stage:

```csharp
using TerraEngine;
using TerraEngine.Locomotion;

NavigationAgent? agent = character.GetComponent<NavigationAgent>();
CharacterLocomotion? motion = character.GetComponent<CharacterLocomotion>();
if (agent == null || motion == null) return;

NavigationAgentReceipt destination = agent.SetDestination(targetObject);
if (!destination.Accepted) return;
ulong destinationCorrelation = destination.CorrelationId;
// In later OnUpdate calls, poll agent.ReadReceipt(destinationCorrelation).
// Remain pending while State == Queued; require Applied before proceeding.
// Then submit motion.Acquire(), poll its receipt, and only after it applies
// submit motion.FollowNavigation() and poll that receipt as well.
```

After the applied destination receipt, the route may still be pending: inspect
`agent.Status` or `agent.Guidance` on later updates and do not interpret unusable
guidance as permission to replay an old direction. `FollowNavigation()` selects
the agent's current published guidance; it does not submit another route.
`agent.Stop()` stops the route through the agent's own command boundary, while
`motion.Stop()` clears the locomotion contribution at the Locomotion boundary.
These are separate receipts and boundaries; a queued navigation Stop does not
mean the controller was already stopped in that frame. After a scene/attachment
lifecycle change, replace both stale managed wrappers with fresh components
rather than retaining their prior identities.

Editor automation should use these same public managed methods and receipt
polling rules. Keep only command identities and copied values between updates;
do not cache native pointers, fabricate handle/owner tokens, or bypass the
protected command boundary. The examples document API shape and timing only;
they do not claim CLR/editor/export or installed-product runtime qualification.

## Build, test, and install

From a clean Windows x64 checkout, initialize pinned submodules, configure the
RelWithDebInfo preset, build the engine/editor/export and test executables, run
the focused locomotion and existing controller regressions, then run CTest and
install. This follows the workflow in [Testing](Testing.md):

```powershell
git submodule update --init --recursive
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Export
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R 'Locomotion|Engine.PhysicsConstraintsAndCharacters|Engine.PhysicsManaged' --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -j 1 --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

The installed application bundle is under
`out/build/RelWithDebInfo/Build`. CTest results use the build tree at
`out/build/RelWithDebInfo`.

On machines where the Visual Studio 18 developer shell omits the Windows SDK
and UCRT include/library paths, use the repository helper for CMake configure,
build, and install commands. It selects the newest installed complete x64 SDK
and restores the caller's environment after each invocation:

```powershell
pwsh -NoProfile -File Tests/Tools/Locomotion/InvokeLocomotionBuild.ps1 --fresh --preset RelWithDebInfo
pwsh -NoProfile -File Tests/Tools/Locomotion/InvokeLocomotionBuild.ps1 --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Export
pwsh -NoProfile -File Tests/Tools/Locomotion/InvokeLocomotionBuild.ps1 --build --preset RelWithDebInfo --target install
```

Use a Visual Studio x64 developer command with MSVC, CMake, Ninja, and a Windows
SDK installed. The helper does not install dependencies. For current test
filters and engine test conventions, see [Testing](Testing.md).
