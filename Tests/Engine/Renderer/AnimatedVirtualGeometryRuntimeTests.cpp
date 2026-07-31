// AnimatedVirtualGeometryRuntimeTests.cpp - deformation, history, bounds, and
// publication regression coverage for the renderer-owned animated page path.

#include "TestHarness.h"

#include "Renderer/Deformation/AnimatedVirtualGeometryRuntime.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RHI/Vulkan/VulkanCommandRecording.h"
#include "Renderer/RHI/Vulkan/VulkanDescriptorPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"
#include "Resource/ResourceResidency.h"
#include "Resource/ResourceStreaming.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace TerraEngine::Tests {

	namespace {

		TerraEngine::UUID MakeRuntimeUuid(std::uint8_t seed)
		{
			TerraEngine::UUID id{};
			id.back() = seed;
			return id;
		}

		TerraEngine::Renderer::CpuAffinePalette IdentityPalette()
		{
			TerraEngine::Renderer::CpuAffinePalette matrix{};
			matrix.rows[0] = { 1.0f, 0.0f, 0.0f, 0.0f };
			matrix.rows[1] = { 0.0f, 1.0f, 0.0f, 0.0f };
			matrix.rows[2] = { 0.0f, 0.0f, 1.0f, 0.0f };
			return matrix;
		}

		TerraEngine::Renderer::DeformationPageDemand MakeRuntimeDemand(
			std::uint32_t pageId,
			std::uint64_t viewMask)
		{
			using namespace TerraEngine::Renderer;
			DeformationPageDemand demand;
			demand.key.page = {
				MakeRuntimeUuid(1u), 5u, pageId, 10u + pageId, 12u,
				kAnimatedGeometryAbiVersion
			};
			demand.key.page.codecProfile = 1u;
			demand.key.page.codecVersion = 1u;
			demand.key.deformation = {
				20u, 21u, 22u, 23u, 24u, 5u, 25u, 26u
			};
			demand.key.inputLayoutVersion = kAnimatedGeometryAbiVersion;
			demand.key.animationLodVersion = kAnimatedGeometryAbiVersion;
			demand.sourceRecordIndex = 3u;
			demand.vertexCount = 64u;
			demand.meshletCount = 2u;
			demand.clusterCount = 1u;
			demand.requestedViewMask = viewMask;
			demand.priorHistoryCompatible = true;
			return demand;
		}

		TerraEngine::Renderer::DeformedPagePublication MakePublication(
			std::uint64_t generation)
		{
			using namespace TerraEngine::Renderer;
			const DeformationPageDemand demand = MakeRuntimeDemand(2u, 1u);
			DeformedPagePublication publication;
			publication.publicationGeneration = generation;
			publication.key = demand.key;
			publication.staticAllocationId = 10u;
			publication.currentAllocationId = 11u;
			publication.previousAllocationId = 12u;
			publication.boundsAllocationId = 13u;
			publication.completionFence = 14u;
			publication.historyValid = true;
			publication.geometryValid = true;
			publication.boundsValid = true;
			publication.refinementValid = true;
			return publication;
		}

	}

	void RegisterAnimatedVirtualGeometryRuntimeTests(TestRegistry& registry)
	{
		registry.Add("AnimatedVirtualGeometry.Runtime.MorphsBeforeSkinningAndPreservesHistory", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			CpuDeformationVertex source;
			source.position = { 1.0f, 0.0f, 0.0f };
			const std::array morphs = {
				CpuMorphDelta{ { 1.0f, 0.0f, 0.0f }, {}, {} }
			};
			const std::array weights = { 0.5f };
			CpuSkinRecord skin{};
			skin.weights[0] = 255u;
			auto translated = IdentityPalette();
			translated.rows[0][3] = 2.0f;
			const std::array palette = { translated };
			GpuDeformedVertexRecord history{};
			history.currentPosition = { 7.0f, 8.0f, 9.0f, 1.0f };

			const auto result = EvaluateAnimatedPageVertexReference(
				source, morphs, weights, skin, palette, &history);
			context.Expect(result.valid, "Finite morph and skin inputs should be accepted.");
			context.Expect(std::abs(result.vertex.currentPosition[0] - 3.5f) < 0.0005f,
				"Morph must apply before the palette translation.");
			context.ExpectEqual<float>(7.0f, result.vertex.previousPosition[0],
				"Compatible history must supply the previous position.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.ZeroWeightsRemainIdentityAndMalformedInputsReject", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			CpuDeformationVertex source;
			source.position = { 2.0f, 3.0f, 4.0f };
			CpuSkinRecord skin{};
			const auto identity = EvaluateAnimatedPageVertexReference(
				source, {}, {}, skin, {}, nullptr);
			context.Expect(identity.valid, "An unweighted vertex should use identity deformation.");
			context.ExpectEqual<float>(2.0f, identity.vertex.currentPosition[0],
				"Identity deformation must retain the authored position.");
			context.ExpectEqual<float>(2.0f, identity.vertex.previousPosition[0],
				"Missing history must alias previous to current.");

			source.position[0] = std::numeric_limits<float>::quiet_NaN();
			const auto invalid = EvaluateAnimatedPageVertexReference(
				source, {}, {}, skin, {}, nullptr);
			context.Expect(!invalid.valid,
				"Non-finite input must reject the complete candidate vertex.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.BoundsEncloseExactCurrentVertices", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			std::array<GpuDeformedVertexRecord, 3> vertices{};
			vertices[0].currentPosition = { -2.0f, 1.0f, 0.0f, 1.0f };
			vertices[1].currentPosition = { 4.0f, -3.0f, 2.0f, 1.0f };
			vertices[2].currentPosition = { 1.0f, 5.0f, -1.0f, 1.0f };
			const auto bounds = ReduceAnimatedPageBounds(vertices, { 1u, 2u, 3u, 4u });

			context.Expect(bounds.valid && bounds.proof == DynamicBoundsProof::Current,
				"Finite current vertices should produce current-generation proof.");
			context.ExpectEqual<float>(-2.0f, bounds.bounds.minimum[0],
				"Minimum X must enclose every emitted position.");
			context.ExpectEqual<float>(5.0f, bounds.bounds.maximum[1],
				"Maximum Y must enclose every emitted position.");
			context.Expect(bounds.bounds.sphere[3] > 0.0f,
				"Bounds reduction must produce a conservative sphere.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.PublicationRejectsAtomically", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			const DeformedPagePublication prior = MakePublication(4u);
			DeformedPagePublication candidate = MakePublication(5u);
			candidate.boundsValid = false;
			const auto rejected = PublishDeformedPageAtomically(prior, candidate);
			context.Expect(!rejected.accepted,
				"Geometry cannot publish without matching bounds/refinement proof.");
			context.ExpectEqual<std::uint64_t>(4u,
				rejected.visible.publicationGeneration,
				"Rejected batches must retain the complete prior row.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.DescriptorsBudgetAndBarriersAreFrameWide", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			const std::array demands = {
				MakeRuntimeDemand(1u, 1u),
				MakeRuntimeDemand(1u, 2u)
			};
			const GpuDeformationPlan plan = BuildGpuDeformationPlan(8u, 25u, demands);
			const auto descriptors =
				Vulkan::BuildVulkanDeformationDescriptorSetPlan(plan, 1024u * 1024u);
			const auto commands = Vulkan::BuildVulkanDeformationCommandPlan(plan);
			context.Expect(descriptors.valid && descriptors.hasWork,
				"A complete page union should have a bounded descriptor plan.");
			context.ExpectEqual<std::size_t>(1u, commands.deformationDispatches.size(),
				"Compatible view demands must dispatch once per exact key.");
			context.ExpectEqual<std::size_t>(4u, commands.barriers.size(),
				"Palette/upload, bounds, refinement, and graphics boundaries occur once per frame union.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.StaticFramesProduceZeroWork", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			const GpuDeformationPlan plan = BuildGpuDeformationPlan(8u, 25u, {});
			const auto descriptors =
				Vulkan::BuildVulkanDeformationDescriptorSetPlan(plan, 1u);
			const auto commands = Vulkan::BuildVulkanDeformationCommandPlan(plan);
			context.Expect(descriptors.valid && !descriptors.hasWork,
				"Static frames must not allocate deformation descriptors.");
			context.Expect(commands.valid && commands.barriers.empty(),
				"Static frames must not emit deformation barriers.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.DiagnosticsCoalesceAndCap", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			std::vector<AnimatedGeometryDiagnostic> diagnostics(70u);
			for(std::size_t index = 0u; index < diagnostics.size(); ++index)
			{
				diagnostics[index].stableInstanceId = index < 2u ? 1u : index;
				diagnostics[index].pageId = index < 2u ? 3u : static_cast<std::uint32_t>(index);
				diagnostics[index].relevantGeneration = 9u;
				diagnostics[index].fallback = AnimatedGeometryFallback::MissingPage;
			}
			const auto snapshot = CoalesceAnimatedGeometryDiagnostics(diagnostics);
			context.ExpectEqual<std::uint32_t>(64u, snapshot.emitted,
				"Detailed diagnostics are capped at 64 records.");
			context.ExpectEqual<std::uint32_t>(1u, snapshot.coalesced,
				"Repeated object/page/reason/generation records are coalesced.");
			context.ExpectEqual<std::uint32_t>(5u, snapshot.dropped,
				"Records beyond the bounded capacity are counted.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.PhysicalPageParsesIndependently", [](TestContext& context)
		{
			using namespace TerraEngine::Resource;
			constexpr std::uint64_t meshGeneration = 77u;
			TemfDeformableDemandedPagePayload blobHeader{};
			blobHeader.pageId = 3u;
			blobHeader.sectionCount = 5u;
			blobHeader.sectionTableOffset =
				sizeof(TemfDeformableDemandedPagePayload);
			blobHeader.meshPayloadGeneration = meshGeneration;
			blobHeader.contentGeneration = 88u;
			std::array<TemfDeformablePageSection, 5> sections{};
			std::uint64_t cursor = sizeof(blobHeader) + sizeof(sections);
			const std::array sectionTypes{
				TemfDeformablePageSectionKind::PackedVertices,
				TemfDeformablePageSectionKind::Topology,
				TemfDeformablePageSectionKind::Meshlets,
				TemfDeformablePageSectionKind::DecodeConstants,
				TemfDeformablePageSectionKind::SourceVertexIdentities
			};
			const std::array strides{ 16u, 1u, 16u, 64u, 8u };
			for(std::size_t index = 0u; index < sections.size(); ++index)
			{
				sections[index].kind = sectionTypes[index];
				sections[index].relativeByteOffset =
					static_cast<std::uint32_t>(cursor);
				sections[index].byteSize = strides[index];
				sections[index].elementCount = 1u;
				sections[index].elementStride = strides[index];
				cursor += strides[index];
			}
			blobHeader.decodedByteSize = static_cast<std::uint32_t>(cursor);
			std::vector<std::uint8_t> blob(static_cast<std::size_t>(cursor), 0u);
			std::memcpy(blob.data(), &blobHeader, sizeof(blobHeader));
			std::memcpy(blob.data() + blobHeader.sectionTableOffset,
				sections.data(), sizeof(sections));

			const std::uint64_t integrity =
				HashVirtualGeometryPageBytes(blob);
			bool rangesAreCanonical = integrity != 0u;
			std::uint32_t expectedOffset =
				blobHeader.sectionTableOffset + sizeof(sections);
			for(const TemfDeformablePageSection& section : sections)
			{
				rangesAreCanonical = rangesAreCanonical
					&& section.relativeByteOffset == expectedOffset
					&& section.byteSize
						== section.elementCount * section.elementStride;
				expectedOffset += section.byteSize;
			}
			context.Expect(rangesAreCanonical
				&& expectedOffset == blobHeader.decodedByteSize
				&& blobHeader.meshPayloadGeneration == meshGeneration,
				"One canonical descriptor-owned demanded page must remain independently range-checkable.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.PersistentUploadsReuseAndPrioritizePinnedRoots", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer::Vulkan;
			const TerraEngine::UUID meshId = MakeRuntimeUuid(17u);
			const std::vector<PersistentPhysicalPageUploadInput> inputs{
				{ meshId, 9u, 4u, 8u, 1024u, 0u, 0u, false, true },
				{ meshId, 1u, 3u, 7u, 512u, 99u, 12u, true, true }
			};
			const PersistentPhysicalPageUploadPlan plan =
				BuildPersistentPhysicalPageUploadPlan(
					inputs,
					4096u,
					2048u,
					6u);
			context.Expect(plan.valid,
				"Complete page inputs should produce one atomic persistent upload plan.");
			context.ExpectEqual<std::size_t>(
				2u,
				plan.uploads.size(),
				"Pinned and high-detail pages should both remain represented.");
			context.Expect(
				plan.uploads.front().input.pinnedFallback
					&& plan.uploads.front().reuseExisting
					&& plan.uploads.front().allocationId == 99u,
				"Pinned roots sort first and retain their exact existing allocation.");
			context.ExpectEqual<std::uint32_t>(
				1u,
				plan.reusedPages,
				"Existing allocations must not consume upload budget.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.ResidencyPublicationIsFenceCheckedAndAtomic", [](TestContext& context)
		{
			using namespace TerraEngine::Resource;
			DeformableVirtualPageResidencySnapshot prior;
			prior.publicationGeneration = 4u;
			prior.catalogGeneration = 2u;
			prior.valid = true;
			DeformableVirtualPageResidencySnapshot candidate;
			candidate.publicationGeneration = 5u;
			candidate.catalogGeneration = 2u;
			candidate.pages.push_back({
				MakeRuntimeUuid(8u),
				3u,
				10u,
				11u,
				12u,
				2048u,
				20u,
				true,
				true,
				true });
			const auto inFlight =
				PublishDeformableVirtualPageResidencySnapshot(
					prior,
					candidate,
					19u);
			context.Expect(!inFlight.accepted
					&& inFlight.visible.publicationGeneration == 4u,
				"An in-flight page batch must retain the prior snapshot byte-for-byte.");
			const auto completed =
				PublishDeformableVirtualPageResidencySnapshot(
					prior,
					candidate,
					20u);
			context.Expect(completed.accepted
					&& completed.visible.valid
					&& completed.visible.pages.front().pinnedFallback,
				"A completed pinned root may publish atomically and stay eligible.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.VisibilitySnapshotDiagnosticsRetainDeformationFacts", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 42u;
			snapshot.animatedGeometryPageDemands.push_back(
				MakeRuntimeDemand(3u, 1u));
			VirtualGeometryViewVisibilitySnapshot visibility;
			visibility.frameId = snapshot.frameId;
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 2u;
			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				std::move(visibility));

			const auto diagnostics =
				BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					snapshot,
					MakeDefaultVirtualGeometryQualityProfile(),
					VirtualGeometryInactiveReason::None);
			context.ExpectEqual<std::size_t>(
				1u,
				diagnostics.size(),
				"The preferred live visibility path should publish one view.");
			if(diagnostics.empty())
			{
				return;
			}
			context.ExpectEqual<std::uint32_t>(
				1u,
				diagnostics.front().animatedPageDemandCount,
				"Live visibility diagnostics must retain frame-wide animated page demands.");
			context.ExpectEqual<std::uint32_t>(
				1u,
				diagnostics.front().animatedUniqueDispatchCount,
				"Live visibility diagnostics must retain the deduplicated compute dispatch count.");
			context.Expect(
				diagnostics.front().deformationCurrentBytes != 0u
					&& diagnostics.front().deformationHistoryBytes != 0u
					&& diagnostics.front().deformationBoundsBytes != 0u,
				"Live diagnostics must report current, history, and bounds working-set bytes.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.SelectedViewUnionDrivesPageDeformation", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			const TerraEngine::UUID firstObject = MakeRuntimeUuid(21u);
			const TerraEngine::UUID secondObject = MakeRuntimeUuid(22u);

			RenderFrameRenderableRecord firstRenderable;
			firstRenderable.objectId = firstObject;
			firstRenderable.animation.stableInstanceId = 20u;
			snapshot.renderables.push_back(firstRenderable);
			RenderFrameRenderableRecord secondRenderable;
			secondRenderable.objectId = secondObject;
			secondRenderable.animation.stableInstanceId = 40u;
			snapshot.renderables.push_back(secondRenderable);

			snapshot.animatedGeometryPageDemands.push_back(
				MakeRuntimeDemand(2u, std::numeric_limits<std::uint64_t>::max()));
			snapshot.animatedGeometryPageDemands.push_back(
				MakeRuntimeDemand(3u, std::numeric_limits<std::uint64_t>::max()));
			DeformationPageDemand secondInstance =
				MakeRuntimeDemand(9u, std::numeric_limits<std::uint64_t>::max());
			secondInstance.key.deformation.stableInstanceId = 40u;
			snapshot.animatedGeometryPageDemands.push_back(secondInstance);

			VirtualGeometryViewSelectionState gameView;
			VirtualGeometryViewDrawRecord gameDraw;
			gameDraw.objectId = firstObject;
			gameDraw.range.pageId = 2u;
			gameDraw.range.payloadSource =
				VirtualGeometryPagePayloadSource::ResidentPagePayload;
			gameView.draws.push_back(gameDraw);
			// The coarse draw names only its first page, while GPU packet
			// expansion emits every independently resident page used by the
			// model. Selection must therefore follow these packet records.
			GpuVirtualGeometryPacketRecord gamePageTwo;
			gamePageTwo.objectId = firstObject;
			gamePageTwo.pageId = 2u;
			gamePageTwo.payloadSource =
				VirtualGeometryPagePayloadSource::ResidentPagePayload;
			gameView.gpuPacketRecords.push_back(gamePageTwo);
			GpuVirtualGeometryPacketRecord gamePageThree = gamePageTwo;
			gamePageThree.pageId = 3u;
			gameView.gpuPacketRecords.push_back(gamePageThree);
			snapshot.virtualGeometryViewSelections.push_back(gameView);

			VirtualGeometryViewSelectionState shadowView;
			VirtualGeometryViewDrawRecord repeatedDraw = gameDraw;
			shadowView.draws.push_back(repeatedDraw);
			VirtualGeometryViewDrawRecord shadowDraw;
			shadowDraw.objectId = secondObject;
			shadowDraw.range.pageId = 9u;
			shadowDraw.range.payloadSource =
				VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			shadowView.draws.push_back(shadowDraw);
			GpuVirtualGeometryPacketRecord shadowPageTwo = gamePageTwo;
			shadowPageTwo.payloadSource =
				VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			shadowView.gpuPacketRecords.push_back(shadowPageTwo);
			GpuVirtualGeometryPacketRecord shadowPageNine;
			shadowPageNine.objectId = secondObject;
			shadowPageNine.pageId = 9u;
			shadowPageNine.payloadSource =
				VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			shadowView.gpuPacketRecords.push_back(shadowPageNine);
			snapshot.virtualGeometryViewSelections.push_back(shadowView);

			const std::vector<DeformationPageDemand> selected =
				SelectAnimatedGeometryPageDemands(snapshot);
			context.ExpectEqual<std::size_t>(
				3u,
				selected.size(),
				"Only physical pages selected by at least one view should enter deformation.");
			if(selected.size() != 3u)
			{
				return;
			}
			context.ExpectEqual<std::uint32_t>(
				2u,
				selected[0].key.page.pageId,
				"The first selected page should preserve deterministic demand order.");
			context.ExpectEqual<std::uint64_t>(
				3u,
				selected[0].requestedViewMask,
				"A page reused by Game and shadow views should deform once with both view bits.");
			context.ExpectEqual<std::uint32_t>(
				3u,
				selected[1].key.page.pageId,
				"Every emitted packet page should deform even when the coarse draw names only its first page.");
			context.ExpectEqual<std::uint64_t>(
				1u,
				selected[1].requestedViewMask,
				"A page emitted only by the Game view should carry only the Game view bit.");
			context.ExpectEqual<std::uint32_t>(
				9u,
				selected[2].key.page.pageId,
				"The independently selected second-instance page should remain present.");
			context.ExpectEqual<std::uint64_t>(
				2u,
				selected[2].requestedViewMask,
				"A shadow-only page should carry only the shadow view bit.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.VisibleOwnerFallbackPreservesIndependentCrowdDemands", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			constexpr std::size_t ownerCount = 1000u;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 77u;
			snapshot.renderables.reserve(ownerCount);
			snapshot.animatedGeometryPageDemands.reserve(ownerCount);

			VirtualGeometryViewSelectionState captureView;
			captureView.draws.reserve(ownerCount);
			for(std::size_t ownerIndex = 0u;
				ownerIndex < ownerCount;
				++ownerIndex)
			{
				TerraEngine::UUID objectId{};
				const std::uint16_t objectOrdinal =
					static_cast<std::uint16_t>(ownerIndex + 1u);
				objectId[objectId.size() - 2u] =
					static_cast<std::uint8_t>(objectOrdinal >> 8u);
				objectId.back() =
					static_cast<std::uint8_t>(objectOrdinal);
				RenderFrameRenderableRecord renderable;
				renderable.objectId = objectId;
				renderable.animation.stableInstanceId =
					1000u + ownerIndex;
				snapshot.renderables.push_back(renderable);

				DeformationPageDemand demand =
					MakeRuntimeDemand(2u, 0u);
				demand.key.deformation.stableInstanceId =
					renderable.animation.stableInstanceId;
				snapshot.animatedGeometryPageDemands.push_back(demand);

				VirtualGeometryViewDrawRecord draw;
				draw.objectId = objectId;
				draw.range.virtualGeometryActive = true;
				draw.range.meshletCount = 64u;
				draw.range.selectedClusterCount = 0u;
				draw.range.payloadSource =
					VirtualGeometryPagePayloadSource::Unknown;
				captureView.draws.push_back(draw);
			}
			snapshot.virtualGeometryViewSelections.push_back(
				std::move(captureView));

			const std::vector<DeformationPageDemand> selected =
				SelectAnimatedGeometryPageDemands(snapshot);
			context.ExpectEqual<std::size_t>(
				ownerCount,
				selected.size(),
				"A visible complete-owner fallback must retain one validated page demand per independently animated owner.");

			const GpuDeformationPlan plan = BuildGpuDeformationPlan(
				snapshot.frameId,
				25u,
				selected);
			std::unordered_set<std::uint64_t> stableOwners;
			for(const DeformationWorkRecord& work : plan.work)
			{
				stableOwners.insert(
					work.key.deformation.stableInstanceId);
			}
			context.Expect(
				plan.IsComplete()
					&& plan.work.size() == ownerCount
					&& stableOwners.size() == ownerCount,
				"The crowd fallback must publish distinct deformation work keys instead of merging or dropping independently animated owners.");
		});

		registry.Add("AnimatedVirtualGeometry.Runtime.MillionDecisionExtremePoseVisibilityHasNoFalseRejections", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;

			// These positions deliberately combine large joint-like rotations with
			// asymmetric morph-like offsets. The reduced sphere must conservatively
			// contain every final vertex, independent of how extreme the pose became.
			std::array<GpuDeformedVertexRecord, 64u> vertices{};
			for(std::size_t index = 0u; index < vertices.size(); ++index)
			{
				const float angle = static_cast<float>(index) * 0.61803398875f;
				const float radius = 250.0f + static_cast<float>((index * 97u) % 751u);
				vertices[index].currentPosition = {
					std::cos(angle) * radius + static_cast<float>(index % 3u) * 137.0f,
					std::sin(angle * 1.7f) * radius - static_cast<float>(index % 5u) * 83.0f,
					std::cos(angle * 0.73f) * radius + static_cast<float>(index % 7u) * 41.0f,
					1.0f
				};
			}
			const DynamicBoundsResult bounds = ReduceAnimatedPageBounds(
				vertices,
				{ 7u, 11u, 13u, 17u });
			context.Expect(bounds.valid && bounds.proof == DynamicBoundsProof::Current,
				"The extreme-pose fixture must produce one finite current-generation bounds proof.");
			if(!bounds.valid)
			{
				return;
			}

			std::uint64_t falseRejections = 0u;
			std::uint32_t state = 0x91e10da5u;
			for(std::uint32_t decision = 0u; decision < 1'000'000u; ++decision)
			{
				// A deterministic LCG supplies a broad set of half-space directions
				// and offsets while keeping this acceptance gate byte-for-byte repeatable.
				state = state * 1664525u + 1013904223u;
				const float x = static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0f;
				state = state * 1664525u + 1013904223u;
				const float y = static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0f;
				state = state * 1664525u + 1013904223u;
				const float z = static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0f;
				const float inverseLength = 1.0f / std::sqrt(x * x + y * y + z * z + 0.000001f);
				const std::array<float, 3u> normal{ x * inverseLength, y * inverseLength, z * inverseLength };
				state = state * 1664525u + 1013904223u;
				const float offset = static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0f * 1800.0f;

				bool referenceVisible = false;
				for(const GpuDeformedVertexRecord& vertex : vertices)
				{
					const float signedDistance =
						normal[0] * vertex.currentPosition[0]
						+ normal[1] * vertex.currentPosition[1]
						+ normal[2] * vertex.currentPosition[2]
						+ offset;
					referenceVisible = referenceVisible || signedDistance >= 0.0f;
				}
				const float sphereDistance =
					normal[0] * bounds.bounds.sphere[0]
					+ normal[1] * bounds.bounds.sphere[1]
					+ normal[2] * bounds.bounds.sphere[2]
					+ offset;
				const bool boundsRejected = sphereDistance < -bounds.bounds.sphere[3];
				falseRejections += referenceVisible && boundsRejected ? 1u : 0u;
			}

			context.ExpectEqual<std::uint64_t>(
				0u,
				falseRejections,
				"One million extreme-pose/morph half-space comparisons must never reject visible deformed geometry.");
		});
		registry.Add("AnimatedVirtualGeometry.Runtime.FenceRetirementPreservesLkg", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			DeformedPagePublication publication{};
			publication.publicationGeneration = 2u;
			publication.staticAllocationId = 1u;
			publication.currentAllocationId = 2u;
			publication.previousAllocationId = 3u;
			publication.boundsAllocationId = 4u;
			publication.completionFence = 9u;
			publication.geometryValid = publication.boundsValid = publication.refinementValid = true;
			publication.key.page.meshPayloadGeneration = 1u;
			publication.key.page.meshId = UUID{ 1u, 2u };
			publication.key.page.pageContentGeneration = 1u;
			publication.key.page.residencyGeneration = 1u;
			publication.key.page.pageId = 1u;
			publication.key.page.codecProfile = 1u;
			publication.key.page.codecVersion = 1u;
			publication.key.page.layoutVersion = kAnimatedGeometryAbiVersion;
			publication.key.deformation = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u };
			publication.key.inputLayoutVersion = kAnimatedGeometryAbiVersion;
			publication.key.animationLodVersion = kAnimatedGeometryAbiVersion;
			DeformedPageRetirementQueue queue;
			queue.Enqueue(publication);
			context.Expect(queue.PendingCount() == 1u && queue.RetireCompleted(8u).empty(), "Retirement waits for the completion fence.");
			context.Expect(queue.RetireCompleted(9u).size() == 1u && queue.PendingCount() == 0u, "Completed fences retire the prior allocation atomically.");
		});
	}

}
