// TerrainCollisionAlignmentTests.cpp -- Editor parity contracts for hole and collision status.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainHoleBrush.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainPicking.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainPageFormat.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainDiagnostics.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace TerraEngine::EditorTests
{
	void RegisterTerrainCollisionAlignmentTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainCollision.HoleParityUsesOneCanonicalCellMask", [](TestContext& context) {
			using namespace TerraEngine::Editor::Terrain;
			std::array<std::uint8_t, 8u> source{};
			std::array<std::uint8_t, 8u> painted{};
			TerrainHoleBrushSettings settings; settings.mode = TerrainHoleBrushMode::PaintHole; settings.radiusCells = 0.6F;
			context.Expect(TerrainHoleBrush::ApplyStamp(source, painted, 8u, 8u, settings, {3.5F, 4.5F}),
				"A bounded hole stamp must change its canonical cell mask.");
			// The compact brush test proves bit addressing; the production packet test
			// below proves the renderer, CPU picker, and collision recipe consume it.
			context.Expect(TerrainHoleBrush::IsHole(painted, 8u, 8u, 3u, 4u),
				"The brush must set the requested canonical hole bit before consumers observe it.");
		});

		registry.Add("Editor.TerrainCollision.HoleRadiusClampsToOneCell", [](TestContext& context) {
			using namespace TerraEngine::Editor::Terrain;
			std::array<std::uint8_t, 8u> source{};
			std::array<std::uint8_t, 8u> painted{};
			TerrainHoleBrushSettings settings;
			settings.mode = TerrainHoleBrushMode::PaintHole;
			settings.radiusCells = 0.25F;
			const TerrainHoleBrushStamp stamp = TerrainHoleBrush::CenteredOnCell(3, 4);

			context.Expect(stamp.centerCellX == 3.5F && stamp.centerCellZ == 4.5F,
				"A picked integer cell must be converted to its canonical centre before stamping.");
			context.Expect(TerrainHoleBrush::EffectiveRadiusCells(settings.radiusCells) == 1.0F,
				"Hole-brush requests below one cell must resolve to the canonical minimum radius.");
			context.Expect(TerrainHoleBrush::ApplyStamp(source, painted, 8u, 8u, settings, stamp)
				&& TerrainHoleBrush::IsHole(painted, 8u, 8u, 3u, 4u)
				&& TerrainHoleBrush::IsHole(painted, 8u, 8u, 4u, 4u)
				&& !TerrainHoleBrush::IsHole(painted, 8u, 8u, 4u, 5u),
				"The kernel must apply a one-cell radius when a smaller value reaches it.");
		});

		registry.Add("Editor.TerrainCollision.AlignmentLatencyAndLastKnownGoodStatusAreVisible", [](TestContext& context) {
			std::vector<std::int32_t> renderedHeights(TerraEngine::Terrain::kTerrainAreaSampleCount, 0);
			for(std::size_t index = 0u; index < renderedHeights.size(); ++index) renderedHeights[index] = static_cast<std::int32_t>(index % 256u) * 10;
			std::vector<std::int32_t> renderHalo(
				TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent
					* TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent,
				0);
			// Clamp the synthetic single-area fixture at its outer boundary while
			// retaining the exact authored samples at halo coordinates [1, 256].
			for(std::uint32_t haloZ = 0u; haloZ < TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent; ++haloZ)
				for(std::uint32_t haloX = 0u; haloX < TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent; ++haloX)
				{
					const std::uint32_t sourceX = std::min<std::uint32_t>(
						haloX == 0u ? 0u : haloX - 1u, 255u);
					const std::uint32_t sourceZ = std::min<std::uint32_t>(
						haloZ == 0u ? 0u : haloZ - 1u, 255u);
					renderHalo[static_cast<std::size_t>(haloZ)
						* TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent + haloX]
						= renderedHeights[static_cast<std::size_t>(sourceZ) * 256u + sourceX];
				}
			std::vector<std::uint8_t> holes(8192u, 0u);
			const std::size_t holeIndex = 12u * 256u + 11u; holes[holeIndex / 8u] |= static_cast<std::uint8_t>(1u << (holeIndex % 8u));
			TerraEngine::Renderer::Terrain::TerrainCookInput renderInput;
			renderInput.assetId = TerraEngine::GenerateUUID(); renderInput.authoredHash = {9u, 10u}; renderInput.authoredGeneration = 18u;
			renderInput.heightMillimetres = renderHalo; renderInput.holeCellBits = holes;
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(renderInput);
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe;
			recipe.terrainAssetId = renderInput.assetId; recipe.authoredGeneration = renderInput.authoredGeneration; recipe.authoredHash = renderInput.authoredHash.low64;
			recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			for(std::uint32_t z = 0u; z < 256u; ++z) for(std::uint32_t x = 0u; x < 256u; ++x)
				recipe.heightMillimetres[static_cast<std::size_t>(z) * 257u + x] = renderedHeights[static_cast<std::size_t>(z) * 256u + x];
			recipe.holeBits = holes;
			const auto picked = TerraEngine::Editor::Terrain::TerrainPicking::PickHeightfield({{20.0, 100.0, 20.0}, {0.0, -1.0, 0.0}},
				[&](std::int64_t x, std::int64_t z) -> std::optional<double> {
					if(x < 0 || z < 0 || x >= 256 || z >= 256) return std::nullopt;
					const std::size_t index = static_cast<std::size_t>(z) * 256u + static_cast<std::size_t>(x);
					if((recipe.holeBits[index / 8u] & (1u << (index % 8u))) != 0u) return std::nullopt;
					return static_cast<double>(recipe.heightMillimetres[static_cast<std::size_t>(z) * 257u + static_cast<std::size_t>(x)]) / 1000.0;
				}, 200.0);
			const double renderedHeightMetres = static_cast<double>(renderedHeights[20u * 256u + 20u]) / 1000.0;
			const double collisionHeightMetres = static_cast<double>(recipe.heightMillimetres[20u * 257u + 20u]) / 1000.0;
			TerraEngine::Physics::Terrain::TerrainCollisionSnapshot source;
			source.status = TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current;
			source.sourceRootHash = {9u, 10u}; source.sourcePublicationGeneration = 18u;
			source.currentRecipe = recipe;
			source.ageMilliseconds = 75u; source.buildLatencyMilliseconds = 999u;
			const TerraEngine::Terrain::TerrainCollisionRecord record = TerraEngine::Terrain::MakeTerrainCollisionRecord(source);
			const bool boundaryRefined = std::any_of(cooked.meshlets.begin(), cooked.meshlets.end(), [](const auto& meshlet) { return meshlet.hasHoleBoundary; })
				&& std::any_of(cooked.lodNodes.begin(), cooked.lodNodes.end(), [](const auto& node) { return node.refinesHoleBoundary; });
			context.Expect(cooked.complete && picked && boundaryRefined && std::abs(renderedHeightMetres - collisionHeightMetres) <= 0.05
				&& record.status == TerraEngine::Terrain::TerrainCollisionStatus::Current
				&& record.sourceRootHash == source.sourceRootHash
				&& record.activeSourcePublicationGeneration == source.currentRecipe.authoredGeneration
				&& record.buildLatencyMilliseconds <= 1000u,
				"A current collision product must identify the authored source, align within five centimetres, and finish a normal stroke within one second.");
			source.status = TerraEngine::Physics::Terrain::TerrainCollisionStatus::Stale;
			source.lastKnownGoodRecipe = source.currentRecipe;
			const auto stale = TerraEngine::Terrain::MakeTerrainCollisionRecord(source);
			context.Expect(stale.status == TerraEngine::Terrain::TerrainCollisionStatus::Stale && stale.lastKnownGoodActive,
				"A stale rebuild must explicitly report that queries still use the last-known-good collision identity.");
		});

		registry.Add("Editor.TerrainCollision.HoleEraseRestoresCanonicalCell", [](TestContext& context) {
			using namespace TerraEngine::Editor::Terrain;
			std::array<std::uint8_t, 8u> source{};
			std::array<std::uint8_t, 8u> holes{};
			std::array<std::uint8_t, 8u> erased{};
			TerrainHoleBrushSettings paint; paint.mode = TerrainHoleBrushMode::PaintHole; paint.radiusCells = 0.6F;
			TerrainHoleBrushSettings erase = paint; erase.mode = TerrainHoleBrushMode::EraseHole;
			const bool paintedCell = TerrainHoleBrush::ApplyStamp(source, holes, 8u, 8u, paint, {1.5F, 1.5F});
			context.Expect(paintedCell && TerrainHoleBrush::ApplyStamp(holes, erased, 8u, 8u, erase, {1.5F, 1.5F})
				&& !TerrainHoleBrush::IsHole(erased, 8u, 8u, 1u, 1u),
				"Erasing a hole must restore the same canonical cell without modifying neighbouring bits.");
		});

		registry.Add("Editor.TerrainCollision.ProtectedPublicationRejectsStaleRecipeAndKeepsLastKnownGood", [](TestContext& context) {
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 3u;
			root->rootContentHash = {42u, 43u}; root->authoredBounds = {0, 0, 256, 256}; root->layerStackGeneration = 1u;
			TerraEngine::Terrain::TerrainAssetStore store(root);
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe;
			recipe.terrainAssetId = root->assetUuid; recipe.authoredGeneration = root->publicationGeneration; recipe.authoredHash = root->rootContentHash.low64;
			recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);
			const auto started = std::chrono::steady_clock::now();
			auto current = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(recipe, root->rootContentHash, root->publicationGeneration);
			current.buildLatencyMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			TerraEngine::Physics::Terrain::TerrainCollisionSnapshot snapshot;
			context.Expect(TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(snapshot, current, store)
				&& snapshot.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current
				&& snapshot.buildLatencyMilliseconds <= 1000.0,
				"A normal hole-stroke collision candidate must receive a current protected-boundary status within one second.");
			auto stale = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(recipe, {99u, 100u}, root->publicationGeneration + 1u);
			context.Expect(!TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(snapshot, stale, store)
				&& snapshot.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Stale
				&& snapshot.currentRecipe.IsComplete(),
				"A stale collision candidate must be rejected while the complete last-known-good collision remains queryable.");
		});
	}
}
