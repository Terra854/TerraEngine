// Qualify actual mounted terrain recipes in Jolt without widening authored bounds.
#include "TestHarness.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/Terrain/TerrainCollisionSeamPreparation.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ContentMount/TerrainRuntimeProxyLoader.h"
#include "TEPAK/PackWriter.h"
#include "Terrain/TerrainAssetStore.h"
#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		struct SeamFixture
		{
			std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerrainSeamReview-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			ContentMount mount;
			TerraEngine::Physics::Backend::JoltPhysicsBackend backend{ThreadPool::instance()};
			TerraEngine::Physics::PhysicsWorldHandle world{{81U}, 1U};
			std::size_t previousWorkers = ThreadPool::instance().getThreadCount();
			std::vector<TerraEngine::Physics::Backend::PhysicsShapeRepresentation> representations;
			TerraEngine::Physics::PhysicsShapeHandle PublishRecipe(TestContext& context,
				const TerraEngine::Physics::TerrainHeightFieldRecipe& recipe, std::uint32_t slot, float offsetX)
			{
				using namespace TerraEngine::Physics;
				using namespace TerraEngine::Physics::Backend;
				const PhysicsShapeHandle shape{world.worldId, world.worldGeneration, slot, 1u};
				PhysicsBackendStagingToken staging;
				context.Expect(backend.CreateShape(world, shape, MakeTerrainHeightFieldShapeRecipe(recipe), staging).Succeeded(),
					"The actual provider must adopt the prepared candidate.");
				PhysicsBodyCreateRequest body{};
				body.handle = {world.worldId, world.worldGeneration, slot, 1u}; body.shape = shape;
				body.objectId = TerraEngine::GenerateUUID(); body.objectGeneration = body.componentGeneration = 1u;
				body.motionType = PhysicsMotionType::Static; body.pose.position = {offsetX, 0.0f, 0.0f};
				context.Expect(backend.CreateBody(body, staging).Succeeded() && backend.CommitStaging(staging).Succeeded(),
					"Prepared native geometry must become queryable only after actual provider staging commits.");
				return shape;
			}
			float SurfaceHeight(TestContext& context, float x, float z)
			{
				using namespace TerraEngine::Physics;
				Backend::PhysicsBackendQueryRequest query{};
				query.world = world; query.kind = PhysicsQueryKind::RayCast; query.pose.position = {x, 3.0f, z};
				query.direction = {0.0f, -1.0f, 0.0f}; query.distance = 5.0f;
				const auto result = backend.Query(query);
				context.Expect(result.result.Succeeded() && !result.hits.empty(), "Real native geometry must answer the bounded surface query.");
				return result.hits.front().position.y;
			}

			void Initialize(TestContext& context, TerraEngine::Terrain::TerrainSampleBounds bounds,
				std::function<void(TerraEngine::Physics::TerrainHeightFieldRecipe&)> prepare = {})
			{
				using namespace TerraEngine::Physics;
				using namespace TerraEngine::Physics::Backend;
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize(2U);
				const auto content = workspace / "Content";
				const auto data = content / "Terrain/Seam.teterrain.data";
				std::filesystem::create_directories(data / "roots");
				TerraEngine::Terrain::TerrainAssetRoot root;
				root.assetUuid = TerraEngine::GenerateUUID(); root.publicationGeneration = 1U; root.persistedRootGeneration = 1U;
				root.authoredBounds = bounds; root.defaultHeightMm = 0; root.defaultLayerId = 1U; root.layerStackGeneration = 1U;
				const auto encoded = TerraEngine::Terrain::EncodeTerrainRootManifest(root);
				context.Expect(encoded.Succeeded(), "The seam fixture must encode an actual implicit-area terrain root.");
				const auto hash = TerraEngine::Terrain::ToHexString(encoded.contentHash);
				// Canonical hashes cover exact LF bytes. Text-mode CRLF translation
				// would corrupt both the CURRENT hash token and the hashed root payload.
				{ std::ofstream output(data / "CURRENT", std::ios::binary); output << hash << "\n-\n"; }
				{ std::ofstream output(data / "roots" / (hash + ".root"), std::ios::binary); output << encoded.canonicalBytes; }
				{ std::ofstream output(content / "Terrain/Seam.teterrain", std::ios::binary); output << nlohmann::json{
					{"format", "TerraEngineTerrain"}, {"rootFormat", "TERRAROOT1"}, {"version", 1U},
					{"assetId", TerraEngine::ToHexString(root.assetUuid)}, {"width", bounds.maxXExclusive},
					{"height", bounds.maxZExclusive}, {"sampleSpacingMetres", 1.0}}; }
				tepak::PackagingRequest pack;
				pack.inputRoot = content; pack.outputPackPath = workspace / "Seam.tepak";
				pack.workerCount = 1U; pack.enableCompression = false; pack.enableDictionaryTraining = false; pack.overwriteExisting = true;
				(void)tepak::PackWriter::Create(pack);
				context.Expect(mount.Mount(pack.outputPackPath), "The actual terrain bundle must mount before runtime preparation.");
				auto loaded = TerraEngine::Runtime::BuildMountedTerrainRuntime(mount, root.assetUuid, "Terrain/Seam.teterrain");
				const auto expectedAreas = TerraEngine::Terrain::EnumerateTerrainAreas(bounds, 4U);
				const auto completeAreas = std::count_if(loaded.areas.begin(), loaded.areas.end(), [](const auto& area) { return area.Succeeded(); });
				const std::string mountedEvidence = " diagnostic='" + loaded.renderProxy.diagnostic
					+ "' areas=" + std::to_string(loaded.areas.size()) + " complete=" + std::to_string(completeAreas)
					+ " expected=" + std::to_string(expectedAreas ? expectedAreas->size() : 0U)
					+ " rootGeneration=" + std::to_string(loaded.sourcePublicationGeneration)
					+ " compatibilityCollision=" + std::to_string(loaded.collisionRecipe.IsComplete());
				std::cout << "[TERRAIN SEAM MOUNT]" << mountedEvidence << '\n';
				context.Expect(loaded.Succeeded() && expectedAreas && loaded.areas.size() == expectedAreas->size(),
					"Mounted preparation must supply the real per-area collision recipes." + mountedEvidence);
				context.Expect(backend.Initialize().Succeeded(), "The seam fixture requires bundled native Jolt.");
				PhysicsBackendWorldConfiguration configuration; configuration.world = world;
				context.Expect(backend.CreateWorld(configuration).Succeeded(), "The seam fixture must create one real provider world.");
				std::uint32_t slot = 0U;
				for(auto& area : loaded.areas)
				{
					if(prepare) prepare(area.collisionRecipe);
					context.Expect(area.activeRect.maxXExclusive <= 256U && area.activeRect.maxZExclusive <= 256U,
						"Qualification must preserve the authored 256-sample limit.");
					const PhysicsShapeHandle shape{world.worldId, world.worldGeneration, slot, 1U};
					PhysicsBackendStagingToken staging;
					context.Expect(backend.CreateShape(world, shape, MakeTerrainHeightFieldShapeRecipe(area.collisionRecipe), staging).Succeeded(),
						"Every tested shape must come from actual mounted terrain preparation.");
					PhysicsBodyCreateRequest body;
					body.handle = {world.worldId, world.worldGeneration, slot++, 1U}; body.shape = shape;
					body.objectId = TerraEngine::GenerateUUID(); body.objectGeneration = 1U; body.componentGeneration = 1U;
					body.motionType = PhysicsMotionType::Static;
					body.pose.position = {static_cast<float>(area.area.x * 256), 0.0F, static_cast<float>(area.area.z * 256)};
					context.Expect(backend.CreateBody(body, staging).Succeeded() && backend.CommitStaging(staging).Succeeded(),
						"Each adjacent area must publish one real Jolt collision owner.");
					const auto memory = backend.ShapeMemoryForTesting(shape);
					context.Expect(memory.has_value(), "The actual adopted terrain representation must be observable.");
					representations.push_back(memory->representation);
					if(!prepare) context.Expect(memory->representation == PhysicsShapeRepresentation::HeightField
						|| memory->representation == PhysicsShapeRepresentation::Empty,
						"Boundary-only and partial-area masks must retain the exact compressed heightfield fast path.");
				}
			}

			void ExpectSupport(TestContext& context, float x, float z, bool supported)
			{
				using namespace TerraEngine::Physics;
				using namespace TerraEngine::Physics::Backend;
				PhysicsBackendQueryRequest query;
				query.world = world; query.kind = PhysicsQueryKind::ShapeCast; query.hitMode = PhysicsQueryHitMode::All;
				query.pose.position = {x, 2.0F, z}; query.direction = {0.0F, -1.0F, 0.0F};
				query.shapeExtent = {0.1F, 0.1F, 0.1F}; query.distance = 4.0F; query.maximumHits = 16U;
				const auto result = backend.Query(query);
				context.Expect(result.result.Succeeded(), "The actual Jolt seam sweep must execute normally.");
				std::vector<PhysicsBodyHandle> owners;
				for(const auto& hit : result.hits)
					if(std::find(owners.begin(), owners.end(), hit.body) == owners.end()) owners.push_back(hit.body);
				context.Expect(owners.size() == (supported ? 1U : 0U),
					"Seam sweep at (" + std::to_string(x) + ", " + std::to_string(z)
					+ ") must have exactly the declared collision owner count, without gaps or unsupported extension. actualOwners="
					+ std::to_string(owners.size()) + " expectedOwners=" + std::to_string(supported ? 1U : 0U)
					+ " providerHits=" + std::to_string(result.hits.size()));
			}

			~SeamFixture()
			{
				(void)backend.DestroyWorld(world); (void)backend.Shutdown();
				mount.Unmount();
				ThreadPool::instance().shutdown(); if(previousWorkers != 0U) ThreadPool::instance().initialize(previousWorkers);
				std::error_code ignored; std::filesystem::remove_all(workspace, ignored);
			}
		};
	}

	void RegisterTerrainSeamOwnershipTests(TestRegistry& registry)
	{
		registry.Add("Physics.TerrainSeam.HoleFallbackSharesFiniteHorizontalTerrainBounds", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			using namespace TerraEngine::Physics::Backend;
			SeamFixture fixture; fixture.Initialize(context, {0,0,256,256});
			std::uint32_t slot = 20U;
			// 256 cells at 4m reach1024m, beyond the primitive1000m extent cap.
			// The exact-boundary and smaller controls keep rejection provenance clear.
			for(const float spacing : {3.0F, 1000.0F / 256.0F, 4.0F}) {
				for(const bool hole : {false, true}) {
					TerrainHeightFieldRecipe recipe;
					recipe.terrainAssetId = TerraEngine::GenerateUUID(); recipe.authoredGeneration = recipe.authoredHash = 1U;
					recipe.sampleSpacingMetres = spacing;
					recipe.heightMillimetres.assign(recipe.kSampleCount, 0); recipe.holeBits.assign(recipe.kHoleByteCount, 0U);
					const auto cell = 10U * 256U + 10U;
					if(hole) recipe.holeBits[cell / 8U] |= static_cast<std::uint8_t>(1U << (cell % 8U));
					const float offset = 2000.0F * static_cast<float>(slot - 19U);
					const auto shape = fixture.PublishRecipe(context, recipe, slot++, offset);
					const auto memory = fixture.backend.ShapeMemoryForTesting(shape);
					context.Expect(memory && memory->representation == (hole ? PhysicsShapeRepresentation::TriangleMesh : PhysicsShapeRepresentation::HeightField),
						"The real isolated-hole case must use mesh fallback; its no-hole control must use heightfield.");
					context.Expect(std::fabs(fixture.SurfaceHeight(context, offset + 255.5F * spacing, 20.5F * spacing)) < 0.001F,
						"Both representations must preserve the supported far edge beyond primitive extents.");
					PhysicsBackendQueryRequest query; query.world = fixture.world; query.kind = PhysicsQueryKind::RayCast;
					query.pose.position = {offset + 10.5F * spacing, 2.0F, 10.5F * spacing};
					query.direction = {0.0F, -1.0F, 0.0F}; query.distance = 4.0F;
					const auto result = fixture.backend.Query(query);
					context.Expect(result.result.Succeeded() && result.hits.empty() == hole,
						"Expanding finite horizontal acceptance must retain the exact authored cell hole.");
				}
			}
		});
		registry.Add("Physics.TerrainSeam.NonfiniteHorizontalSpanAndMeshHeightRemainRejected", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			using namespace TerraEngine::Physics::Backend;
			SeamFixture fixture; fixture.Initialize(context, {0,0,256,256});
			TerrainHeightFieldRecipe recipe;
			recipe.terrainAssetId = TerraEngine::GenerateUUID(); recipe.authoredGeneration = recipe.authoredHash = 1U;
			recipe.heightMillimetres.assign(recipe.kSampleCount, 0); recipe.holeBits.assign(recipe.kHoleByteCount, 0U);
			std::uint32_t slot = 40U;
			const auto reject = [&](const TerrainHeightFieldRecipe& invalid) {
				PhysicsBackendStagingToken staging;
				const PhysicsShapeHandle handle{fixture.world.worldId, fixture.world.worldGeneration, slot++, 1U};
				const auto result = fixture.backend.CreateShape(fixture.world, handle, MakeTerrainHeightFieldShapeRecipe(invalid), staging);
				if(staging.IsValid()) (void)fixture.backend.RollbackStaging(staging);
				context.Expect(!result.Succeeded(), "Invalid numeric terrain must reject before native shape publication.");
			};
			for(const bool hole : {false, true}) {
				recipe.holeBits.assign(recipe.kHoleByteCount, 0U);
				if(hole) recipe.holeBits[0] = 2U;
				recipe.sampleSpacingMetres = std::numeric_limits<float>::max(); reject(recipe);
				recipe.sampleSpacingMetres = 1.0e8F; reject(recipe);
				recipe.sampleSpacingMetres = std::numeric_limits<float>::infinity(); reject(recipe);
			}
			// Both representations use terrain arithmetic bounds. High but ordinary
			// elevations are valid; finite values beyond safe cross-product ranges
			// and nonfinite datums must still reject before native publication.
			recipe.sampleSpacingMetres = 4.0F; recipe.holeBits.assign(recipe.kHoleByteCount, 0U);
			for(const bool hole : {false, true}) {
				recipe.holeBits[0] = hole ? 2U : 0U;
				recipe.heightDatumMetres = 1.0e10F; reject(recipe);
				recipe.heightDatumMetres = std::numeric_limits<float>::infinity(); reject(recipe);
			}
		});
		registry.Add("Physics.TerrainSeam.FullAdjacentXHasExactlyOneOwner", [](TestContext& context) {
			SeamFixture fixture; fixture.Initialize(context, {0, 0, 512, 256});
			for(float x : {10.25F, 253.25F, 255.25F, 255.75F, 256.25F, 258.25F}) fixture.ExpectSupport(context, x, 10.25F, true);
		});
		registry.Add("Physics.TerrainSeam.FullAdjacentZHasExactlyOneOwner", [](TestContext& context) {
			SeamFixture fixture; fixture.Initialize(context, {0, 0, 256, 512});
			for(float z : {10.25F, 253.25F, 255.25F, 255.75F, 256.25F, 258.25F}) fixture.ExpectSupport(context, 10.25F, z, true);
		});
		registry.Add("Physics.TerrainSeam.PartialNeighborDoesNotExtendAuthoredCoverage", [](TestContext& context) {
			SeamFixture fixture; fixture.Initialize(context, {0, 0, 300, 256});
			fixture.ExpectSupport(context, 270.25F, 10.25F, true);
			fixture.ExpectSupport(context, 300.25F, 10.25F, false);
			fixture.ExpectSupport(context, 255.5F, 10.25F, true);
		});
		registry.Add("Physics.TerrainSeam.MissingNeighborNeverCreatesSupport", [](TestContext& context) {
			SeamFixture fixture; fixture.Initialize(context, {0, 0, 256, 256});
			fixture.ExpectSupport(context, 10.25F, 10.25F, true);
			fixture.ExpectSupport(context, 255.5F, 10.25F, false);
			fixture.ExpectSupport(context, 256.25F, 10.25F, false);
		});
		registry.Add("Physics.TerrainSeam.FourAreaCornerHasExactlyOneOwner", [](TestContext& context) {
			SeamFixture fixture; fixture.Initialize(context, {0, 0, 512, 512});
			for(float x : {255.25F, 255.75F, 256.25F})
				for(float z : {255.25F, 255.75F, 256.25F}) fixture.ExpectSupport(context, x, z, true);
		});
		registry.Add("Physics.TerrainSeam.InternalHoleDoesNotRemoveAdjacentCells", [](TestContext& context) {
			SeamFixture fixture;
			fixture.Initialize(context, {0, 0, 300, 40}, [&context](auto& recipe) {
				if(recipe.areaX != 0) return;
				const std::uint32_t cell = 10u * 256u + 10u;
				recipe.holeBits[cell / 8u] |= static_cast<std::uint8_t>(1u << (cell % 8u));
				recipe.cookedCollision.reset();
				context.Expect(TerraEngine::Physics::Terrain::CookTerrainCollisionRecipe(recipe), "The real worker cooker must prepare an exact internal-cell hole.");
			});
			context.Expect(!fixture.representations.empty()
				&& fixture.representations.front() == TerraEngine::Physics::Backend::PhysicsShapeRepresentation::TriangleMesh,
				"An isolated internal cell hole must retain the exact mesh fallback.");
			fixture.ExpectSupport(context, 10.5F, 10.5F, false);
			fixture.ExpectSupport(context, 9.5F, 10.5F, true);
			fixture.ExpectSupport(context, 10.5F, 9.5F, true);
			fixture.ExpectSupport(context, 11.5F, 10.5F, true);
			fixture.ExpectSupport(context, 10.5F, 11.5F, true);
			fixture.ExpectSupport(context, 255.5F, 10.5F, true);
		});
		registry.Add("Physics.TerrainSeam.NeighborOnlyEditRecooksAndRejectedCaptureIsAtomic", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			using namespace TerraEngine::Physics::Terrain;
			// Establish the actual worker/provider lifetime before direct worker
			// cooking; earlier fixture cleanup intentionally restores an empty pool.
			SeamFixture native;
			native.Initialize(context, {0,0,256,256});
			TerrainHeightFieldRecipe base{};
			base.terrainAssetId = TerraEngine::GenerateUUID(); base.authoredGeneration = 1u; base.authoredHash = 17u;
			base.heightMillimetres.assign(base.kSampleCount, 0); base.holeBits.assign(base.kHoleByteCount, 0u);
			TerrainCollisionRootCapture captured{};
			captured.assetId = base.terrainAssetId; captured.rootHash = {23u, 27u}; captured.generation = 1u;
			auto own = std::make_shared<const std::vector<std::int32_t>>(65536u, 0);
			captured.areas.emplace(TerraEngine::Terrain::TerrainAreaCoordinate{0,0}, TerrainCollisionAreaCapture{{0,0,256,256}, own});
			captured.areas.emplace(TerraEngine::Terrain::TerrainAreaCoordinate{1,0}, TerrainCollisionAreaCapture{{0,0,256,256}, own});
			captured.areas.emplace(TerraEngine::Terrain::TerrainAreaCoordinate{0,1}, TerrainCollisionAreaCapture{{0,0,256,256}, own});
			auto first = base;
			context.Expect(PrepareTerrainCollisionSeams(first, captured), "First same-root capture must produce supported detached seam samples.");
			context.Expect(CookTerrainCollisionRecipe(first), "First same-root seam candidate must cook through the actual provider path.");
			captured.rootHash.high64 = 28u; captured.generation = 2u;
			captured.areas.at({1,0}).heights = std::make_shared<const std::vector<std::int32_t>>(65536u, 1000);
			auto second = base;
			context.Expect(PrepareTerrainCollisionSeams(second, captured) && CookTerrainCollisionRecipe(second), "Neighbor-only change must produce a fresh complete cook.");
			context.Expect(first.authoredHash == second.authoredHash && first.authoredGeneration == second.authoredGeneration
				&& first.cookedCollision->recipeHash != second.cookedCollision->recipeHash
				&& first.heightMillimetres[256u] == 0 && second.heightMillimetres[256u] == 1000,
				"Unchanged own authored identity must not hide new immutable neighbor geometry from the shape cache.");
			(void)native.PublishRecipe(context, first, 10u, 1000.0f);
			(void)native.PublishRecipe(context, second, 11u, 2000.0f);
			context.Expect(std::abs(native.SurfaceHeight(context, 1255.5f, 10.5f)) < 0.02f
				&& std::abs(native.SurfaceHeight(context, 2255.5f, 10.5f) - 0.5f) < 0.02f,
				"The actual provider cache must expose the newly captured neighbor slope rather than the prior flat edge.");
			// Both edge neighbors exist but the diagonal does not. Only the corner
			// cell lacks its fourth endpoint; adjacent seam cells remain supported.
			native.ExpectSupport(context, 1255.5f, 255.5f, false);
			native.ExpectSupport(context, 1010.5f, 255.5f, true);
			const auto before = second;
			captured.areas.at({1,0}).heights = std::make_shared<const std::vector<std::int32_t>>(1u, 0);
			context.Expect(!PrepareTerrainCollisionSeams(second, captured) && second.heightMillimetres == before.heightMillimetres
				&& second.holeBits == before.holeBits && second.cookedCollision == before.cookedCollision,
				"Rejected malformed neighbor capture must preserve every prepared recipe field.");
			TerrainCollisionSnapshot snapshot{}; snapshot.currentRecipe = first;
			auto stale = PrepareMountedTerrainCollisionCandidate(before, {99u, 99u}, 2u);
			bool providerCalled = false;
			context.Expect(!PublishMountedTerrainCollisionCandidate(snapshot, stale, [&providerCalled](auto) {
				providerCalled = true; return TerrainCollisionReplacementReceipt{};
			}) && !providerCalled && snapshot.currentRecipe.cookedCollision == first.cookedCollision,
				"Mismatched same-root evidence must be rejected before provider work and retain prior geometry.");
		});
		registry.Add("Physics.TerrainSeam.BoundedMeshCookQueryAndMemoryComparison", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			SeamFixture fixture; fixture.Initialize(context, {0,0,256,256});
			TerrainHeightFieldRecipe complete{};
			complete.terrainAssetId = TerraEngine::GenerateUUID(); complete.authoredGeneration = complete.authoredHash = 1u;
			complete.heightMillimetres.assign(complete.kSampleCount, 0); complete.holeBits.assign(complete.kHoleByteCount, 0u);
			// A saddle quad distinguishes NW/SE from the opposite diagonal. Matching
			// queries across representations prove nonplanar geometry stays identical.
			complete.heightMillimetres[20u * 257u + 21u] = 1000;
			complete.heightMillimetres[21u * 257u + 20u] = 1000;
			auto boundary = complete, partial = complete, sparse = complete;
			const auto maskOutside = [](auto& recipe, std::uint32_t maxCellX, std::uint32_t maxCellZ) {
				for(std::uint32_t z = 0u; z < 256u; ++z)
					for(std::uint32_t x = 0u; x < 256u; ++x) {
						if(x < maxCellX && z < maxCellZ) continue;
						const auto cell = z * 256u + x;
						recipe.holeBits[cell / 8u] |= static_cast<std::uint8_t>(1u << (cell % 8u));
					}
			};
			maskOutside(boundary, 255u, 255u); maskOutside(partial, 199u, 179u);
			const auto isolatedHole = 10u * 256u + 100u;
			sparse.holeBits[isolatedHole / 8u] |= static_cast<std::uint8_t>(1u << (isolatedHole % 8u));
			std::uint32_t slot = 10u;
			for(auto* recipe : {&complete, &boundary, &partial, &sparse}) {
				const auto start = std::chrono::steady_clock::now();
				context.Expect(TerraEngine::Physics::Terrain::CookTerrainCollisionRecipe(*recipe), "Both representative bounded forms must cook with production Jolt.");
				const auto cookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				const bool mesh = recipe == &sparse;
				const float offset = 1000.0f * static_cast<float>(slot - 9u);
				const auto adopted = fixture.PublishRecipe(context, *recipe, slot++, offset);
				const auto memory = fixture.backend.ShapeMemoryForTesting(adopted);
				context.Expect(memory && memory->nativeSizeBytes != 0u, "The adopted native shape must expose actual vendor memory statistics.");
				context.Expect(memory->worldStrongCacheEntries == 0u,
					"Terrain adoption must not add historical native geometry to the legacy world-lifetime strong cache.");
				context.Expect(memory->representation == (mesh ? TerraEngine::Physics::Backend::PhysicsShapeRepresentation::TriangleMesh
					: TerraEngine::Physics::Backend::PhysicsShapeRepresentation::HeightField),
					"Complete, boundary-only, and partial masks must use compressed heightfields; isolated internal holes require exact meshes.");
				context.Expect(std::abs(fixture.SurfaceHeight(context, offset + 20.25f, 20.25f)) < 0.02f
					&& std::abs(fixture.SurfaceHeight(context, offset + 20.75f, 20.25f) - 0.5f) < 0.02f
					&& std::abs(fixture.SurfaceHeight(context, offset + 20.25f, 20.75f) - 0.5f) < 0.02f,
					"Native saddle-cell queries away from the hole must preserve the same NW/SE triangles in every representation.");
				if(recipe == &boundary) {
					fixture.ExpectSupport(context, offset + 254.5f, 10.5f, true);
					fixture.ExpectSupport(context, offset + 255.5f, 10.5f, false);
				}
				if(recipe == &partial) {
					fixture.ExpectSupport(context, offset + 198.5f, 10.5f, true);
					fixture.ExpectSupport(context, offset + 199.5f, 10.5f, false);
				}
				const auto queryStart = std::chrono::steady_clock::now();
				for(unsigned query = 0u; query < 128u; ++query)
					context.Expect(std::abs(fixture.SurfaceHeight(context, offset + 10.5f + query % 32u, 10.5f)) < 0.02f,
						"Both forms must answer representative native surface queries.");
				const auto queryMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - queryStart).count();
				context.Expect(recipe->cookedCollision->payload->size() <= 64u * 1024u * 1024u,
					"Representative cooked output must remain within the existing provider payload bound.");
				const char* label = mesh ? "cell-exact-mesh" : recipe == &boundary ? "boundary-heightfield"
					: recipe == &partial ? "partial-heightfield" : "complete-heightfield";
				std::cout << "[TERRAIN COLLISION COMPARISON] representation=" << label
					<< " cookMs=" << cookMs << " query128Ms=" << queryMs
					<< " payloadBytes=" << recipe->cookedCollision->payload->size()
					<< " derivedGeometryBoundBytes=" << recipe->cookedCollision->derivedGeometryBytes
					<< " vendorStatsBytes=" << memory->nativeSizeBytes << " vendorTriangles=" << memory->triangleCount << '\n';
			}
		});
	}
}
