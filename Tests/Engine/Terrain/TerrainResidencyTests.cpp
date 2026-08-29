// TerrainResidencyTests.cpp -- demand, protection, and stale publication checks.

#include "TestHarness.h"

#include "Terrain/TerrainDiagnostics.h"
#include "Terrain/TerrainStreamingSystem.h"
#include "Renderer/RHI/Vulkan/VulkanSamplerPolicy.h"
#include "Renderer/Terrain/TerrainLodSelection.h"
#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainPageFormat.h"
#include "Renderer/Terrain/TerrainRenderPackets.h"
#include "Resource/ResourceStreaming.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <unordered_set>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterTerrainResidencyTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Residency.ProtectedDemandSurvivesBudgetAndStalePlansReject", [](TestContext& context) {
			Terrain::TerrainResidencySnapshot prior{};
			const auto asset = TerraEngine::GenerateUUID();
			Terrain::TerrainResidencyDemand edit{};
			edit.assetId = asset; edit.area = {0, 0}; edit.rootGeneration = 4u;
			edit.source = Terrain::TerrainDemandSource::ActiveEdit; edit.priority = 100u; edit.authoredBytes = 80u;
			Terrain::TerrainResidencyDemand scene{};
			scene.assetId = asset; scene.area = {1, 0}; scene.rootGeneration = 4u;
			scene.source = Terrain::TerrainDemandSource::SceneView; scene.priority = 1u; scene.authoredBytes = 80u;
			Terrain::TerrainResidencyBudget budget{};
			budget.authoredBytes = 100u; budget.renderBytes = budget.controlBytes = budget.collisionBytes = 1024u;
			const auto plan = Terrain::PrepareTerrainResidencyPlan(4u, prior, {edit, scene}, budget);
			context.Expect(plan.evictAreas.size() == 1u && plan.candidate.areas.size() == 1u && plan.candidate.areas.front().area == edit.area,
				"Budget pressure must evict ordinary detail before an active edit area.");
			Terrain::TerrainResidencySnapshot published{};
			context.Expect(!Terrain::PublishTerrainResidencyPlan(published, plan, 5u) && published.areas.empty(),
				"A plan prepared from a stale root must leave the previous residency snapshot untouched.");
			context.Expect(Terrain::PublishTerrainResidencyPlan(published, plan, 4u) && published.areas.size() == 1u,
				"A current immutable plan must publish all selected residency at one boundary.");
		});

		registry.Add("Terrain.Residency.CropForcesProtectedAreaRetirementAtPublication", [](TestContext& context) {
			Terrain::TerrainResidencySnapshot prior{};
			prior.rootGeneration = 4u;
			const auto asset = TerraEngine::GenerateUUID();
			prior.areas.push_back({asset, {0, 0}, 4u, {1u, 2u}, 10u, 10u, 10u, 10u, 100u, true, true});
			prior.areas.push_back({asset, {1, 0}, 4u, {3u, 4u}, 10u, 10u, 10u, 10u, 100u, true, false});
			const std::array<Terrain::TerrainAreaCoordinate, 1u> forced{{{1, 0}}};
			const auto plan = Terrain::PrepareTerrainResidencyPlan(5u, prior, {}, {}, nullptr, forced);
			context.Expect(plan.candidate.areas.size() == 1u && plan.evictAreas == std::vector<Terrain::TerrainAreaCoordinate>{{1, 0}}
				&& prior.areas.size() == 2u,
				"Crop retirement must remove even pinned old coverage from the detached candidate while the published snapshot stays intact.");
			Terrain::TerrainResidencySnapshot published = prior;
			context.Expect(Terrain::PublishTerrainResidencyPlan(published, plan, 5u) && published.areas.size() == 1u,
				"Forced retirement becomes visible only with the complete matching-generation residency swap.");
		});

		registry.Add("Terrain.Residency.CoarseCoverageEnumeratesImplicitSignedPartialAreas", [](TestContext& context) {
			const auto asset = TerraEngine::GenerateUUID();
			const auto demands = Terrain::BuildTerrainCoarseCoverageDemands(
				asset, 7u, {-1, -257, 257, 1}, 16u, 8u);
			Terrain::TerrainResidencyBudget budget{};
			budget.renderBytes = 80u;
			const auto plan = demands
				? Terrain::PrepareTerrainResidencyPlan(7u, {}, *demands, budget)
				: Terrain::TerrainResidencyPlan{};
			context.Expect(demands && demands->size() == 9u
				&& std::all_of(demands->begin(), demands->end(), [](const auto& demand) { return demand.regionalRoot; })
				&& plan.candidate.areas.size() == 9u && plan.candidate.hasCoherentFallbackCoverage,
				"Sparse roots must expose coarse fallback for every signed full or partial in-bounds area without materializing authored blobs.");
			context.Expect(!Terrain::BuildTerrainCoarseCoverageDemands(asset, 7u,
				{-1, -257, 257, 1}, 5u, 8u),
				"Coarse coverage enumeration must reject before exceeding its bounded area budget.");
		});

		registry.Add("Terrain.Residency.AggregatesEveryDemandClassAndRetainsOnlyMatchingLkg", [](TestContext& context) {
			const auto asset = TerraEngine::GenerateUUID();
			Terrain::TerrainResidencyBudget budget{};
			budget.authoredBytes = 1'000u; budget.renderBytes = budget.controlBytes = budget.collisionBytes = 1'000u;
			std::vector<Terrain::TerrainResidencyDemand> demands;
			for (const auto source : {Terrain::TerrainDemandSource::SceneView, Terrain::TerrainDemandSource::GameView, Terrain::TerrainDemandSource::Shadow,
				Terrain::TerrainDemandSource::Reflection, Terrain::TerrainDemandSource::Capture, Terrain::TerrainDemandSource::Selection,
				Terrain::TerrainDemandSource::ActiveEdit, Terrain::TerrainDemandSource::Play, Terrain::TerrainDemandSource::ExplicitPin})
			{
				Terrain::TerrainResidencyDemand demand{};
				demand.assetId = asset; demand.area = {static_cast<std::int64_t>(demands.size()), 0}; demand.rootGeneration = 8u;
				demand.source = source; demand.priority = static_cast<std::uint32_t>(demands.size()); demand.authoredBytes = 10u;
				demands.push_back(demand);
			}
			Terrain::TerrainResidencyDemand duplicate = demands.front(); duplicate.priority = 99u; demands.push_back(duplicate);
			const auto plan = Terrain::PrepareTerrainResidencyPlan(8u, {}, demands, budget);
			const auto protectedCount = std::count_if(plan.candidate.areas.begin(), plan.candidate.areas.end(), [](const auto& area) { return area.protectedFromEviction; });
			context.Expect(plan.candidate.areas.size() == 9u && plan.candidate.coalescedDemands == 1u && protectedCount == 4u,
				"Scene, game, capture, shadow, reflection, selection, edit, play, and pin demand must coalesce while preserving all protected sources.");

			std::atomic_bool cancelled{true};
			const auto cancelledPlan = Terrain::PrepareTerrainResidencyPlan(8u, {}, demands, budget, &cancelled);
			context.Expect(cancelledPlan.cancelled, "Cancelled IO/planning work must not produce a publishable residency candidate.");

			std::vector<std::int32_t> heights(
				TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent
					* TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent,
				50);
			TerraEngine::Renderer::Terrain::TerrainCookInput input{};
			input.assetId = asset; input.authoredHash = {71u, 72u}; input.authoredGeneration = 8u; input.heightMillimetres = heights;
			const auto cook = TerraEngine::Renderer::Terrain::CookTerrainPage(input);
			TerraEngine::Renderer::Terrain::TerrainViewPacket view{};
			view.assetId = asset; view.authoredHash = input.authoredHash; view.authoredGeneration = 8u; view.pageSourceIdentity = cook.sourceIdentity; view.frameId = 1u;
			const auto fresh = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, cook, {});
			TerraEngine::Resource::VirtualGeometryPagePublication lkg{};
			lkg.candidateId = fresh->candidate.candidateId; lkg.publicationGeneration = fresh->candidate.publicationGeneration; lkg.key = fresh->candidate.key;
			lkg.payload = fresh->candidate.payload;
			lkg.hierarchy = fresh->candidate.hierarchy;
			// Worker preparation deliberately leaves uploadCompleted false. Model the
			// protected Vulkan fence boundary before presenting this page as LKG.
			lkg.descriptor = {fresh->candidate.candidateId, 0u, 0u, true};
			lkg.state = TerraEngine::Resource::VirtualGeometryPagePublicationState::Resident; lkg.publicationFrame = 1u;
			const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication> visible = {lkg};
			const auto fallback = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, {}, visible);
			view.authoredGeneration = 9u;
			const auto rejected = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, {}, visible);
			context.Expect(fallback.has_value() && fallback->usesLastKnownGood && !rejected.has_value(),
				"Last-known-good residency must cover only the exact authored generation, never stale IO output.");
		});

		registry.Add("Terrain.Residency.VisibleTouchSnapshotIncludesEveryEnabledSurfaceSource", [](TestContext& context) {
			using namespace TerraEngine::Resource;
			TerrainVisibleMaterialArea visible{};
			visible.assetId = TerraEngine::GenerateUUID();
			visible.area = {-7, 13};
			visible.materialId = TerraEngine::GenerateUUID();
			visible.nearestViewDistance = 42.5;
			visible.weightTextureId = TerraEngine::GenerateUUID();
			visible.layerIdTextureId = TerraEngine::GenerateUUID();
			visible.layerCount = 2u;
			visible.layers[0].stableLayerId = 11u;
			visible.layers[1].stableLayerId = 22u;
			visible.layers[0].baseColorTextureId = TerraEngine::GenerateUUID();
			visible.layers[0].normalTextureId = TerraEngine::GenerateUUID();
			visible.layers[0].ormTextureId = TerraEngine::GenerateUUID();
			visible.layers[0].emissiveTextureId = TerraEngine::GenerateUUID();
			visible.layers[1].baseColorTextureId = TerraEngine::GenerateUUID();
			visible.layers[1].normalTextureId = TerraEngine::GenerateUUID();
			visible.layers[1].ormTextureId = TerraEngine::GenerateUUID();
			visible.layers[1].emissiveTextureId = TerraEngine::GenerateUUID();
			const std::array visibleAreas{visible};
			const auto snapshot = PrepareTerrainVisibleTextureTouchSnapshot(
				91u, visibleAreas);

			const auto contains = [&](TerraEngine::UUID textureId,
				TerrainVisibleTextureSemantic semantic) {
				return std::find_if(snapshot.touches.begin(), snapshot.touches.end(),
					[&](const TerrainVisibleTextureTouch& touch) {
						return touch.textureId == textureId && touch.semantic == semantic
							&& touch.nearestViewDistance == visible.nearestViewDistance
							&& touch.assetId == visible.assetId && touch.area == visible.area;
					}) != snapshot.touches.end();
			};
			bool allSourcesPresent = contains(visible.weightTextureId,
				TerrainVisibleTextureSemantic::ControlWeight)
				&& contains(visible.layerIdTextureId, TerrainVisibleTextureSemantic::ControlId);
			for(std::uint32_t layerIndex = 0u; layerIndex < visible.layerCount; ++layerIndex)
			{
				const auto& layer = visible.layers[layerIndex];
				allSourcesPresent = allSourcesPresent
					&& contains(layer.baseColorTextureId, TerrainVisibleTextureSemantic::BaseColor)
					&& contains(layer.normalTextureId, TerrainVisibleTextureSemantic::Normal)
					&& contains(layer.ormTextureId, TerrainVisibleTextureSemantic::ORM)
					&& contains(layer.emissiveTextureId, TerrainVisibleTextureSemantic::Emissive);
			}
			context.Expect(snapshot.complete && snapshot.frameId == 91u
				&& snapshot.touches.size() == 10u && allSourcesPresent,
				"One immutable visible-area capture must touch both control textures and all four source semantics for every enabled layer.");
		});

		registry.Add("Terrain.Residency.VisibleTouchSnapshotDeduplicatesAtNearestArea", [](TestContext& context) {
			using namespace TerraEngine::Resource;
			TerrainVisibleMaterialArea far{};
			far.assetId = TerraEngine::GenerateUUID();
			far.area = {4, 5};
			far.materialId = TerraEngine::GenerateUUID();
			far.nearestViewDistance = 500.0;
			far.weightTextureId = TerraEngine::GenerateUUID();
			far.layerIdTextureId = TerraEngine::GenerateUUID();
			far.layerCount = 2u;
			const TerraEngine::UUID sharedBaseColor = TerraEngine::GenerateUUID();
			for(std::uint32_t layerIndex = 0u; layerIndex < far.layerCount; ++layerIndex)
			{
				auto& layer = far.layers[layerIndex];
				layer.stableLayerId = static_cast<std::uint16_t>(layerIndex + 1u);
				layer.baseColorTextureId = sharedBaseColor;
				layer.normalTextureId = TerraEngine::GenerateUUID();
				layer.ormTextureId = TerraEngine::GenerateUUID();
				layer.emissiveTextureId = TerraEngine::GenerateUUID();
			}
			TerrainVisibleMaterialArea near = far;
			near.area = {5, 5}; near.nearestViewDistance = 12.0;
			const std::array visibleAreas{far, near};
			auto snapshot = PrepareTerrainVisibleTextureTouchSnapshot(
				104u, visibleAreas);

			const auto shared = std::find_if(snapshot.touches.begin(), snapshot.touches.end(),
				[&](const TerrainVisibleTextureTouch& touch) {
					return touch.textureId == sharedBaseColor;
				});
			// Mutating the detached inputs after preparation must not alter the
			// returned frame-owned values or cause a registry read on the consumer.
			near.nearestViewDistance = 1.0;
			far.layers[0].baseColorTextureId = TerraEngine::GenerateUUID();
			context.Expect(snapshot.complete && snapshot.touches.size() == 9u
				&& snapshot.duplicateEliminationCount == 11u
				&& shared != snapshot.touches.end()
				&& shared->semantic == TerrainVisibleTextureSemantic::BaseColor
				&& shared->nearestViewDistance == 12.0 && shared->assetId == far.assetId
				&& shared->area == TerraEngine::Terrain::TerrainAreaCoordinate{5, 5},
				"Visible terrain texture demand must deduplicate UUIDs, retain the nearest distance and diagnostics, and publish an immutable value snapshot.");
		});

		registry.Add("Terrain.Residency.SamplerPolicyConvertsEveryRequestedFilterExactly", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			TerraEngine::Terrain::TerrainSurfaceSamplerRequest request{};
			request.addressU = SamplerAddressMode::MirroredRepeat;
			request.addressV = SamplerAddressMode::ClampToEdge;
			request.addressW = SamplerAddressMode::ClampToBorder;
			const VulkanSamplerCapabilities capable{true, 16.0F};

			request.filter = SamplerFilter::Nearest;
			const auto nearest = NormalizeTerrainSamplerPolicy(request, capable, 8.0F);
			request.filter = SamplerFilter::Bilinear;
			const auto bilinear = NormalizeTerrainSamplerPolicy(request, capable, 8.0F);
			request.filter = SamplerFilter::Trilinear;
			const auto trilinear = NormalizeTerrainSamplerPolicy(request, capable, 8.0F);
			request.filter = SamplerFilter::Anisotropic;
			const auto anisotropic = NormalizeTerrainSamplerPolicy(request, capable, 8.0F);

			context.Expect(nearest.requestedFilter == SamplerFilter::Nearest
				&& nearest.effectiveFilter == SamplerFilter::Nearest
				&& nearest.key.minFilter == VulkanSamplerFilter::Nearest
				&& nearest.key.magFilter == VulkanSamplerFilter::Nearest
				&& nearest.key.mipFilter == VulkanSamplerMipFilter::Nearest
				&& !nearest.key.anisotropyEnabled && nearest.key.effectiveAnisotropy == 1.0F,
				"Nearest filtering must use nearest minification, magnification, and mip selection with anisotropy disabled.");
			context.Expect(bilinear.requestedFilter == SamplerFilter::Bilinear
				&& bilinear.effectiveFilter == SamplerFilter::Bilinear
				&& bilinear.key.minFilter == VulkanSamplerFilter::Linear
				&& bilinear.key.magFilter == VulkanSamplerFilter::Linear
				&& bilinear.key.mipFilter == VulkanSamplerMipFilter::Nearest
				&& !bilinear.key.anisotropyEnabled,
				"Bilinear filtering must use linear minification and magnification with nearest mip selection.");
			context.Expect(trilinear.requestedFilter == SamplerFilter::Trilinear
				&& trilinear.effectiveFilter == SamplerFilter::Trilinear
				&& trilinear.key.minFilter == VulkanSamplerFilter::Linear
				&& trilinear.key.magFilter == VulkanSamplerFilter::Linear
				&& trilinear.key.mipFilter == VulkanSamplerMipFilter::Linear
				&& !trilinear.key.anisotropyEnabled,
				"Trilinear filtering must linearly filter both texels and adjacent mip levels without anisotropy.");
			context.Expect(anisotropic.requestedFilter == SamplerFilter::Anisotropic
				&& anisotropic.effectiveFilter == SamplerFilter::Anisotropic
				&& anisotropic.key.minFilter == VulkanSamplerFilter::Linear
				&& anisotropic.key.magFilter == VulkanSamplerFilter::Linear
				&& anisotropic.key.mipFilter == VulkanSamplerMipFilter::Linear
				&& anisotropic.key.anisotropyEnabled && anisotropic.key.effectiveAnisotropy == 8.0F,
				"Supported anisotropic filtering must preserve the requested level on a capable device.");
			context.Expect(anisotropic.key.addressU == request.addressU
				&& anisotropic.key.addressV == request.addressV
				&& anisotropic.key.addressW == request.addressW,
				"Sampler normalization must preserve each authored address axis exactly.");
		});

		registry.Add("Terrain.Residency.SamplerPolicyClampsCapabilityAndFallsBackSafely", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			TerraEngine::Terrain::TerrainSurfaceSamplerRequest request{};
			request.filter = SamplerFilter::Anisotropic;

			const auto clamped = NormalizeTerrainSamplerPolicy(
				request, VulkanSamplerCapabilities{true, 4.0F}, 16.0F);
			const auto unsupported = NormalizeTerrainSamplerPolicy(
				request, VulkanSamplerCapabilities{false, 16.0F}, 16.0F);
			const auto invalidCapability = NormalizeTerrainSamplerPolicy(
				request, VulkanSamplerCapabilities{true, std::numeric_limits<float>::quiet_NaN()}, 16.0F);

			context.Expect(clamped.key.anisotropyEnabled
				&& clamped.key.effectiveAnisotropy == 4.0F
				&& clamped.effectiveFilter == SamplerFilter::Anisotropic
				&& clamped.fallback == VulkanSamplerFallback::RequestedLevelClamped,
				"Anisotropy must clamp to the finite physical-device limit and expose that quality reduction.");
			context.Expect(!unsupported.key.anisotropyEnabled
				&& unsupported.key.effectiveAnisotropy == 1.0F
				&& unsupported.key.mipFilter == VulkanSamplerMipFilter::Linear
				&& unsupported.effectiveFilter == SamplerFilter::Trilinear
				&& unsupported.fallback == VulkanSamplerFallback::AnisotropyUnsupported,
				"A device without sampler anisotropy must use a diagnostic trilinear fallback instead of rejecting initialization.");
			context.Expect(!invalidCapability.key.anisotropyEnabled
				&& invalidCapability.key.effectiveAnisotropy == 1.0F
				&& invalidCapability.effectiveFilter == SamplerFilter::Trilinear
				&& invalidCapability.fallback == VulkanSamplerFallback::InvalidDeviceLimit,
				"A non-finite device anisotropy limit must normalize to a safe finite trilinear policy.");
		});

		registry.Add("Terrain.Residency.SampledAliasesReuseSamplersAndDistinguishSamplerKeys", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			SampledTextureAliasCache cache{};
			TerraEngine::Terrain::TerrainSurfaceSamplerRequest request{};
			request.filter = SamplerFilter::Trilinear;
			const auto trilinearKey = NormalizeTerrainSamplerPolicy(
				request, VulkanSamplerCapabilities{true, 16.0F}, 8.0F).key;
			request.filter = SamplerFilter::Nearest;
			const auto nearestKey = NormalizeTerrainSamplerPolicy(
				request, VulkanSamplerCapabilities{true, 16.0F}, 8.0F).key;
			const TerraEngine::UUID textureA = TerraEngine::GenerateUUID();
			const TerraEngine::UUID textureB = TerraEngine::GenerateUUID();

			const auto first = cache.Acquire(textureA, trilinearKey, 7u);
			const auto repeated = cache.Acquire(textureA, trilinearKey, 7u);
			const auto sameSampler = cache.Acquire(textureB, trilinearKey, 3u);
			const auto distinctSampler = cache.Acquire(textureA, nearestKey, 7u);

			context.Expect(first.accepted && repeated.accepted && repeated.aliasReused
				&& first.descriptorIndex == repeated.descriptorIndex,
				"The same texture UUID and normalized sampler key must return one stable combined descriptor alias.");
			context.Expect(sameSampler.accepted && sameSampler.samplerReused
				&& sameSampler.descriptorIndex != first.descriptorIndex && cache.SamplerCount() == 2u,
				"Different textures with the same normalized key must share one sampler object while owning distinct descriptors.");
			context.Expect(distinctSampler.accepted && !distinctSampler.aliasReused
				&& distinctSampler.descriptorIndex != first.descriptorIndex
				&& cache.ActiveAliasCount() == 3u,
				"One image sampled with different normalized keys must own distinct stable combined descriptor aliases.");
		});

		registry.Add("Terrain.Residency.SampledAliasViewReplacementUpdatesEveryTextureAlias", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			SampledTextureAliasCache cache{};
			TerraEngine::Terrain::TerrainSurfaceSamplerRequest request{};
			request.filter = SamplerFilter::Trilinear;
			const auto firstKey = NormalizeTerrainSamplerPolicy(
				request, VulkanSamplerCapabilities{true, 16.0F}, 4.0F).key;
			request.filter = SamplerFilter::Nearest;
			const auto secondKey = NormalizeTerrainSamplerPolicy(
				request, VulkanSamplerCapabilities{true, 16.0F}, 4.0F).key;
			const TerraEngine::UUID replacedTexture = TerraEngine::GenerateUUID();
			const TerraEngine::UUID untouchedTexture = TerraEngine::GenerateUUID();
			const auto first = cache.Acquire(replacedTexture, firstKey, 4u);
			const auto second = cache.Acquire(replacedTexture, secondKey, 4u);
			const auto untouchedAcquisition = cache.Acquire(untouchedTexture, firstKey, 9u);

			const std::size_t updated = cache.ReplaceTextureView(replacedTexture, 12u);
			const auto updatedFirst = cache.Find(replacedTexture, firstKey);
			const auto updatedSecond = cache.Find(replacedTexture, secondKey);
			const auto untouched = cache.Find(untouchedTexture, firstKey);
			context.Expect(updated == 2u && updatedFirst.has_value() && updatedSecond.has_value()
				&& updatedFirst->descriptorIndex == first.descriptorIndex
				&& updatedSecond->descriptorIndex == second.descriptorIndex
				&& updatedFirst->imageViewGeneration == 12u
				&& updatedSecond->imageViewGeneration == 12u,
				"Replacing an image or mip view must update every alias for its texture UUID while preserving stable descriptor indices.");
			context.Expect(untouchedAcquisition.accepted && untouched.has_value()
				&& untouched->imageViewGeneration == 9u,
				"Texture view replacement must not mutate aliases owned by another source UUID.");
		});

		registry.Add("Terrain.Residency.SampledAliasRetirementWaitsForCompletedFrames", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			SampledTextureAliasCache cache{2u};
			TerraEngine::Terrain::TerrainSurfaceSamplerRequest request{};
			const auto key = NormalizeTerrainSamplerPolicy(
				request, VulkanSamplerCapabilities{true, 16.0F}, 4.0F).key;
			const TerraEngine::UUID retiredTexture = TerraEngine::GenerateUUID();
			const auto retired = cache.Acquire(retiredTexture, key, 1u);
			const auto occupied = cache.Acquire(TerraEngine::GenerateUUID(), key, 1u);
			const bool releaseQueued = cache.Release(retiredTexture, key, 10u, 2u);
			const auto premature = cache.Acquire(TerraEngine::GenerateUUID(), key, 1u);

			context.Expect(retired.accepted && occupied.accepted && releaseQueued
				&& cache.ActiveAliasCount() == 1u && cache.PendingRetirementCount() == 1u
				&& !premature.accepted && premature.capacityExhausted,
				"A released descriptor must remain unavailable while an in-flight frame can still reference it.");
			context.Expect(cache.RetireCompleted(11u) == 0u
				&& cache.PendingRetirementCount() == 1u,
				"Completing a frame before the safe lag expires must not recycle the pending descriptor.");
			context.Expect(cache.RetireCompleted(12u) == 1u
				&& cache.PendingRetirementCount() == 0u,
				"The protected retirement boundary may recycle an alias only after its final referencing frame completes.");
			const auto replacement = cache.Acquire(TerraEngine::GenerateUUID(), key, 2u);
			context.Expect(replacement.accepted && replacement.descriptorIndex == retired.descriptorIndex,
				"A safely retired descriptor index should become reusable without growing descriptor capacity.");
		});

		registry.Add("Terrain.Residency.SampledAliasCapacityRejectsSlot1025Atomically", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			SampledTextureAliasCache cache{};
			TerraEngine::Terrain::TerrainSurfaceSamplerRequest request{};
			const auto key = NormalizeTerrainSamplerPolicy(
				request, VulkanSamplerCapabilities{true, 16.0F}, 4.0F).key;
			std::unordered_set<std::uint32_t> descriptorIndices;
			TerraEngine::UUID firstTexture{};
			SampledTextureAliasAcquireResult first{};
			bool allAccepted = true;
			for(std::size_t index = 0u; index < SampledTextureAliasCache::kDefaultCapacity; ++index)
			{
				TerraEngine::UUID textureId{};
				textureId[0] = static_cast<std::uint8_t>(index & 0xffu);
				textureId[1] = static_cast<std::uint8_t>((index >> 8u) & 0xffu);
				textureId[15] = 1u;
				const auto alias = cache.Acquire(textureId, key, 1u);
				if(index == 0u)
				{
					firstTexture = textureId;
					first = alias;
				}
				allAccepted = allAccepted && alias.accepted;
				descriptorIndices.insert(alias.descriptorIndex);
			}

			TerraEngine::UUID overflowTexture{};
			overflowTexture[0] = 0xffu;
			overflowTexture[1] = 0xffu;
			overflowTexture[15] = 2u;
			const auto overflow = cache.Acquire(overflowTexture, key, 1u);
			const auto repeatedAtCapacity = cache.Acquire(firstTexture, key, 1u);
			context.Expect(allAccepted
				&& descriptorIndices.size() == SampledTextureAliasCache::kDefaultCapacity
				&& cache.ActiveAliasCount() == SampledTextureAliasCache::kDefaultCapacity,
				"Exactly 1024 unique combined aliases must receive unique stable descriptor slots.");
			context.Expect(!overflow.accepted && overflow.capacityExhausted
				&& cache.ActiveAliasCount() == SampledTextureAliasCache::kDefaultCapacity,
				"The 1025th unique alias must fail without evicting or partially replacing a live descriptor.");
			context.Expect(repeatedAtCapacity.accepted && repeatedAtCapacity.aliasReused
				&& repeatedAtCapacity.descriptorIndex == first.descriptorIndex,
				"A cache hit must remain available when unique descriptor capacity is exhausted.");
		});

		registry.Add("Terrain.Residency.DiagnosticsExposeValidationSamplerAliasAndCapacityFacts", [](TestContext& context) {
			using namespace TerraEngine::Resource;
			using namespace TerraEngine::Terrain;
			TerrainVisibleMaterialArea far{};
			far.assetId = TerraEngine::GenerateUUID();
			far.area = {1, 2};
			far.materialId = TerraEngine::GenerateUUID();
			far.nearestViewDistance = 90.0;
			far.weightTextureId = TerraEngine::GenerateUUID();
			far.layerIdTextureId = TerraEngine::GenerateUUID();
			far.layerCount = 1u;
			far.layers[0].stableLayerId = 19u;
			far.layers[0].baseColorTextureId = TerraEngine::GenerateUUID();
			far.layers[0].normalTextureId = TerraEngine::GenerateUUID();
			far.layers[0].ormTextureId = TerraEngine::GenerateUUID();
			far.layers[0].emissiveTextureId = TerraEngine::GenerateUUID();
			TerrainVisibleMaterialArea near = far;
			near.area = {2, 2};
			near.nearestViewDistance = 20.0;
			const std::array visibleAreas{far, near};
			const auto touches = PrepareTerrainVisibleTextureTouchSnapshot(55u, visibleAreas);
			const auto streaming = BuildTerrainTextureStreamingDiagnosticSnapshot(
				touches, TerrainDescriptorAliasDiagnosticRecord{
					3u, 7u, 1u, 1024u, 8u, true});

			TerrainDiagnosticBuildInput input{};
			input.snapshot.layerValidations.push_back(
				MakeTerrainLayerValidationDiagnosticRecord(
					TerrainResultCode::UnsupportedVersion,
					TerrainRootDiagnosticCode::UnsupportedTerrainRoot,
					12u, 4u, {}));
			TerrainSamplerQualityDiagnosticRecord quality{};
			quality.stableLayerId = 19u;
			quality.requestedQuality = SamplerFilter::Anisotropic;
			quality.effectiveQuality = SamplerFilter::Trilinear;
			quality.requestedAnisotropy = 16.0F;
			quality.effectiveAnisotropy = 1.0F;
			quality.fallbackReason = TerrainSamplerFallbackReason::AnisotropyUnsupported;
			input.snapshot.samplerQuality.push_back(quality);
			input.snapshot.textureStreaming = streaming;
			const auto diagnostics = BuildTerrainDiagnosticSnapshot(std::move(input));

			const bool hasLegacyRejection = std::any_of(
				diagnostics.items.begin(), diagnostics.items.end(),
				[](const TerrainDiagnosticItem& item) {
					return item.category == TerrainDiagnosticCategory::LayerValidation
						&& item.severity == TerrainDiagnosticSeverity::Error
						&& item.impact == TerrainDiagnosticImpact::Unavailable;
				});
			const bool hasSamplerFallback = std::any_of(
				diagnostics.items.begin(), diagnostics.items.end(),
				[](const TerrainDiagnosticItem& item) {
					return item.category == TerrainDiagnosticCategory::Sampler
						&& item.severity == TerrainDiagnosticSeverity::Warning;
				});
			const bool hasCapacityFailure = std::any_of(
				diagnostics.items.begin(), diagnostics.items.end(),
				[](const TerrainDiagnosticItem& item) {
					return item.category == TerrainDiagnosticCategory::Residency
						&& item.severity == TerrainDiagnosticSeverity::Error
						&& item.action == TerrainDiagnosticAction::IncreaseBudget;
				});
			context.Expect(diagnostics.layerValidations.size() == 1u
				&& diagnostics.layerValidations[0].legacyRootRejected
				&& diagnostics.layerValidations[0].definitionCount == 12u
				&& diagnostics.layerValidations[0].enabledCount == 4u
				&& hasLegacyRejection,
				"Immutable diagnostics must retain layer validation counts and identify rejected legacy roots with actionable severity.");
			context.Expect(diagnostics.samplerQuality.size() == 1u
				&& diagnostics.samplerQuality[0].requestedQuality == SamplerFilter::Anisotropic
				&& diagnostics.samplerQuality[0].effectiveQuality == SamplerFilter::Trilinear
				&& diagnostics.samplerQuality[0].requestedAnisotropy == 16.0F
				&& diagnostics.samplerQuality[0].effectiveAnisotropy == 1.0F
				&& ToString(diagnostics.samplerQuality[0].fallbackReason)
					== std::string_view("AnisotropyUnsupported")
				&& hasSamplerFallback,
				"Sampler diagnostics must expose requested and effective quality plus a stable capability fallback reason.");
			context.Expect(diagnostics.textureStreaming.has_value()
				&& diagnostics.textureStreaming->sourceTouchCount == 12u
				&& diagnostics.textureStreaming->uniqueTouchCount == 6u
				&& diagnostics.textureStreaming->duplicateEliminationCount == 6u
				&& diagnostics.textureStreaming->descriptors.samplerCount == 3u
				&& diagnostics.textureStreaming->descriptors.activeAliasCount == 7u
				&& diagnostics.textureStreaming->descriptors.pendingAliasRetirementCount == 1u
				&& diagnostics.textureStreaming->descriptors.descriptorCapacity == 1024u
				&& diagnostics.textureStreaming->descriptorSlotsAvailable == 1016u
				&& diagnostics.textureStreaming->descriptorCapacityExhausted
				&& hasCapacityFailure,
				"Resource diagnostics must preserve touch deduplication, sampler/alias counts, pending retirement, and descriptor-capacity failure facts.");
		});

		registry.Add("Terrain.Residency.MissingFinePageUsesOnlyItsPinnedOwnAncestor", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Terrain;
			using TerraEngine::Resource::VirtualGeometryPagePublicationState;
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			const TerraEngine::Terrain::TerrainAreaCoordinate area{-4, 9};
			std::vector<TerrainPageResidencyEntry> residency;
			TerrainPageResidencyEntry desired{};
			desired.assetId = assetId;
			desired.area = area;
			desired.pageId = 12u;
			desired.parentPageId = 6u;
			desired.state = VirtualGeometryPagePublicationState::Missing;
			residency.push_back(desired);
			TerrainPageResidencyEntry parent = desired;
			parent.pageId = 6u;
			parent.parentPageId = 0u;
			parent.state = VirtualGeometryPagePublicationState::PendingUpload;
			parent.uploadCompleted = false;
			parent.published = false;
			residency.push_back(parent);
			TerrainPageResidencyEntry root = desired;
			root.pageId = 0u;
			root.parentPageId = kInvalidTerrainPageId;
			root.state = VirtualGeometryPagePublicationState::Resident;
			root.uploadCompleted = true;
			root.published = true;
			root.pinnedCoarseCoverage = true;
			residency.push_back(root);
			TerrainPageResidencyEntry unrelated = root;
			unrelated.pageId = 99u;
			unrelated.pinnedCoarseCoverage = false;
			residency.push_back(unrelated);

			TerrainPageSelectionRequest request{};
			request.frameId = 40u;
			request.viewId = 3u;
			request.assetId = assetId;
			request.area = area;
			request.desiredPageId = desired.pageId;
			request.priority = 90;
			const auto resolution = ResolveTerrainOwnAncestorPage(request, residency);
			context.Expect(resolution.completeCoverage
				&& !resolution.desiredResident
				&& resolution.desiredPageId == 12u
				&& resolution.actualPageId == 0u
				&& resolution.fallbackDepth == 2u
				&& resolution.pinnedCoarseCoverage,
				"Missing detail must walk only the selected node's own parent chain to pinned coarse coverage, never borrow an unrelated resident page.");
		});

		registry.Add("Terrain.Residency.UploadMustPublishBeforeRecoveryAndEvictionWaitsForFrameLifetime", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Terrain;
			using TerraEngine::Resource::VirtualGeometryPagePublicationState;
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			const TerraEngine::Terrain::TerrainAreaCoordinate area{2, -3};
			TerrainPageResidencyEntry root{};
			root.assetId = assetId;
			root.area = area;
			root.pageId = 0u;
			root.parentPageId = kInvalidTerrainPageId;
			root.state = VirtualGeometryPagePublicationState::Resident;
			root.uploadCompleted = true;
			root.published = true;
			root.pinnedCoarseCoverage = true;
			TerrainPageResidencyEntry fine = root;
			fine.pageId = 4u;
			fine.parentPageId = 0u;
			fine.state = VirtualGeometryPagePublicationState::PendingUpload;
			fine.uploadCompleted = true;
			fine.published = false;
			fine.pinnedCoarseCoverage = false;
			std::vector<TerrainPageResidencyEntry> residency{root, fine};
			TerrainPageSelectionRequest request{};
			request.frameId = 100u;
			request.viewId = 1u;
			request.assetId = assetId;
			request.area = area;
			request.desiredPageId = fine.pageId;
			request.priority = 80;

			const auto beforePublication = ResolveTerrainOwnAncestorPage(request, residency);
			context.Expect(beforePublication.completeCoverage
				&& beforePublication.actualPageId == root.pageId
				&& !beforePublication.desiredResident,
				"An upload-complete page must remain invisible until the protected publication boundary accepts it.");

			residency[1].state = VirtualGeometryPagePublicationState::Resident;
			residency[1].published = true;
			const auto recovered = ResolveTerrainOwnAncestorPage(request, residency);
			context.Expect(recovered.completeCoverage && recovered.desiredResident
				&& recovered.actualPageId == fine.pageId && recovered.fallbackDepth == 0u,
				"The desired page must recover on the first captured generation after upload-complete protected publication.");

			residency[1].state = VirtualGeometryPagePublicationState::RetirePending;
			residency[1].retireAfterFrame = 105u;
			request.frameId = 104u;
			const auto capturedBeforeRetirement = ResolveTerrainOwnAncestorPage(request, residency);
			request.frameId = 105u;
			const auto afterRetirement = ResolveTerrainOwnAncestorPage(request, residency);
			context.Expect(capturedBeforeRetirement.actualPageId == fine.pageId
				&& capturedBeforeRetirement.desiredResident
				&& afterRetirement.actualPageId == root.pageId
				&& !afterRetirement.desiredResident,
				"Eviction must keep a page alive through its declared in-flight frame lifetime, then fall back to pinned coverage after retirement completes.");
		});

		registry.Add("Terrain.Residency.PageDemandAndResidentPagesShareAcrossViews", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Terrain;
			using TerraEngine::Resource::VirtualGeometryPagePublicationState;
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			const TerraEngine::Terrain::TerrainAreaCoordinate area{7, 8};
			TerrainPageResidencyEntry root{};
			root.assetId = assetId;
			root.area = area;
			root.pageId = 0u;
			root.parentPageId = kInvalidTerrainPageId;
			root.state = VirtualGeometryPagePublicationState::Resident;
			root.uploadCompleted = true;
			root.published = true;
			root.pinnedCoarseCoverage = true;
			TerrainPageResidencyEntry desired = root;
			desired.pageId = 10u;
			desired.parentPageId = 0u;
			desired.state = VirtualGeometryPagePublicationState::Missing;
			desired.uploadCompleted = false;
			desired.published = false;
			desired.pinnedCoarseCoverage = false;
			std::vector<TerrainPageResidencyEntry> residency{root, desired};
			TerrainPageSelectionRequest scene{};
			scene.frameId = 60u;
			scene.viewId = 1u;
			scene.assetId = assetId;
			scene.area = area;
			scene.desiredPageId = desired.pageId;
			scene.priority = 70;
			TerrainPageSelectionRequest game = scene;
			game.viewId = 2u;
			game.priority = 90;
			const std::array requests{scene, game};
			const auto missing = BuildTerrainPageDemands(requests, residency);
			context.Expect(missing.completeCoverage
				&& missing.resolutions.size() == 2u
				&& missing.resolutions[0].actualPageId == root.pageId
				&& missing.resolutions[1].actualPageId == root.pageId
				&& missing.demands.size() == 1u
				&& missing.demands[0].pageId == desired.pageId
				&& missing.demands[0].requestingViewCount == 2u
				&& missing.demands[0].priority == game.priority,
				"Independent views requesting the same absent fine page must share one highest-priority demand while retaining complete ancestor coverage.");

			residency[1].state = VirtualGeometryPagePublicationState::Resident;
			residency[1].uploadCompleted = true;
			residency[1].published = true;
			const auto resident = BuildTerrainPageDemands(requests, residency);
			context.Expect(resident.completeCoverage && resident.demands.empty()
				&& resident.resolutions.size() == 2u
				&& resident.resolutions[0].actualPageId == desired.pageId
				&& resident.resolutions[1].actualPageId == desired.pageId,
				"Once published, the same immutable physical terrain page must satisfy every requesting view without duplicate uploads.");
		});
	}
}
