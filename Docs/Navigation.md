# TerraEngine Navigation

Navigation is an engine-owned, ground-only service. Objects do not carry
navigation components. Gameplay submits object-to-location or
object-to-object requests with an explicit `NavigationProfile`; gameplay owns
movement, cancellation, replacement, and replanning.

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

## Streaming and export

Streaming uses world/cell generations and reports missing coverage or expired
leases explicitly. Export packages contain automatically discovered cooked
`.tenavgeom` companions and optional `.tenav` caches. Caches accelerate startup
but are never required; runtime generation remains authoritative. Recast
Navigation is private to the native backend and its zlib notice is retained in
the installed notice bundle.

## Hard-break document rules

The removed navigation component types are not registered, reflected,
serialized, scripted, exported, or exposed by editor automation. There is no
migration, alias, tombstone, compatibility loader, deprecated ABI, or automatic
conversion path. A scene or prefab containing an unregistered type is rejected
before publication, and the active scene remains unchanged. Generic editor and
automation operations use the standard unknown-component rejection path.

Profiles and navigation settings remain durable editor configuration and may be
saved, restored, and recorded in history. Geometry authoring and explicit
connection authoring are not part of the scene API.

## Collision-safe script movement

`NavigationRouteFollower` is an optional managed helper for gameplay that
already owns a `CharacterController`. It is not a navigation component and it
does not move a transform. `FixedStep` reads the controller object's published
position, then sends `CharacterController.SetDesiredVelocity`; the physics
controller therefore remains responsible for collision, stepping, and final
transform publication.

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

void Start()
{
	CharacterController controller = Object.GetComponent<CharacterController>()!;
	NavigationProfile profile = /* copied, conservative traversal values */;
	_follower = new NavigationRouteFollower(Object, controller, profile,
		speedMetresPerSecond: 4.0F, replacementKey: 0x4455434BUL);
	_follower.Begin(destination);
}

void OnFixedUpdate(float fixedDeltaSeconds)
{
	NavigationRouteFollowerState state = _follower!.FixedStep(fixedDeltaSeconds);
	if (state == NavigationRouteFollowerState.Arrived) { /* choose next goal */ }
}

void OnDisable() => _follower?.Stop();
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
