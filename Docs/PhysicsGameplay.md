# Physics Gameplay Commands, Queries, and Events

TerraEngine's gameplay physics API is provider-neutral. Public headers contain
engine handles, UUIDs, generations, packets, and typed status values; Jolt types
remain private to `Engine/Physics/Backend`. The runtime provider is pristine Jolt
Physics 5.6.0 at commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`.

## Commands

Use the typed factories in `PhysicsCommands.h` for continuous force,
mass-independent acceleration, impulse, point impulse, instantaneous velocity
change, explicit velocity, teleport, kinematic target, sleep, and wake. A command
target includes the world, object, component, and body generations. Metadata
provides producer ordering, correlation, idempotency, cancellation, and an
effective tick.

`PhysicsSystem::SubmitCommand` means accepted into the bounded queue, not already
visible. The protected command-commit workload merges all producers by the
engine-defined ordering key and publishes completion receipts after execution.
Removed or recreated targets fail generation validation and cannot affect a new
body that reused the same compact slot.

## Scheduled queries

Create a `PhysicsQueryRequest` stamped with `PhysicsSystem::WorldHandle()`, then
submit one request or a span through `SubmitQuery` or `SubmitQueryBatch`.
Supported kinds are ray cast, box shape cast, box overlap, and point query. A ray
can use `Any`, `Closest`, or bounded `All` hit mode. Filters select collision
layers, sensors, static/dynamic/kinematic bodies, and optional excluded body or
object identities.

The calibrated project limits are:

- maximum cast distance: 100,000 metres;
- maximum shape half-extent: 1,000 metres per axis;
- maximum hits: 64 per query;
- maximum scheduled batch: 10,000 requests;
- retained result history: two physics ticks by default.

Preparation copies validated requests into immutable packets. After Jolt has
joined its simulation jobs, large batches are divided into independent tasks on
TerraEngine's shared `ThreadPool`. Provider hit collection stays bounded, then
the protected publication boundary revalidates every body/object/component/world
generation, removes duplicates, and sorts by distance/fraction and stable
identity. `ReadQuery` returns an immutable result or a typed unavailable,
expired, cancelled, or stale status.

`QueryImmediate` is a synchronous protected-boundary convenience for tooling and
rare native decisions. Ordinary gameplay and automation should prefer scheduled
batches so query work can share the job graph.

## Events

Subscribe with a data-only `PhysicsEventSubscriptionPolicy`. Policies select
collision, contact, trigger, activation, sleep, or future constraint-break
categories and can filter layers, object IDs, and sensors. Provider callbacks do
not invoke gameplay, managed code, editor UI, scene mutation, or rendering.

Jolt callback threads append bounded raw values to sharded collectors. After the
provider barrier, the backend resolves body IDs to engine generations. The
protected publication boundary validates those generations again, canonicalizes
participant order, applies the subscription snapshot, sorts the packet, and
records filtered/dropped overflow counts. `LatestEventPacket` is immutable and
tick-scoped. Unsubscription affects the next packet, never one already being
published.

Collision and trigger transitions are `Enter`, `Stay`, and `Exit`. Physical
contacts additionally publish a manifold summary containing point, normal,
relative velocity, and penetration. Activation and sleep are unary events. A
destroyed object, component, collider record, body slot, or world generation is
dropped before publication.

## Scheduler and visibility

The seven physics workloads declare their resources explicitly:

1. `PhysicsSnapshot` captures flushed scene authoring.
2. `PhysicsPrepare` prepares bodies, commands, and query batches.
3. `PhysicsCommandCommit` validates generations and commits provider staging.
4. `PhysicsSimulate` runs fixed ticks through the shared job adapter.
5. `PhysicsExtract` collects body state, raw events, and parallel query results.
6. `PhysicsPublish` publishes immutable results and queues typed transforms.
7. `PhysicsPublicationBarrier` precedes the protected `After Physics` flush.

No worker writes live `ObjectManager` or component storage. Transform mutations
become visible only through the declared transactional flush.

## Build and focused validation

From a Visual Studio x64 developer prompt:

```powershell
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests --parallel 4
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^Engine\.PhysicsUS3$' --output-on-failure
cmake --build --preset RelWithDebInfo --target install --parallel 4
```

The US3 suite covers concurrent deterministic commands, all query contracts,
10,000-request bounds, event ordering/filtering/overflow, provider query/contact
behavior, and stale world/object/slot publication races.
