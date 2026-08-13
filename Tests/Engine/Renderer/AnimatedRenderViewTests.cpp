// AnimatedRenderViewTests.cpp - GPU animation all-view render contract tests.
//
// The deformation generation lives on the immutable renderable record, while
// each camera owns only visibility state. This separation lets every pass share
// one complete palette without a camera movement rebuilding animation work.

#include "TestHarness.h"

#include "Renderer/Backend/RenderBackendWork.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderSystem.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		UUID MakeAnimatedViewTestUuid(std::uint8_t seed)
		{
			UUID id{};
			id[15] = seed;
			return id;
		}

		TerraEngine::Renderer::RenderFrameSnapshot BuildAllViewSnapshot()
		{
			using namespace TerraEngine::Renderer;

			RenderFrameSnapshot snapshot;
			snapshot.frameId = 440u;
			snapshot.viewRequests = {
				{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1920u, 1080u), 1u, 10u },
				{ RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(1280u, 720u), 2u, 20u },
				{ RenderOutputKind::ValidationOnly, RenderViewType::Shadow, glm::uvec2(2048u, 2048u), 3u, 30u },
				{ RenderOutputKind::OffscreenTexture, RenderViewType::Game, glm::uvec2(640u, 360u), 4u, 40u }
			};
			snapshot.renderables.push_back({
				MakeAnimatedViewTestUuid(1u),
				MakeAnimatedViewTestUuid(2u),
				MakeAnimatedViewTestUuid(3u),
				glm::mat4(1.0f),
				{ 0u, 0u, 12u, true, false, {}, true },
				true,
				false
			});
			AnimationRenderGenerationRecord& generation = snapshot.renderables.back().animation;
			generation.stableInstanceId = 7u;
			generation.poseGeneration = 12u;
			generation.morphGeneration = 13u;
			generation.paletteGeneration = 14u;
			generation.deformablePayloadGeneration = 15u;
			generation.programGeneration = 16u;
			generation.rendererGeneration = 17u;
			generation.source = AnimationRenderEvaluationSource::CpuPose;
			generation.complete = true;
			snapshot.renderables.back().invalidation = {
				12u, 13u, 15u, 18u, 40u, 19u, 17u, 20u };
			return snapshot;
		}

		VirtualGeometryAssetRuntime BuildFailOpenBoundsAsset()
		{
			using namespace TerraEngine::Renderer;

			VirtualGeometryAssetRuntime asset;
			asset.assetId = MakeAnimatedViewTestUuid(10u);
			asset.clusterCoverageValidated = true;
			asset.cullableGroups.resize(4u);

			GeometryPageRuntime page;
			page.pageId = 0u;
			page.fallbackPageId = 0u;
			page.state = GeometryPageState::Pinned;
			page.fallbackPage = true;
			page.pinnedFallback = true;
			asset.pages.push_back(page);

			VirtualGeometryClusterRuntime cluster;
			cluster.clusterId = 0u;
			cluster.lodIndex = 0u;
			cluster.firstGroupIndex = 0u;
			cluster.groupCount = 4u;
			cluster.pageId = 0u;
			cluster.fallbackClusterId = 0u;
			cluster.boundsRadius = 1.0f;
			asset.clusters.push_back(cluster);
			return asset;
		}

		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> BuildGpuAnimationPlanFixture()
		{
			auto plan = std::make_shared<TerraEngine::Animation::GpuAnimationPlan>();
			plan->generation = 21U;
			plan->topologyInputSignature = 0x2100U;
			plan->topologyRendererGeneration = 17U;
			TerraEngine::Animation::GpuAnimationGenerationRecord generation{};
			generation.rendererGeneration = 17U;
			plan->generationRecords.push_back(generation);
			TerraEngine::Animation::GpuAnimationWorkPacket packet{};
			packet.stableInstanceId = 7U;
			packet.generationRecordIndex = 0U;
			packet.stageMask = TerraEngine::Animation::kGpuAnimationAllStageMask;
			plan->workPackets.push_back(packet);
			TerraEngine::Animation::GpuAnimationRenderIdentity renderIdentity{};
			renderIdentity.meshId = MakeAnimatedViewTestUuid(1U);
			plan->renderIdentities.push_back(renderIdentity);
			return plan;
		}

		void MakeSnapshotGpuDriven(TerraEngine::Renderer::RenderFrameSnapshot& snapshot)
		{
			using namespace TerraEngine::Renderer;
			AnimationRenderGenerationRecord& generation = snapshot.renderables.front().animation;
			generation.animationPlanGeneration = 21U;
			generation.firstGpuWorkPacket = 0U;
			generation.gpuWorkPacketCount = 1U;
			generation.gpuStageMask = TerraEngine::Animation::kGpuAnimationAllStageMask;
			generation.source = AnimationRenderEvaluationSource::GpuVisual;
		}

		TerraEngine::Renderer::RenderDynamicStateRoot BuildDynamicState(
			const TerraEngine::Renderer::RenderFrameSnapshot& snapshot)
		{
			using namespace TerraEngine::Renderer;
			RenderDynamicStateRoot root{};
			root.generation = snapshot.frameId;
			root.rendererSessionGeneration = 1u;
			auto page = std::make_shared<RenderDynamicPage>();
			page->generation = snapshot.frameId;
			page->lanes.reserve(snapshot.renderables.size());
			for(const RenderFrameRenderableRecord& renderable : snapshot.renderables)
			{
				RenderDynamicLane lane{};
				// Mirror the production publication boundary: tests retain only the
				// immutable values consumed by backend animation work.
				lane.animation.stableInstanceId = renderable.animation.stableInstanceId;
				lane.animation.poseGeneration = renderable.animation.poseGeneration;
				lane.animation.paletteGeneration = renderable.animation.paletteGeneration;
				lane.animation.deformablePayloadGeneration =
					renderable.animation.deformablePayloadGeneration;
				lane.animation.programGeneration = renderable.animation.programGeneration;
				lane.animation.animationPlanGeneration =
					renderable.animation.animationPlanGeneration;
				lane.animation.rendererGeneration = renderable.animation.rendererGeneration;
				lane.animation.firstGpuWorkPacket = renderable.animation.firstGpuWorkPacket;
				lane.animation.gpuWorkPacketCount = renderable.animation.gpuWorkPacketCount;
				lane.animation.gpuStageMask = renderable.animation.gpuStageMask;
				lane.animation.source = static_cast<std::uint8_t>(renderable.animation.source);
				lane.animation.complete = renderable.animation.complete;
				lane.occupied = true;
				page->lanes.push_back(std::move(lane));
			}
			root.pages.push_back(std::move(page));
			return root;
		}

		TerraEngine::Renderer::AnimationRenderBindingRequest BuildGpuVisualBindingRequest()
		{
			using namespace TerraEngine::Renderer;
			AnimationRenderBindingRequest request{};
			request.meshId = MakeAnimatedViewTestUuid(50U);
			request.animatorObjectId = MakeAnimatedViewTestUuid(51U);
			request.animatorObjectGeneration = 2U;
			request.animationAuthorityGeneration = 3U;
			request.deformablePayloadGeneration = 22U;
			request.componentGeneration = 7U;
			request.skeletonCompatibilityFingerprint = 8U;
			request.expectedGpuRenderIdentity.meshId = request.meshId;
			request.expectedGpuRenderIdentity.meshHeaderVersion = 9U;
			request.expectedGpuRenderIdentity.deformablePayloadGeneration = 22U;
			request.expectedGpuRenderIdentity.rendererGeneration = 16U;
			request.expectedGpuRenderIdentity.sceneGeneration = 10U;
			request.expectedGpuRenderIdentity.skeletonCompatibilityFingerprint = 8U;
			request.invalidation = { 0U, 0U, 22U, 22U, 10U, 11U, 12U, 22U };
			return request;
		}

		TerraEngine::Renderer::AnimationGpuRenderBinding BuildGpuVisualIdentity()
		{
			using namespace TerraEngine::Renderer;
			AnimationGpuRenderBinding identity{};
			identity.stableInstanceId = (static_cast<std::uint64_t>(4U) << 32U) | 5U;
			identity.presentationGeneration = 13U;
			identity.sourceGeneration = 14U;
			identity.paletteGeneration = 13U;
			identity.planGeneration = 15U;
			identity.rendererGeneration = 16U;
			identity.workPacketIndex = 0U;
			identity.stableSubmissionOrdinal = 17U;
			identity.instanceSlot = 4U;
			identity.instanceGeneration = 5U;
			identity.componentGeneration = 7U;
			identity.skeletonGeneration = 18U;
			identity.meshGeneration = 9U;
			identity.resourceGeneration = 22U;
			identity.sceneGeneration = 10U;
			identity.workGeneration = 19U;
			identity.programGeneration = 20U;
			identity.stageMask = TerraEngine::Animation::kGpuAnimationAllStageMask;
			identity.renderIdentity.meshId = MakeAnimatedViewTestUuid(50U);
			identity.renderIdentity.meshHeaderVersion = 9U;
			identity.renderIdentity.deformablePayloadGeneration = 22U;
			identity.renderIdentity.rendererGeneration = 16U;
			identity.renderIdentity.sceneGeneration = 10U;
			identity.renderIdentity.skeletonCompatibilityFingerprint = 8U;
			identity.valid = true;
			return identity;
		}
	}

	void RegisterAnimatedRenderViewTests(TestRegistry& registry)
	{
		registry.Add("Renderer.AnimationView.VisibilityPublicationIsImmutableAndDeduplicated", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			const UUID first = MakeAnimatedViewTestUuid(31u);
			const UUID second = MakeAnimatedViewTestUuid(32u);
			AnimatedObjectVisibilitySnapshot snapshot;
			snapshot.frameId = 91u;
			snapshot.sceneGeneration = 17u;
			snapshot.visibleObjectIds = {second, first, second};
			snapshot.hasActiveView = true;
			snapshot.valid = true;
			PublishAnimatedObjectVisibilitySnapshot(std::move(snapshot));

			const auto published = LoadAnimatedObjectVisibilitySnapshot();
			context.Expect(published && published->valid,
				"Animated visibility must publish one complete immutable snapshot.");
			if(published)
			{
				context.ExpectEqual(std::size_t{2u}, published->visibleObjectIds.size(),
					"Objects visible in multiple views must be deduplicated once.");
				context.Expect(!published->visibleObjectLookupIndices.empty(),
					"Published visibility must build the reusable exact-membership lookup.");
				context.Expect(published->Contains(first) && published->Contains(second),
					"Indexed visibility membership must retain every unique object.");
				context.Expect(!published->Contains(MakeAnimatedViewTestUuid(33u)),
					"Objects outside every camera and shadow list must remain absent.");
			}
			ClearAnimatedObjectVisibilitySnapshot();
			context.Expect(!LoadAnimatedObjectVisibilitySnapshot(),
				"Renderer shutdown must retire the prior scene's visibility snapshot.");
		});

		registry.Add("Renderer.AnimationView.CanonicalVisibilityPublicationSkipsRenormalization", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			const UUID first = MakeAnimatedViewTestUuid(41u);
			const UUID second = MakeAnimatedViewTestUuid(42u);
			AnimatedObjectVisibilitySnapshot snapshot;
			snapshot.frameId = 92u;
			snapshot.sceneGeneration = 18u;
			snapshot.visibleObjectIds = {first, second};
			snapshot.hasActiveView = true;
			snapshot.valid = true;
			snapshot.canonicalUnique = true;
			PublishAnimatedObjectVisibilitySnapshot(std::move(snapshot));

			const auto published = LoadAnimatedObjectVisibilitySnapshot();
			context.Expect(published && published->canonicalUnique,
				"Renderer-owned visibility masks must retain their canonical-order proof.");
			if(published)
			{
				context.ExpectEqual(std::size_t{2u}, published->visibleObjectIds.size(),
					"Canonical publication must preserve one record per marked renderable.");
				context.Expect(published->Contains(first) && published->Contains(second),
					"Canonical renderer publication must populate the same exact lookup.");
				context.Expect(published->visibleObjectIds[0] == first
					&& published->visibleObjectIds[1] == second,
					"Canonical publication must preserve the renderable table's UUID order.");
			}
			ClearAnimatedObjectVisibilitySnapshot();
		});

		registry.Add("Renderer.AnimationView.RouteSelectionRecordRemainsCompact", [](TestContext& context) {
			context.Expect(
				sizeof(TerraEngine::Renderer::GeometryRouteSelectionRecord)
					< sizeof(TerraEngine::Renderer::GeometryRouteDecision),
				"Per-instance snapshots must not retain mesh-level route candidate vectors and strings.");
		});

		registry.Add("Renderer.AnimationView.ImmutableAllViewGenerationContract", [](TestContext& context) {
			const TerraEngine::Renderer::RenderFrameSnapshot snapshot = BuildAllViewSnapshot();
			const TerraEngine::Renderer::AnimationRenderGenerationRecord& generation =
				snapshot.renderables.front().animation;
			const auto states = TerraEngine::Renderer::BuildVirtualGeometryViewSelectionStates(snapshot);

			context.ExpectEqual(static_cast<std::size_t>(4u), states.size(), "Every submitted view must receive a copied visibility state.");
			context.Expect(TerraEngine::Renderer::ValidateAnimationRenderGenerationRecord(generation)
				&& generation.complete && generation.poseGeneration != 0u
				&& generation.morphGeneration != 0u && generation.paletteGeneration != 0u
				&& generation.deformablePayloadGeneration != 0u,
				"The snapshot must attach one complete immutable animation generation bundle to every submitted view.");
		});

		registry.Add("Renderer.AnimationView.CameraChangesDoNotMutateDeformationGeneration", [](TestContext& context) {
			const auto snapshot = BuildAllViewSnapshot();
			const std::uint64_t deformationHash = TerraEngine::Renderer::BuildAnimationRenderGenerationHash(
				snapshot.renderables.front().animation);
			auto states = TerraEngine::Renderer::BuildVirtualGeometryViewSelectionStates(snapshot);
			const auto scene = std::find_if(states.begin(), states.end(), [](const auto& state) {
				return state.outputId == 2u;
			});
			const auto capture = std::find_if(states.begin(), states.end(), [](const auto& state) {
				return state.outputId == 4u;
			});

			context.Expect(scene != states.end() && capture != states.end(), "Scene and capture views must both have independent snapshot records.");
			if (scene != states.end() && capture != states.end())
			{
				const std::uint32_t captureMeshlets = capture->draws.front().range.meshletCount;
				scene->draws.front().range.meshletCount = 1u;
				context.ExpectEqual(captureMeshlets, capture->draws.front().range.meshletCount,
					"Changing one copied camera visibility result must not mutate another view's deformation input.");
			}
			context.ExpectEqual(deformationHash,
				TerraEngine::Renderer::BuildAnimationRenderGenerationHash(snapshot.renderables.front().animation),
				"Camera-local visibility changes must leave the shared deformation generation unchanged.");
		});

		registry.Add("Renderer.AnimationView.GpuWorkIsDeduplicatedAcrossMeshesAndViews", [](TestContext& context) {
			auto snapshot = BuildAllViewSnapshot();
			MakeSnapshotGpuDriven(snapshot);
			auto second = snapshot.renderables.front();
			second.objectId = MakeAnimatedViewTestUuid(24U);
			snapshot.renderables.push_back(second);
			const TerraEngine::Renderer::AnimationBackendWorkPlan work =
				TerraEngine::Renderer::BuildAnimationBackendWorkPlan(
					snapshot.frameId, BuildDynamicState(snapshot),
					BuildGpuAnimationPlanFixture());

			context.Expect(work.valid && work.ranges.size() == 1U
				&& work.gpuAnimatorCount == 1U && work.immutablePlan != nullptr,
				"One animator shared by multiple meshes and views must submit one retained GPU packet range.");
		});

		registry.Add("Renderer.AnimationView.MixedGpuGenerationsRejectAtomically", [](TestContext& context) {
			auto snapshot = BuildAllViewSnapshot();
			MakeSnapshotGpuDriven(snapshot);
			auto conflicting = snapshot.renderables.front();
			conflicting.objectId = MakeAnimatedViewTestUuid(25U);
			++conflicting.animation.paletteGeneration;
			snapshot.renderables.push_back(conflicting);
			const TerraEngine::Renderer::AnimationBackendWorkPlan work =
				TerraEngine::Renderer::BuildAnimationBackendWorkPlan(
					snapshot.frameId, BuildDynamicState(snapshot),
					BuildGpuAnimationPlanFixture());

			context.Expect(!work.valid && work.ranges.empty() && work.immutablePlan == nullptr
				&& !work.rejectionReason.empty(),
				"Conflicting references to one animator must discard the complete GPU submission envelope.");
		});

		registry.Add("Renderer.AnimationView.GpuVisualBindingDoesNotRequireCpuPose", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			auto request = BuildGpuVisualBindingRequest();
			request.gpuVisualIdentity = BuildGpuVisualIdentity();
			const auto binding = RenderSystem::ResolveAnimationRenderBinding(request);

			context.Expect(binding.status == AnimationRenderBindingStatus::Ready
				&& binding.source == AnimationRenderPoseSource::GpuVisual
				&& !binding.safePose && !binding.HasCpuPose()
				&& binding.gpu.IsComplete() && binding.gpu.stableSubmissionOrdinal == 17U,
				"A fully identified Visual GPU packet must bind without manufacturing a CPU pose.");
		});

		registry.Add("Renderer.AnimationView.PageCapabilitySurvivesPoseBindingResolution", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			const std::filesystem::path renderSystemPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream renderSystemStream(renderSystemPath, std::ios::binary);
			std::string renderSystemSource{
				std::istreambuf_iterator<char>(renderSystemStream), {} };
			// Source contracts describe tokens and control flow, not checkout line
			// endings. Normalize CRLF so Windows worktrees match the reviewed LF text.
			renderSystemSource.erase(std::remove(renderSystemSource.begin(),
				renderSystemSource.end(), '\r'), renderSystemSource.end());
			auto request = BuildGpuVisualBindingRequest();
			request.gpuVisualIdentity = BuildGpuVisualIdentity();
			const AnimationRenderBinding legacy =
				RenderSystem::ResolveAnimationRenderBinding(request);
			request.pageLocalDeformationAvailable = true;
			const AnimationRenderBinding pageLocal =
				RenderSystem::ResolveAnimationRenderBinding(request);

			AnimationRenderBinding bindPose = legacy;
			bindPose.source = AnimationRenderPoseSource::BindPose;
			AnimationRenderBinding safePose = legacy;
			safePose.safePose = true;
			safePose.status = AnimationRenderBindingStatus::SafePose;
			AnimationRenderBinding staticBinding{};

			context.Expect(
				legacy.RequiresOwnerMeshDeformationFallback()
					&& legacy.RequiresVirtualGeometryOwnerFallbackVisibility(true)
					&& !legacy.RequiresVirtualGeometryOwnerFallbackVisibility(false)
					&& pageLocal.pageLocalDeformationAvailable
					&& !pageLocal.RequiresOwnerMeshDeformationFallback()
					&& !pageLocal.RequiresVirtualGeometryOwnerFallbackVisibility(true)
					&& !legacy.IsComputeRasterDeformationSafe()
					&& pageLocal.IsComputeRasterDeformationSafe()
					&& !bindPose.RequiresOwnerMeshDeformationFallback()
					&& bindPose.IsComputeRasterDeformationSafe()
					&& !safePose.RequiresOwnerMeshDeformationFallback()
					&& safePose.IsComputeRasterDeformationSafe()
					&& !staticBinding.RequiresOwnerMeshDeformationFallback()
					&& renderSystemSource.find(
						"RequiresVirtualGeometryOwnerFallbackVisibility(") != std::string::npos,
				"Pose binding must preserve demanded-page compute eligibility while a rejected page state retains fail-open owner coverage.");
		});

		registry.Add("Renderer.AnimationView.DenseGpuWorkRetainsFullImmutablePlan",
			[](TestContext& context)
		{
			auto snapshot = BuildAllViewSnapshot();
			MakeSnapshotGpuDriven(snapshot);
			const TerraEngine::Renderer::AnimationBackendWorkPlan work =
				TerraEngine::Renderer::BuildAnimationBackendWorkPlan(
					snapshot.frameId, BuildDynamicState(snapshot),
					BuildGpuAnimationPlanFixture());

			context.Expect(work.valid && work.fullImmutablePlanCoverage
				&& work.ranges.empty() && work.gpuAnimatorCount == 1U
				&& work.rendererGeneration == 17U
				&& work.immutablePlan != nullptr,
				"A complete one-to-one GPU crowd must retain its immutable plan without duplicating packet ranges.");
		});

		registry.Add("Renderer.AnimationView.DenseProducerProofSkipsDynamicLaneScan",
			[](TestContext& context)
		{
			const auto immutablePlan = BuildGpuAnimationPlanFixture();
			const TerraEngine::Renderer::AnimationBackendWorkPlan work =
				TerraEngine::Renderer::BuildDenseAnimationBackendWorkPlan(
					101U, 1U, immutablePlan);
			context.Expect(work.valid && work.fullImmutablePlanCoverage
				&& work.ranges.empty() && work.gpuAnimatorCount == 1U
				&& work.rendererGeneration == 17U
				&& work.immutablePlan == immutablePlan,
				"A sealed one-owner/one-packet topology must publish backend work without reopening renderer dynamic pages.");

			const TerraEngine::Renderer::AnimationBackendWorkPlan rejected =
				TerraEngine::Renderer::BuildDenseAnimationBackendWorkPlan(
					102U, 2U, immutablePlan);
			context.Expect(!rejected.valid && rejected.immutablePlan == nullptr
				&& !rejected.rejectionReason.empty(),
				"A mismatched producer owner count must reject the O(1) coverage proof atomically.");
		});

		registry.Add("Renderer.AnimationView.MixedCpuFallbackRetainsDenseGpuPages",
			[](TestContext& context)
		{
			const std::filesystem::path renderSystemPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream renderSystemStream(renderSystemPath, std::ios::binary);
			std::string renderSystemSource{
				std::istreambuf_iterator<char>(renderSystemStream), {} };
			// Source contracts describe tokens and control flow, not checkout line
			// endings. Normalize CRLF so Windows worktrees match the reviewed LF text.
			renderSystemSource.erase(std::remove(renderSystemSource.begin(),
				renderSystemSource.end(), '\r'), renderSystemSource.end());
			const bool denseIgnoresSparseCpuCount = renderSystemSource.find(
				"const bool hasDenseAnimationPublication =\n\t\thasDenseAnimationTopologyCandidate;")
				!= std::string::npos;
			const bool currentCpuPagesMapped = renderSystemSource.find(
				"currentCpuDynamicPageIndices.push_back(") != std::string::npos;
			const bool priorCpuPagesRetired = renderSystemSource.find(
				"markCpuPages(retainedCpuDynamicPageIndices);") != std::string::npos;
			const bool unaffectedPagesSkipped = renderSystemSource.find(
				"if(pagesRequiringDynamicRefresh[pageIndex] == 0U)")
				!= std::string::npos;

			context.Expect(denseIgnoresSparseCpuCount && currentCpuPagesMapped
				&& priorCpuPagesRetired && unaffectedPagesSkipped,
				"Sparse CPU deformation must refresh its current and prior pages while a mixed crowd keeps producer-sealed GPU publication dense.");
		});

		registry.Add("Renderer.AnimationView.VisibleCacheInvalidatesEmptyPreReadyOwnerFallback", [](TestContext& context) {
			const std::filesystem::path renderSystemPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream renderSystemStream(renderSystemPath, std::ios::binary);
			const std::string renderSystemSource{
				std::istreambuf_iterator<char>(renderSystemStream), {} };
			const std::size_t visibilitySignature = renderSystemSource.find(
				"std::uint64_t renderableVisibilitySignature = renderableWorldSignature;");
			const std::size_t ownerFallbackAdmission = renderSystemSource.find(
				"RequiresVirtualGeometryOwnerFallbackVisibility(", visibilitySignature);
			const std::size_t unboundedPoseInvalidation = renderSystemSource.find(
				"if(requiresUnboundedCoarseVisibility)", ownerFallbackAdmission);
			const std::size_t poseGenerationToken = renderSystemSource.find(
				"binding.posePublicationGeneration", unboundedPoseInvalidation);
			const std::size_t visibleCacheKey = renderSystemSource.find(
				"std::uint64_t signature = BuildVisibleCommandCacheKey(",
				poseGenerationToken);
			const std::size_t visibleAdmissionArgument = renderSystemSource.find(
				"renderableVisibilitySignature,", visibleCacheKey);
			const std::size_t topologyGenerationArgument = renderSystemSource.find(
				"HashCombine(signature, retainedRenderTopology.topology.generation);",
				visibleCacheKey);
			const std::size_t authoritativeReuseCounter = renderSystemSource.find(
				"target.renderOptimization.viewEvidenceReuseHits =",
				topologyGenerationArgument);

			context.Expect(visibilitySignature != std::string::npos
				&& ownerFallbackAdmission != std::string::npos
				&& unboundedPoseInvalidation != std::string::npos
				&& poseGenerationToken != std::string::npos
				&& visibleCacheKey != std::string::npos
				&& visibleAdmissionArgument != std::string::npos
				&& topologyGenerationArgument != std::string::npos
				&& authoritativeReuseCounter != std::string::npos,
				"A visible-command cache entry made before animation becomes ready must hash owner-fallback admission state, while pose generations invalidate only unbounded legacy deformation that lacks a conservative envelope.");
		});

		registry.Add("Renderer.AnimationView.CaptureSelectionReuseKeepsReadbackIdentitySeparate", [](TestContext& context) {
			const std::filesystem::path renderSystemPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream renderSystemStream(renderSystemPath, std::ios::binary);
			const std::string source{
				std::istreambuf_iterator<char>(renderSystemStream), {} };
			const std::size_t cacheKeyBegin = source.find(
				"std::uint64_t BuildVisibleCommandCacheKey(");
			const std::size_t cacheKeyEnd = source.find("return seed;", cacheKeyBegin);
			const std::string cacheKeyBody = cacheKeyBegin == std::string::npos
				|| cacheKeyEnd == std::string::npos
				? std::string{}
				: source.substr(cacheKeyBegin, cacheKeyEnd - cacheKeyBegin);

			context.Expect(!cacheKeyBody.empty()
				&& cacheKeyBody.find("captureRequestIdentity") == std::string::npos
				&& source.find("submittedCaptureReadbackRequests.push_back(")
					!= std::string::npos
				&& source.find("result.requestId = request.requestId;")
					!= std::string::npos
				&& source.find(
					"result.targetGeneration != resolvedSnapshot.targetGeneration")
					!= std::string::npos,
				"Repeated identical captures should reuse geometry selection while asynchronous readback retains request and target-generation ownership.");
		});

		registry.Add("Renderer.AnimationView.GroupedCacheInvalidatesEmptyPreReadyAnimation", [](TestContext& context) {
			const std::filesystem::path renderSystemPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream renderSystemStream(renderSystemPath, std::ios::binary);
			const std::string renderSystemSource{
				std::istreambuf_iterator<char>(renderSystemStream), {} };
			const std::size_t compatibilityHash = renderSystemSource.find(
				"const auto dynamicCompatibilityHash");
			const std::size_t animationReadyBit = renderSystemSource.find(
				"combine(command.HasCompleteGpuAnimation() ? 1u : 0u);",
				compatibilityHash);
			const std::size_t membershipLookup = renderSystemSource.find(
				"GroupedSubmissionMembershipCacheRecord& membershipCache",
				compatibilityHash);

			context.Expect(compatibilityHash != std::string::npos
				&& animationReadyBit != std::string::npos
				&& membershipLookup != std::string::npos
				&& animationReadyBit < membershipLookup,
				"A grouped-submission cache populated before GPU animation readiness must rebuild when complete packet identity becomes available.");
		});

		registry.Add("Renderer.AnimationView.GroupedSceneContextRetainsSharedTint", [](TestContext& context) {
			const std::filesystem::path renderSystemPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream renderSystemStream(renderSystemPath, std::ios::binary);
			const std::string renderSystemSource{
				std::istreambuf_iterator<char>(renderSystemStream), {} };
			const std::size_t compatibility = renderSystemSource.find(
				"&& a.editorTint == b.editorTint;");
			const std::size_t tintHash = renderSystemSource.find(
				"combine(std::bit_cast<std::uint32_t>(command.editorTint.x));",
				compatibility);
			const std::size_t instanceTint = renderSystemSource.find(
				"record.editorTint = command.editorTint;", tintHash);

			context.Expect(compatibility != std::string::npos
				&& tintHash != std::string::npos
				&& instanceTint != std::string::npos,
				"Scene context objects with the same tint must remain group-compatible while distinct tint groups retain exact instance evidence.");
		});

		registry.Add("Renderer.AnimationView.OwnerFallbackRetainsGroupedMeshTaskEligibility", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			RenderItemRecord command{};
			command.geometryRoute = GeometryRoute::VirtualGeometryFallback;
			context.Expect(command.UsesGroupableMeshTaskRoute()
				&& !command.UsesVirtualGeometryRoute(),
				"A correctness-preserving owner fallback must remain groupable without being mislabeled as page-local virtual geometry.");

			command.geometryRoute = GeometryRoute::VirtualGeometryHardware;
			context.Expect(command.UsesGroupableMeshTaskRoute()
				&& command.UsesVirtualGeometryRoute(),
				"The hardware virtual-geometry route must remain in both its page-local and grouped task families.");
		});

		registry.Add("Renderer.AnimationView.DedicatedCaptureCollectsWithoutAuthoredCamera", [](TestContext& context) {
			const std::filesystem::path renderSystemPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream renderSystemStream(renderSystemPath, std::ios::binary);
			const std::string renderSystemSource{
				std::istreambuf_iterator<char>(renderSystemStream), {} };
			const std::size_t queuedCaptureState = renderSystemSource.find(
				"bool hasQueuedDedicatedCapture = false;");
			const std::size_t renderableCollection = renderSystemSource.find(
				"Render.CollectRenderables", queuedCaptureState);
			const std::size_t admission = renderSystemSource.find(
				"&& !hasQueuedDedicatedCapture", renderableCollection);

			context.Expect(queuedCaptureState != std::string::npos
				&& renderableCollection != std::string::npos
				&& admission != std::string::npos,
				"A queued custom dedicated capture must collect renderables even when the scene has no authored or Scene View camera.");
		});

		registry.Add("Renderer.AnimationView.DiagnosticsUseSealedDeformationPublication", [](TestContext& context) {
			const std::filesystem::path renderSystemPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream renderSystemStream(renderSystemPath, std::ios::binary);
			const std::string renderSystemSource{
				std::istreambuf_iterator<char>(renderSystemStream), {} };
			const std::size_t transfer = renderSystemSource.find(
				"backendWork->deformationWork = std::move(selectedDeformation.immutablePlan);");
			const std::size_t diagnostics = renderSystemSource.find(
				"BuildVirtualGeometryViewDiagnosticsFromSnapshot(", transfer);
			const std::size_t sealedRoot = renderSystemSource.find(
				"sealedPublication->backendWork->deformationWork.get()", diagnostics);
			const std::size_t staleRoot = renderSystemSource.find(
				"selectedDeformation.immutablePlan.get()", diagnostics);

			context.Expect(transfer != std::string::npos
				&& diagnostics != std::string::npos
				&& sealedRoot != std::string::npos
				&& (staleRoot == std::string::npos || staleRoot > sealedRoot),
				"Post-seal diagnostics must inspect the retained backend deformation root, never the moved-from selection candidate.");
		});

		registry.Add("Renderer.AnimationView.ActiveDeformationUsesProvenCpuBroadphaseEnvelope", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			auto request = BuildGpuVisualBindingRequest();
			request.gpuVisualIdentity = BuildGpuVisualIdentity();
			request.pageLocalDeformationAvailable = true;
			const AnimationRenderBinding active =
				RenderSystem::ResolveAnimationRenderBinding(request);

			AnimationRenderBinding bindPose = active;
			bindPose.source = AnimationRenderPoseSource::BindPose;
			AnimationRenderBinding safePose = active;
			safePose.safePose = true;
			safePose.status = AnimationRenderBindingStatus::SafePose;
			AnimationRenderBinding staticBinding{};

			context.Expect(
				active.RequiresConservativeCoarseVisibility(),
				"Active deformation must still report that its bounds require a conservative proof.");
			context.Expect(
				active.RequiresUnboundedCoarseVisibility(false)
					&& !active.RequiresUnboundedCoarseVisibility(true),
				"An active deformable mesh must fail open without bounds, but a validated whole-animation envelope may safely use CPU broadphase culling.");
			context.Expect(
				!bindPose.RequiresConservativeCoarseVisibility()
					&& !safePose.RequiresConservativeCoarseVisibility()
					&& !staticBinding.RequiresConservativeCoarseVisibility()
					&& !bindPose.RequiresUnboundedCoarseVisibility(false)
					&& !safePose.RequiresUnboundedCoarseVisibility(false)
					&& !staticBinding.RequiresUnboundedCoarseVisibility(false),
				"Bind pose, safe pose, and static meshes may continue using their CPU spatial bounds.");
		});

		registry.Add("Renderer.AnimationView.HybridAndMismatchedGpuIdentityFailOpen", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			auto hybridRequest = BuildGpuVisualBindingRequest();
			auto hybridIdentity = BuildGpuVisualIdentity();
			hybridIdentity.hybridCpuAuthority = true;
			hybridRequest.gpuVisualIdentity = hybridIdentity;
			const auto hybrid = RenderSystem::ResolveAnimationRenderBinding(hybridRequest);

			auto staleRequest = BuildGpuVisualBindingRequest();
			auto staleIdentity = BuildGpuVisualIdentity();
			++staleIdentity.meshGeneration;
			staleRequest.gpuVisualIdentity = staleIdentity;
			const auto stale = RenderSystem::ResolveAnimationRenderBinding(staleRequest);

			context.Expect(hybrid.status != AnimationRenderBindingStatus::Ready
				&& stale.status != AnimationRenderBindingStatus::Ready,
				"Hybrid CPU authority and stale GPU mesh identity must preserve CPU/safe fallback instead of binding GPU work.");
		});

		registry.Add("Renderer.AnimationView.ExactPayloadGenerationRejectsHighBitCollision", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			constexpr std::uint64_t lowBits = 0x00c0ffeeULL;
			constexpr std::uint64_t expectedPayloadGeneration =
				(1ULL << 32U) | lowBits;
			constexpr std::uint64_t stalePayloadGeneration =
				(2ULL << 32U) | lowBits;
			static_assert(
				static_cast<std::uint32_t>(expectedPayloadGeneration)
					== static_cast<std::uint32_t>(stalePayloadGeneration));

			auto request = BuildGpuVisualBindingRequest();
			request.deformablePayloadGeneration = expectedPayloadGeneration;
			request.expectedGpuRenderIdentity.deformablePayloadGeneration =
				expectedPayloadGeneration;
			auto matchingIdentity = BuildGpuVisualIdentity();
			matchingIdentity.renderIdentity.deformablePayloadGeneration =
				expectedPayloadGeneration;
			request.gpuVisualIdentity = matchingIdentity;
			const AnimationRenderBinding matching =
				RenderSystem::ResolveAnimationRenderBinding(request);

			auto staleIdentity = matchingIdentity;
			staleIdentity.renderIdentity.deformablePayloadGeneration =
				stalePayloadGeneration;
			request.gpuVisualIdentity = staleIdentity;
			const AnimationRenderBinding stale =
				RenderSystem::ResolveAnimationRenderBinding(request);

			context.Expect(matching.status == AnimationRenderBindingStatus::Ready
				&& matching.source == AnimationRenderPoseSource::GpuVisual,
				"The complete exact 64-bit payload identity should bind its GPU packet.");
			context.Expect(stale.status != AnimationRenderBindingStatus::Ready
				&& stale.source != AnimationRenderPoseSource::GpuVisual,
				"Payload generations with identical low 32 bits but different high bits must never share GPU palette work.");
		});

		registry.Add("Renderer.AnimationView.ShadowAndCaptureUseTheSameGenerationContract", [](TestContext& context) {
			const auto states = TerraEngine::Renderer::BuildVirtualGeometryViewSelectionStates(BuildAllViewSnapshot());
			const auto shadow = std::find_if(states.begin(), states.end(), [](const auto& state) {
				return state.viewType == RenderViewType::Shadow;
			});
			const auto capture = std::find_if(states.begin(), states.end(), [](const auto& state) {
				return state.outputId == 4u;
			});

			context.Expect(shadow != states.end() && capture != states.end(), "Shadow and offscreen capture must participate in all-view animation submission.");
			if (shadow != states.end() && capture != states.end())
			{
				context.ExpectEqual(12u, shadow->draws.front().range.meshletCount, "Shadow visibility should carry the same renderable coverage as the source snapshot.");
				context.ExpectEqual(12u, capture->draws.front().range.meshletCount, "Offscreen capture visibility should carry the same renderable coverage as the source snapshot.");
			}
			context.Expect(RenderViewType::Scene != RenderViewType::Shadow,
				"Reflection support must use a separately identified view record rather than reusing mutable Scene View state.");
		});

		registry.Add("Renderer.AnimationView.InvalidDynamicBoundsFailOpenToVisibleFallback", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			const VirtualGeometryClusterSelectionResult selection = RenderSystem::SelectVirtualGeometryClusterCut(
				BuildFailOpenBoundsAsset(),
				MakeAnimatedViewTestUuid(10u),
				MakeAnimatedViewTestUuid(11u),
				MakeDefaultVirtualGeometryQualityProfile(),
				512.0f,
				true,
				false);

			context.Expect(selection.range.failOpen, "Unknown animated bounds must select conservative visible fallback instead of culling the deformable mesh.");
			context.Expect(!selection.range.fallbackReason.empty(), "Fail-open animated bounds must retain an actionable diagnostics reason.");
		});

		registry.Add("Renderer.AnimationView.SnapshotHashIncludesAllViewRelevantInputs", [](TestContext& context) {
			auto baseline = BuildAllViewSnapshot();
			auto changed = baseline;
			changed.viewRequests.back().targetSize = glm::uvec2(800u, 450u);

			context.Expect(TerraEngine::Renderer::BuildSnapshotDeterministicHash(baseline)
				!= TerraEngine::Renderer::BuildSnapshotDeterministicHash(changed),
				"Capture-view changes must invalidate immutable submission data rather than reuse another camera's visibility result.");
			changed = baseline;
			++changed.renderables.front().invalidation.residency;
			context.Expect(TerraEngine::Renderer::BuildSnapshotDeterministicHash(baseline)
				!= TerraEngine::Renderer::BuildSnapshotDeterministicHash(changed),
				"Residency-generation changes must invalidate prepared submission data.");
			const auto copiedView = TerraEngine::Renderer::BuildVirtualGeometryViewSelectionState(
				baseline.viewRequests.front(), baseline.renderables);
			context.ExpectEqual(baseline.viewRequests.front().viewGeneration, copiedView.viewGeneration,
				"Worker visibility state must copy the exact camera generation without changing deformation identity.");
		});

		registry.Add("Renderer.AnimationView.GpuCrowdSkipsEmptyCpuDeformationScan", [](TestContext& context) {
			const std::filesystem::path rendererPath =
				std::filesystem::path(__FILE__).parent_path().parent_path()
					.parent_path().parent_path()
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream stream(rendererPath, std::ios::binary);
			const std::string source(
				std::istreambuf_iterator<char>(stream), {});
			context.Expect(source.find("hasCpuDeformationCandidates")
					!= std::string::npos
				&& source.find("std::span<RenderableEntry> cpuDeformationCandidates")
					!= std::string::npos,
				"Collection must carry the final CPU-deformation candidate proof so an all-GPU crowd does not get rescanned.");
		});
	}
}
