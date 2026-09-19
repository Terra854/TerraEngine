# TerraEngine Navigation

Navigation is an engine-owned, ground-only service. Direct callers submit
object-to-location or object-to-object requests with an explicit
`NavigationProfile`; gameplay owns movement, cancellation, replacement, and
replanning. An optional `NavigationAgentComponent` provides authored defaults
and a generation-qualified destination/status/guidance facade for gameplay
that wants persistent agent state. It does not move its owner.

## Direct queries and optional agents

Use the direct `Navigation` APIs when a caller needs a one-shot route or wants
to own all request state itself. A direct query has no component-registration
requirement and has no movement side effects.

Use the optional native `NavigationAgent` facade (or managed
`NavigationAgent` mapped to `NavigationAgentComponent`) when a caller needs a
stable destination, progress, arrival, stop, replanning, status, or guidance
record. Resolve the copied identity before submitting a destination, retain
the returned correlation ID, and treat a queued receipt as admission rather
than completion. Poll the receipt/status/guidance records and handle stale,
rejected, superseded, and expired outcomes explicitly. Runtime destinations,
requests, routes, and progress are not authored component data and are not
saved by scene/prefab persistence.

Both workflows share terrain coverage, profile validation, generation-qualified
results, and bounded resource limits. The agent facade is an optional layer;
it is not required for direct routing.

Fresh agent commands are validated at a protected command boundary before route
capture in the Navigation phase. Guidance is prepared from copied values and
published after routing in that same phase. Commands submitted later by gameplay
wait for the next command boundary; no extra scene flush or worker-side component
mutation is introduced. Automatic retries still respect the authored replan
interval.

## Editor authoring and diagnostics

Select one or more objects that already have `NavigationAgentComponent` in the
Inspector to edit authored settings. The Inspector validates every staged
candidate before enqueueing one transactional batch, so a multi-selection is
all-or-none. The project-profile action copies the existing project profile
values into the selected agents through the same transaction path. Area
penalties are grouped under the collapsed **Area Penalties** section to keep
the common traversal settings compact.

Authored controls include activation, traversal dimensions, slope/step and
clearance limits, resolutions, excluded-area mask, arrival/target thresholds,
replan interval, and area penalties. Runtime destination, request, route,
receipt, state, and guidance values are diagnostic read-only observations; they
are never edited through authored controls.

Open **Windows > Navigation** for the Navigation diagnostics window. It shows
bounded coverage/connection/route information, selected-agent route and next
waypoint guidance, and aggregate active/requesting/following/failed counters.
It also reports pending/capacity, current-owned
memory, retries, rejected/coalesced commands, and protected publish time.
Selected routes are advisory and may be truncated; the overlay never treats a
stale or unusable route prefix as movement input.

## Requests and outcomes

Requests capture stable object identity, lifetime, world, and current pose at a
protected boundary. A route is immutable and generation-qualified. `Complete`
returns route points; `PendingPreparation` means coverage is rebuilding;
`Stale` requires a new request; `Cancelled` is an explicit caller outcome; and
`UnavailableCoverage`, `InvalidEndpoint`, `Unreachable`, `QueueFull`, and
`ProviderFailure` remain distinct diagnostics. Navigation never writes
transforms, velocity, physics, animation, or locomotion state.

## Automatic geometry discovery

Current published terrain collision is mandatory. Without it, navigation
reports unavailable coverage and does not fabricate a flat world. Eligible
static and dynamic geometry is discovered from physical scene/resource
publications: collider-backed geometry is preferred, then cooked mesh
companions, with physically-ready procedural typed sources included. Render
visibility does not control eligibility.

Dynamic create, enable, disable, move, rotate, scale, delete, replace, and
reshape changes carry stable identity and revision evidence. Workers rebuild
affected coverage from immutable packets; root, shard, portal, terrain,
source-index, validity, and diagnostic state becomes visible together at one
protected boundary. Stale work is discarded.

Dynamic recovery captures all required profiles against one immutable world
snapshot. Up to two profile builds run concurrently; larger groups proceed in
bounded waves. Their compatible results are merged at one protected publication
boundary, so callers never observe a partially published recovery group. Work
that no longer matches the current world or source revisions is discarded.
Build scratch reservations and retired jobs remain capacity-accounted until
their workers finish. Direct route searches use separate bounded worker ranges;
neither path permits workers to mutate live scene/component storage.

## Streaming and export

Streaming uses world/cell generations and reports missing coverage or expired
leases explicitly. Export packages contain automatically discovered cooked
`.tenavgeom` companions and optional `.tenav` caches. Caches accelerate startup
but are never required; runtime generation remains authoritative. Recast
Navigation is private to the native backend and its zlib notice is retained in
the installed notice bundle.

Navigation geometry and cache identity retain the canonical world-cell placement
of each source. Provider shards may store local coordinates internally, but
published routes and cache validation remain generation-qualified world
locations; a cache is usable only when its captured source and profile
fingerprints match the current authoritative publication. A stale, incomplete,
or malformed cache must be rejected with a caller-visible reason before fresh
coverage is allowed to replace it.

## Authored component and hard-break rules

`NavigationAgentComponent` uses ordinary component registration, reflection,
serialization, prefab loading, and transactional validation. It stores only
authored profile/threshold/replan values and inherited activation; there is no
agent-specific schema discriminator or legacy gate. A failed settings batch is
atomic and leaves the previous authored values unchanged.

The old navigation source and connection component types remain unregistered,
unreflected, unserialized, unscripted, unexported, and unavailable to editor
automation. A scene or prefab containing either unsupported type is rejected
before publication, and the active scene remains unchanged. There is no
migration, alias, tombstone, compatibility loader, deprecated ABI, or automatic
conversion path for those unsupported names.

Profiles and navigation settings remain durable editor configuration and may be
saved, restored, and recorded in history. Geometry authoring and explicit
connection authoring are not part of the scene API.

## Collision-safe script movement

`NavigationRouteFollower` is an optional managed helper for gameplay that
already owns a `CharacterController`. It is not a navigation component and it
does not move a transform. `Update(Time.DeltaTime)` reads the controller
object's published position, then sends `CharacterController.SetDesiredVelocity`;
the physics controller therefore remains responsible for collision, stepping,
and final transform publication. The managed agent sample follows the same
ownership rule: it reads guidance in the existing `OnUpdate` callback, submits
deferred controller intent, and clears velocity when guidance is unusable or
stopped.

Agent commands are copied into a bounded deferred queue. Destination commands
may coalesce within an intent segment and produce superseded receipts; Stop is
ordered and cannot be bypassed by an older destination. Receipts distinguish
queued admission from terminal applied/rejected/superseded state and are
retained only within the configured bound. An expired receipt is an explicit
result, not proof that the command was applied.

Velocity commands set persistent movement intent; submitting more commands does
not advance time. The physics scheduler advances characters once per fixed tick
and publishes their poses through the protected scene mutation boundary. Send
zero velocity (or stop the follower) to stop movement.

Virtual characters block one another without automatically inheriting another
character's contact velocity. A stationary character therefore holds its place;
rigid-body contact and impulse behavior remains governed by physics. Direct
transform assignments and explicit teleports are not collision-safe movement.

```csharp
private NavigationRouteFollower? _follower;

public override void OnStart()
{
	CharacterController controller = Object.GetComponent<CharacterController>()!;
	NavigationProfile profile = /* copied, conservative traversal values */;
	_follower = new NavigationRouteFollower(Object, controller, profile,
		speedMetresPerSecond: 4.0F, replacementKey: 0x4455434BUL);
	_follower.Begin(destination);
}

public override void OnUpdate()
{
	// Update changes deferred movement intent; physics owns fixed-step motion.
	NavigationRouteFollowerState state = _follower!.Update(Time.DeltaTime);
	if (state == NavigationRouteFollowerState.Arrived) { /* choose next goal */ }
}

public override void OnDisable() => _follower?.Stop();
```

The profile must be conservative for the controller: its radius and height are
at least the controller capsule dimensions, its clearance margin covers contact
padding, and it allows no larger step or slope than the controller. Ground-only
following also requires an upright controller with its local center halfway up
the capsule. The follower rejects a disabled controller or an object that also
has an enabled `ColliderSet`, which would make the controller contact itself.
Configure the controller and profile before `Begin`; if their geometry changes,
call `Stop`, apply the configuration, and begin a new route. If movement stops against a dynamic blocker,
the helper clears velocity, releases the old request, and retries a route at a
bounded interval. Each retry sends safe controller velocity again, so clearing
the blocker lets the actor continue without transform writes or a persistent
navigation agent.

## Verification

Use [Testing.md](Testing.md) for focused CTest, installed acceptance, five-run
evidence comparison, and mandatory install commands. The provider integration
is documented in [Dependencies/RecastNavigation.md](Dependencies/RecastNavigation.md).
## Navigation profiles

Navigation profiles are copied traversal values: capsule dimensions, slope and
step limits, build resolution, and area preferences. They do not carry a
profile ID or revision. Project presets are selected by their registration UUID,
while runtime build sharing is derived from the copied traversal values.
