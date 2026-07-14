// PhysicsCookedShapeSimulationTests.cpp - provider-neutral regression coverage
// for Jolt 5.6 collider cooking, opaque restoration, and cache publication.

#include "../TestHarness.h"

#include "Physics/Assets/ColliderRecipe.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsWorld.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		// Jolt uses the engine-wide worker pool. Each test owns one short-lived
		// session so provider jobs cannot outlive the backend being inspected.
		class ThreadPoolSession final {
		public:
			ThreadPoolSession() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession() {
				ThreadPool::instance().shutdown();
			}
		};

		struct BackendFixture final {
			ThreadPoolSession pool;
			JoltPhysicsBackend backend{ThreadPool::instance()};
			PhysicsWorldHandle world{{1U}, 1U};

			bool Initialize() {
				if(!backend.Initialize().Succeeded()) return false;
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				return backend.CreateWorld(configuration).Succeeded();
			}

			~BackendFixture() {
				// DestroyWorld is intentionally best-effort: initialization failures are
				// asserted by the individual test before this destructor executes.
				(void)backend.DestroyWorld(world);
				(void)backend.Shutdown();
			}
		};

		PhysicsShapeRecipe MakeBoxRecipe(float extent = 1.0F) {
			PhysicsShapeRecipe recipe;
			recipe.kind = PhysicsShapeKind::Box;
			recipe.dimensions = {extent, extent, extent};
			return recipe;
		}

		PhysicsShapeRecipe MakeConvexRecipe() {
			PhysicsShapeRecipe recipe;
			recipe.kind = PhysicsShapeKind::ConvexHull;
			recipe.stableRecipeHash = 0xC0A0'0001ULL;
			recipe.points = {
				{-0.75F, 0.0F, -0.75F}, {0.75F, 0.0F, -0.75F},
				{0.0F, 0.0F, 0.75F}, {0.0F, 1.5F, 0.0F}};
			return recipe;
		}

		PhysicsShapeRecipe MakeMeshRecipe(std::uint32_t cells = 1U) {
			PhysicsShapeRecipe recipe;
			recipe.kind = PhysicsShapeKind::Mesh;
			recipe.stableRecipeHash = 0x4D45'5300ULL + cells;
			const float halfWidth = static_cast<float>(cells) * 0.5F;
			for(std::uint32_t z = 0U; z <= cells; ++z) {
				for(std::uint32_t x = 0U; x <= cells; ++x) {
					recipe.points.push_back({static_cast<float>(x) - halfWidth, 0.0F, static_cast<float>(z) - halfWidth});
				}
			}
			const std::uint32_t rowWidth = cells + 1U;
			for(std::uint32_t z = 0U; z < cells; ++z) {
				for(std::uint32_t x = 0U; x < cells; ++x) {
					const std::uint32_t first = z * rowWidth + x;
					recipe.indices.insert(recipe.indices.end(), {
						first, first + rowWidth, first + 1U,
						first + 1U, first + rowWidth, first + rowWidth + 1U});
				}
			}
			return recipe;
		}

		PhysicsShapeRecipe MakeCompoundRecipe() {
			PhysicsShapeRecipe recipe;
			recipe.kind = PhysicsShapeKind::Compound;
			recipe.stableRecipeHash = 0xC04D'0001ULL;
			PhysicsShapeRecipe left = MakeBoxRecipe(1.0F);
			left.localCenter = {-1.5F, 0.0F, 0.0F};
			left.subShapeKey = 11U;
			PhysicsShapeRecipe right = MakeBoxRecipe(1.0F);
			right.localCenter = {1.5F, 0.0F, 0.0F};
			right.subShapeKey = 12U;
			recipe.children = {std::move(left), std::move(right)};
			return recipe;
		}

		PhysicsShapeRecipe MakeRestoredRecipe(const PhysicsCookResult& cooked) {
			PhysicsShapeRecipe restored;
			restored.kind = PhysicsShapeKind::Compound; // Opaque bytes define the actual provider shape.
			restored.stableRecipeHash = cooked.recipeHash;
			restored.cookedFormatFamily = cooked.formatFamily;
			restored.cookedProvider = cooked.provider;
			restored.cookedProviderVersion = cooked.providerVersion;
			restored.cookedProviderRevision = cooked.providerRevision;
			restored.cookedFormatVersion = cooked.formatVersion;
			// Carry the envelope-equivalent checksum beside the opaque bytes so the
			// backend can reject tampering before entering provider deserialization.
			restored.cookedPayloadChecksum = CookedColliderAsset::ComputePayloadChecksum(cooked.payload);
			restored.cookedPayload = std::make_shared<const std::vector<std::byte>>(cooked.payload);
			return restored;
		}

		bool PublishShape(JoltPhysicsBackend& backend, PhysicsWorldHandle world, PhysicsShapeHandle handle,
			const PhysicsShapeRecipe& recipe) {
			PhysicsBackendStagingToken staging;
			return backend.CreateShape(world, handle, recipe, staging).Succeeded()
				&& backend.CommitStaging(staging).Succeeded();
		}

		bool PublishBody(JoltPhysicsBackend& backend, PhysicsWorldHandle world, PhysicsBodyHandle handle,
			PhysicsShapeHandle shape, PhysicsMotionType motionType, PhysicsVector3 position, TerraEngine::UUID& objectId) {
			PhysicsBodyCreateRequest request;
			request.handle = handle;
			request.objectId = TerraEngine::GenerateUUID();
			request.objectGeneration = 1U;
			request.componentGeneration = 1U;
			request.shape = shape;
			request.motionType = motionType;
			request.pose.position = position;
			PhysicsBackendStagingToken staging;
			if(!backend.CreateBody(request, staging).Succeeded() || !backend.CommitStaging(staging).Succeeded()) return false;
			objectId = request.objectId;
			return true;
		}

		bool StepAndFindBody(JoltPhysicsBackend& backend, PhysicsWorldHandle world, PhysicsBodyHandle body,
			PhysicsBackendBodyState& state, PhysicsTick lastTick = 180U) {
			for(PhysicsTick tick = 1U; tick <= lastTick; ++tick) {
				if(!backend.Step({world, tick, 1.0F / 60.0F, 1U}).Succeeded()) return false;
			}
			PhysicsBackendExtraction extraction;
			extraction.world = world;
			if(!backend.Extract(extraction).Succeeded()) return false;
			const auto found = std::find_if(extraction.bodies.begin(), extraction.bodies.end(), [&](const auto& value) {
				return value.handle == body;
			});
			if(found == extraction.bodies.end()) return false;
			state = *found;
			return true;
		}

		PhysicsCookedShapeData MakeCacheData(std::uint64_t recipeHash, std::uint8_t seed) {
			PhysicsCookedShapeData data;
			data.recipeHash = recipeHash;
			data.formatFamily = std::string(kJoltCookPayloadFormatFamily);
			data.provider = "Jolt";
			data.providerVersion = std::string(kJoltCookProviderVersion);
			data.providerRevision = std::string(kJoltCookRevision);
			data.formatVersion = kJoltCookPayloadFormatVersion;
			data.payload = {static_cast<std::byte>(seed), static_cast<std::byte>(seed + 1U), static_cast<std::byte>(seed + 2U)};
			data.payloadChecksum = CookedColliderAsset::ComputePayloadChecksum(data.payload);
			return data;
		}
	}

	void RegisterPhysicsCookedShapeSimulationTests(TestRegistry& registry)
	{
		registry.Add("Physics.CookedShapeSimulation.ConvexCookRestoreAndSimulation", [](TestContext& context) {
			BackendFixture fixture;
			context.Expect(fixture.Initialize(), "Jolt 5.6 must initialize before cooking a convex collider.");
			const PhysicsCookResult cooked = fixture.backend.CookShape({fixture.world, MakeConvexRecipe(), "convex-roundtrip"});
			context.Expect(cooked.result.Succeeded() && !cooked.payload.empty(), "A valid convex recipe must cook to opaque bytes.");
			context.Expect(cooked.provider == "Jolt" && cooked.providerVersion == kJoltCookProviderVersion
				&& cooked.providerRevision == kJoltCookRevision && cooked.formatFamily == kJoltCookPayloadFormatFamily,
				"Every convex cook must identify the pinned Jolt 5.6.0 revision and engine payload family.");

			const PhysicsShapeHandle convexShape{{1U}, 1U, 1U, 1U};
			const PhysicsShapeHandle floorShape{{1U}, 1U, 2U, 1U};
			context.Expect(PublishShape(fixture.backend, fixture.world, convexShape, MakeRestoredRecipe(cooked)),
				"A cooked convex payload must restore through the provider-neutral shape API.");
			context.Expect(PublishShape(fixture.backend, fixture.world, floorShape, MakeBoxRecipe(30.0F)),
				"The convex simulation fixture needs a static support shape.");
			TerraEngine::UUID floorId{};
			TerraEngine::UUID bodyId{};
			const PhysicsBodyHandle floor{{1U}, 1U, 1U, 1U};
			const PhysicsBodyHandle body{{1U}, 1U, 2U, 1U};
			context.Expect(PublishBody(fixture.backend, fixture.world, floor, floorShape, PhysicsMotionType::Static, {0.0F, -1.0F, 0.0F}, floorId)
				&& PublishBody(fixture.backend, fixture.world, body, convexShape, PhysicsMotionType::Dynamic, {0.0F, 5.0F, 0.0F}, bodyId),
				"Restored convex geometry must create a dynamic simulation body.");
			PhysicsBackendBodyState state;
			context.Expect(StepAndFindBody(fixture.backend, fixture.world, body, state),
				"A restored convex body must remain extractable during fixed-step simulation.");
			context.Expect(std::isfinite(state.pose.position.y) && state.pose.position.y > -0.2F,
				"The cooked convex body must collide with the static support instead of falling through it.");
		});

		registry.Add("Physics.CookedShapeSimulation.MeshCookRestoreStaticOnlyAndLargeMesh", [](TestContext& context) {
			BackendFixture fixture;
			context.Expect(fixture.Initialize(), "Jolt 5.6 must initialize before cooking mesh colliders.");
			const PhysicsCookResult smallCooked = fixture.backend.CookShape({fixture.world, MakeMeshRecipe(), "mesh-roundtrip"});
			context.Expect(smallCooked.result.Succeeded(), "A non-degenerate triangle mesh must cook successfully.");
			const PhysicsShapeHandle meshShape{{1U}, 1U, 1U, 1U};
			context.Expect(PublishShape(fixture.backend, fixture.world, meshShape, MakeRestoredRecipe(smallCooked)),
				"A cooked triangle mesh must restore as a static-ready shape.");

			PhysicsBodyCreateRequest invalidDynamic;
			invalidDynamic.handle = {{1U}, 1U, 1U, 1U};
			invalidDynamic.objectId = TerraEngine::GenerateUUID();
			invalidDynamic.objectGeneration = 1U;
			invalidDynamic.componentGeneration = 1U;
			invalidDynamic.shape = meshShape;
			invalidDynamic.motionType = PhysicsMotionType::Dynamic;
			PhysicsBackendStagingToken rejectedStaging;
			const PhysicsBackendResult dynamicMesh = fixture.backend.CreateBody(invalidDynamic, rejectedStaging);
			context.Expect(!dynamicMesh.Succeeded() && dynamicMesh.error == PhysicsBackendErrorCode::Unsupported,
				"Triangle-mesh payloads must reject dynamic-body ownership rather than corrupting simulation.");

			const PhysicsCookResult largeCooked = fixture.backend.CookShape({fixture.world, MakeMeshRecipe(64U), "large-mesh-roundtrip"});
			context.Expect(largeCooked.result.Succeeded() && largeCooked.payload.size() <= kCookedColliderMaximumPayloadBytes,
				"A 64x64 grid must cook within the explicit opaque-payload bound.");
			const PhysicsShapeHandle largeMeshShape{{1U}, 1U, 2U, 1U};
			const PhysicsShapeHandle boxShape{{1U}, 1U, 3U, 1U};
			context.Expect(PublishShape(fixture.backend, fixture.world, largeMeshShape, MakeRestoredRecipe(largeCooked))
				&& PublishShape(fixture.backend, fixture.world, boxShape, MakeBoxRecipe()),
				"The large cooked mesh and a dynamic test box must publish together.");
			TerraEngine::UUID meshId{};
			TerraEngine::UUID boxId{};
			const PhysicsBodyHandle meshBody{{1U}, 1U, 2U, 1U};
			const PhysicsBodyHandle boxBody{{1U}, 1U, 3U, 1U};
			context.Expect(PublishBody(fixture.backend, fixture.world, meshBody, largeMeshShape, PhysicsMotionType::Static, {0.0F, 0.0F, 0.0F}, meshId)
				&& PublishBody(fixture.backend, fixture.world, boxBody, boxShape, PhysicsMotionType::Dynamic, {0.0F, 4.0F, 0.0F}, boxId),
				"The restored large mesh must support a dynamic box.");
			PhysicsBackendBodyState state;
			context.Expect(StepAndFindBody(fixture.backend, fixture.world, boxBody, state),
				"Large-mesh fixed-step simulation must remain extractable.");
			context.Expect(state.pose.position.y > -0.2F,
				"The dynamic box must remain supported by the restored large triangle mesh.");
		});

		registry.Add("Physics.CookedShapeSimulation.CompoundCookRestoreAndInputRejection", [](TestContext& context) {
			BackendFixture fixture;
			context.Expect(fixture.Initialize(), "Jolt 5.6 must initialize before compound cooking.");
			const PhysicsCookResult cooked = fixture.backend.CookShape({fixture.world, MakeCompoundRecipe(), "compound-roundtrip"});
			context.Expect(cooked.result.Succeeded(), "A bounded compound of primitive children must cook successfully.");
			const PhysicsShapeHandle compoundShape{{1U}, 1U, 1U, 1U};
			const PhysicsShapeHandle boxShape{{1U}, 1U, 2U, 1U};
			context.Expect(PublishShape(fixture.backend, fixture.world, compoundShape, MakeRestoredRecipe(cooked))
				&& PublishShape(fixture.backend, fixture.world, boxShape, MakeBoxRecipe()),
				"A cooked compound must restore independently of its source children.");
			TerraEngine::UUID compoundId{};
			TerraEngine::UUID boxId{};
			const PhysicsBodyHandle compoundBody{{1U}, 1U, 1U, 1U};
			const PhysicsBodyHandle boxBody{{1U}, 1U, 2U, 1U};
			context.Expect(PublishBody(fixture.backend, fixture.world, compoundBody, compoundShape, PhysicsMotionType::Static, {0.0F, 0.0F, 0.0F}, compoundId)
				&& PublishBody(fixture.backend, fixture.world, boxBody, boxShape, PhysicsMotionType::Dynamic, {-1.5F, 3.0F, 0.0F}, boxId),
				"The restored compound must participate in normal static/dynamic simulation.");
			PhysicsBackendBodyState state;
			context.Expect(StepAndFindBody(fixture.backend, fixture.world, boxBody, state),
				"Compound simulation must publish the dynamic body state.");
			context.Expect(state.pose.position.y > -0.2F,
				"A cooked compound child must support the falling dynamic box.");

			PhysicsShapeRecipe degenerate = MakeMeshRecipe();
			// The first triangle uses vertices 0, 2, and 1. Put vertex 2 on
			// their shared X-axis so this is a true zero-area triangle, not merely
			// a visually unusual but valid mesh.
			degenerate.points[2] = {1.5F, 0.0F, -0.5F};
			const PhysicsCookResult degenerateResult = fixture.backend.CookShape({fixture.world, degenerate, "degenerate-mesh"});
			context.Expect(!degenerateResult.result.Succeeded(), "Degenerate mesh triangles must be rejected before provider cooking.");
			PhysicsShapeRecipe oversized = MakeConvexRecipe();
			oversized.points.front().x = kPhysicsMaximumShapeExtent + 1.0F;
			const PhysicsCookResult oversizedResult = fixture.backend.CookShape({fixture.world, oversized, "oversized-convex"});
			context.Expect(!oversizedResult.result.Succeeded(), "Geometry beyond the configured shape extent must be rejected.");

			PhysicsCookResult validCooked = fixture.backend.CookShape({fixture.world, MakeConvexRecipe(), "corrupt-restore"});
			context.Expect(validCooked.result.Succeeded(), "The corrupt-payload fixture needs a valid Jolt 5.6 source cook.");
			PhysicsShapeRecipe corrupt = MakeRestoredRecipe(validCooked);
			auto corruptPayload = std::make_shared<std::vector<std::byte>>(*corrupt.cookedPayload);
			corruptPayload->front() ^= std::byte{0xFFU};
			corrupt.cookedPayload = corruptPayload;
			PhysicsBackendStagingToken corruptStaging;
			const PhysicsBackendResult corruptResult = fixture.backend.CreateShape(
				fixture.world, {{1U}, 1U, 3U, 1U}, corrupt, corruptStaging);
			context.Expect(!corruptResult.Succeeded(), "Corrupt opaque bytes must fail restoration without creating a live shape.");

			ColliderRecipe scaled;
			scaled.kind = ColliderRecipeKind::ConvexHull;
			scaled.provenance.sourceAssetId = TerraEngine::GenerateUUID();
			scaled.provenance.sourceContentHash = 1U;
			scaled.provenance.settingsHash = 1U;
			scaled.vertices = MakeConvexRecipe().points;
			scaled.sourceScale = {1.0F, 2.0F, 1.0F};
			const ColliderRecipeValidationResult scaleResult = scaled.Validate();
			context.Expect(!scaleResult.accepted && scaleResult.code == ColliderRecipeValidationCode::InvalidScale,
				"Non-uniform source scale must be rejected before a deterministic cook is scheduled.");
		});

		registry.Add("Physics.CookedShapeSimulation.ImmutableCacheDedupReplacementStaleAndRetirement", [](TestContext& context) {
			PhysicsWorld world;
			context.Expect(world.Create(PhysicsSettings::Defaults()), "A live world is required for cooked-shape cache publication.");
			const TerraEngine::UUID firstAsset = TerraEngine::GenerateUUID();
			const TerraEngine::UUID secondAsset = TerraEngine::GenerateUUID();
			const PhysicsCookedShapeData shared = MakeCacheData(0x1001U, 7U);
			context.Expect(world.PublishCookedShape({firstAsset, 1U, shared})
				&& world.PublishCookedShape({secondAsset, 1U, shared}),
				"Two assets with the same normalized cook must publish a shared immutable payload.");
			const auto firstLease = world.AcquireCookedShape(firstAsset);
			const auto secondLease = world.AcquireCookedShape(secondAsset);
			context.Expect(firstLease != nullptr && secondLease != nullptr && firstLease->data == secondLease->data,
				"Deduplicated publications must share one immutable data allocation.");
			context.Expect(world.CookedShapeCacheStatistics().sharedShapeCount == 1U,
				"A deduplicated recipe hash must consume one shared cache entry.");

			const PhysicsCookedShapeData replacement = MakeCacheData(0x1002U, 31U);
			context.Expect(world.PublishCookedShape({firstAsset, 2U, replacement}),
				"A newer asset revision must replace only that asset publication.");
			const auto replacementLease = world.AcquireCookedShape(firstAsset);
			context.Expect(replacementLease != nullptr && replacementLease->data != firstLease->data
				&& firstLease->data->recipeHash == shared.recipeHash,
				"An existing reader must retain its original immutable bytes after replacement.");
			context.Expect(world.MarkCookedShapeRecookFailed(firstAsset, "intentional recook failure"),
				"A failed recook must retain the latest known-good publication.");
			const auto staleLease = world.AcquireCookedShape(firstAsset);
			context.Expect(staleLease != nullptr && staleLease->staleLastKnownGood
				&& staleLease->data == replacementLease->data,
				"Stale-last-known-good state must preserve the replacement payload for readers.");
			context.Expect(world.RetireCookedShape(firstAsset), "Asset retirement must remove the cache owner at a protected boundary.");
			context.Expect(world.AcquireCookedShape(firstAsset) == nullptr && staleLease->data->recipeHash == replacement.recipeHash,
				"Retirement must hide the asset while outstanding reader leases remain safe and immutable.");
			context.Expect(world.RetireCookedShape(secondAsset), "The final shared asset owner must be retireable.");
			context.Expect(world.CookedShapeCacheStatistics().sharedShapeCount == 0U,
				"After all owners retire, cache bookkeeping must release the shared entry without invalidating leases.");
			context.Expect(world.BeginStopping() && world.BeginDraining() == 0U && world.CompleteTeardown(),
				"The cache test world must complete normal lifecycle teardown.");
		});
	}
}
