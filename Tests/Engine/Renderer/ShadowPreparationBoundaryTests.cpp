#include "TestHarness.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderSystem.h"
#include "RendererThreadingTestFixtures.h"

#include <algorithm>
#include <memory>
#include <string_view>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Renderer;
		AnimationRenderGenerationRecord AnimationInput()
		{
			AnimationRenderGenerationRecord source;
			source.stableInstanceId = 11; source.poseGeneration = 13; source.morphGeneration = 17;
			source.paletteGeneration = 19; source.deformablePayloadGeneration = 23;
			source.programGeneration = 29; source.animationPlanGeneration = 31; source.rendererGeneration = 37;
			source.deformationContentIdLow = 41; source.deformationContentIdHigh = 43;
			source.firstGpuWorkPacket = 47; source.gpuWorkPacketCount = 1;
			source.source = AnimationRenderEvaluationSource::GpuVisual; source.complete = true;
			return source;
		}
		struct CapturedShadowFixture
		{
			std::shared_ptr<const RenderFramePublication> publication;
			RenderReferenceFrame reference;
			ShadowDrawList selection;
			TerraEngine::UUID objectId, meshId, materialId;
		};
		CapturedShadowFixture Capture(const AnimationRenderGenerationRecord& workerSource, std::uint64_t frame = 1)
		{
			CapturedShadowFixture fixture;
			fixture.objectId = GenerateUUID(); fixture.meshId = GenerateUUID(); fixture.materialId = GenerateUUID();
			RenderPersistentSlot slot;
			slot.key = {0, 17}; slot.objectId = fixture.objectId; slot.meshId = fixture.meshId; slot.materialId = fixture.materialId;
			slot.meshGeneration = 2; slot.materialGeneration = 3; slot.resourceGeneration = 5;
			slot.packetTemplateIdentity = 7; slot.occupied = true;
			auto persistentPage = std::make_shared<RenderPersistentPage>();
			persistentPage->generation = 17; persistentPage->slots.push_back(slot);
			RenderDynamicLane dynamic;
			dynamic.key = slot.key; dynamic.occupied = true; dynamic.hasCanonicalWorldPose = true;
			dynamic.canonicalWorldPose.position.offsetMetres.x = 51.5;
			dynamic.animation = TerraEngine::Renderer::BuildPublishedAnimationGeneration(workerSource);
			auto dynamicPage = std::make_shared<RenderDynamicPage>();
			dynamicPage->generation = 1; dynamicPage->lanes.push_back(dynamic); dynamicPage->changedLanes.push_back(true);
			auto candidate = std::make_shared<RenderFramePublication>();
			candidate->frameId = frame; candidate->generation = frame; candidate->rendererSessionGeneration = 1;
			candidate->persistent = std::make_shared<RenderPersistentTopologyRoot>(RenderPersistentTopologyRoot{17, 1, {persistentPage}});
			candidate->dynamic = std::make_shared<RenderDynamicStateRoot>(RenderDynamicStateRoot{1, 1, {dynamicPage}});
			auto backend = std::make_shared<RenderBackendWorkPublication>();
			backend->frameId = frame; backend->valid = true; backend->animationWork.frameId = frame; backend->animationWork.valid = true;
			candidate->backendWork = backend;
			fixture.reference = MakeRendererTestReferenceFrame(frame, 1, 1);
			fixture.reference.canonicalOrigin.offsetMetres.x = 50.0;
			candidate->referenceFrames.push_back(fixture.reference);
			fixture.selection.lightId = GenerateUUID(); fixture.selection.topologyGeneration = 17;
			fixture.selection.stableOrdinals = {0}; fixture.selection.selectedLods = {3};
			fixture.selection.routeSelectors = {static_cast<std::uint32_t>(GeometryRoute::VirtualGeometryHardware)};
			fixture.selection.editorTints = {glm::vec4(0)};
			RenderViewCommandLane lane;
			lane.virtualMeshletOffset = 59; lane.virtualMeshletCount = 61;
			lane.virtualGeometryPageId = 67; lane.virtualGeometryPagePayloadByteOffset = 71;
			lane.virtualGeometryPagePayloadByteSize = 73;
			fixture.selection.commandLanes = {lane};
			// Transfer all mutable construction owners. The resolver receives only the
			// exact immutable roots and a copied light-specific selection thereafter.
			fixture.publication = std::move(candidate);
			return fixture;
		}
		RenderItemRecord Resolve(TestContext& context, const CapturedShadowFixture& fixture)
		{
			context.Expect(SealRenderFramePublication(fixture.publication, 1).IsValid(), "The fixture must pass production sealing before shadow consumption.");
			RenderItemRecord output;
			context.Expect(ResolveShadowDrawListItem(*fixture.publication, fixture.reference, fixture.selection, 0, output),
				"Production shadow resolver must consume the sealed light-specific item.");
			return output;
		}
		bool Contains(const std::vector<std::string>& values, std::string_view name)
		{
			return std::find(values.begin(), values.end(), name) != values.end();
		}
	}

	void RegisterShadowPreparationBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.ShadowBoundary.SealedLightInstanceStreamIgnoresCameraMembership", [](TestContext& context) {
			auto fixture = Capture(AnimationInput());
			RenderViewPresentationInput view; view.referenceFrame = fixture.reference;
			view.output.diagnosticsView = RenderViewType::Scene; view.output.outputId = 7;
			fixture.selection.instanceInputs = BuildShadowPreparedInstanceInputs(*fixture.publication, view, fixture.selection);
			context.Expect(ShadowPreparedInstanceInputsCurrent(*fixture.publication, view, fixture.selection),
				"A light-selected caster remains valid without any camera-visible selection.");
			const auto& record = fixture.selection.instanceInputs->records.front();
			context.Expect(record.modelMatrix[3][0] == 1.5f && record.geometrySelectors.z == 59
				&& record.geometrySelectors.w == 61 && record.virtualGeometryPayloadSelectors.z == 71,
				"Worker records must retain the localized transform and exact light-selected page/range bytes.");
			view.output.outputId = 8;
			context.Expect(!ShadowPreparedInstanceInputsCurrent(*fixture.publication, view, fixture.selection),
				"A stream from another output cannot authorize this light's grouped draw.");
			view.output.outputId = 7; ++view.referenceFrame.originGeneration;
			context.Expect(!ShadowPreparedInstanceInputsCurrent(*fixture.publication, view, fixture.selection),
				"Rebased view generations must reseal their per-light transforms.");
			view.referenceFrame = fixture.reference;
			view.referenceFrame.canonicalOrigin.offsetMetres.x += 1.0;
			context.Expect(!ShadowPreparedInstanceInputsCurrent(*fixture.publication, view, fixture.selection),
				"Exact origin values also qualify the stream when a producer mistakenly retains its generation.");
		});
		registry.Add("Renderer.Efficiency.ShadowBoundary.MorphAndExactDeformationContentSurvivePublication", [](TestContext& context) {
			const auto source = AnimationInput();
			const auto published = TerraEngine::Renderer::BuildPublishedAnimationGeneration(source);
			context.ExpectEqual(source.poseGeneration, published.poseGeneration, "Effective pose identity must survive compact publication.");
			context.ExpectEqual(source.morphGeneration, published.morphGeneration, "Stationary morph casters require their effective morph generation.");
			context.Expect(published.deformationContentIdLow == source.deformationContentIdLow
				&& published.deformationContentIdHigh == source.deformationContentIdHigh,
				"Both exact deformation content identity halves must survive worker-to-backend capture.");
		});
		registry.Add("Renderer.Efficiency.ShadowBoundary.RetainsCanonicalIdentityAndObjectLifetime", [](TestContext& context) {
			const auto fixture = Capture(AnimationInput()); const auto item = Resolve(context, fixture);
			context.Expect(item.objectId == fixture.objectId && item.meshId == fixture.meshId && item.materialId == fixture.materialId,
				"Object, geometry and material identity must come from the selected immutable slot.");
			context.ExpectEqual(std::uint64_t{17}, item.objectLifetimeGeneration, "Recycled object ordinals require their exact lifetime qualifier.");
			context.ExpectEqual(51.5, item.canonicalWorldPose.position.offsetMetres.x, "Shadow identity must retain the canonical pose, not reconstruct it from a rounded view matrix.");
			context.ExpectEqual(1.5f, item.modelMatrix[3][0], "View consumption localizes canonical position against the copied origin.");
		});
		registry.Add("Renderer.Efficiency.ShadowBoundary.WorkerSourceCannotMutateCapturedSemanticInputs", [](TestContext& context) {
			auto source = AnimationInput(); const auto original = source; const auto fixture = Capture(source);
			source.poseGeneration = 101; source.morphGeneration = 103; source.deformationContentIdLow = 107;
			const auto item = Resolve(context, fixture);
			context.Expect(item.animation.poseGeneration == original.poseGeneration && item.animation.morphGeneration == original.morphGeneration
				&& item.animation.deformationContentIdLow == original.deformationContentIdLow,
				"Later worker input edits cannot alter the immutable caster publication.");
		});
		registry.Add("Renderer.Efficiency.ShadowBoundary.LightSpecificSelectionRetainsCoverageAndRejectsStaleKeys", [](TestContext& context) {
			auto fixture = Capture(AnimationInput()); const auto item = Resolve(context, fixture);
			context.Expect(item.selectedVirtualLod == 3 && item.virtualMeshletOffset == 59 && item.virtualMeshletCount == 61
				&& item.virtualGeometryPageId == 67 && item.virtualGeometryPagePayloadByteOffset == 71 && item.virtualGeometryPagePayloadByteSize == 73,
				"Shadow caster coverage uses the light's selected LOD, ranges and page provenance.");
			RenderItemRecord sentinel; sentinel.objectId = GenerateUUID(); const auto original = sentinel.objectId;
			fixture.selection.topologyGeneration = 18;
			context.Expect(!ResolveShadowDrawListItem(*fixture.publication, fixture.reference, fixture.selection, 0, sentinel), "A stale lifetime/topology qualifier must reject before backend preparation.");
			context.Expect(sentinel.objectId == original, "Rejected resolution must not partially replace caller output.");
		});
		registry.Add("Renderer.Efficiency.ShadowBoundary.StampUsesOwnedBaseAndExactSelection", [](TestContext& context) {
			RenderItemRecord item{}; auto base = std::make_shared<ShadowCasterStamp>();
			base->lifetimeGeneration = 71; base->evidenceMask = kCompleteShadowCasterEvidence;
			base->staticSupport = {true, true, true, true, true, true};
			const auto capturedStaticSupport = base->staticSupport;
			base->deformationPalette.push_back({}); base->morphWeights = {0.25};
			item.shadowCasterInputs = base; item.objectLifetimeGeneration = 72;
			// Inputs are copied before parallel preparation, never borrowed from scene storage.
			item.geometryRoute = GeometryRoute::VirtualGeometryHardware;
			item.selectedVirtualClusterCount = 1; item.selectedVirtualClusterOffset = 100;
			item.virtualMeshletCount = 7; item.virtualGeometryFlags = 16;
			item.canonicalWorldPose.position.cell.x = 123;
			item.animation = BuildPublishedAnimationGeneration(AnimationInput());
			auto reference = MakeRendererTestReferenceFrame(1, 1, 1);
			const auto missing = BuildShadowCasterStampInput(item, reference, 19);
			const auto exact = BuildShadowCasterStampInput(item, reference, 19, 83);
			// Static eligibility is copied with the owned semantic inputs. A later
			// producer edit cannot change an already prepared classification proof.
			base->staticSupport = {};
			context.Expect(exact.semantic.staticSupport == capturedStaticSupport,
				"All copied static classification proofs survive exact selection preparation independently of producer edits.");
			// Restore this independent input before testing presentation-only changes;
			// static support remains part of exact semantic equality.
			base->staticSupport = capturedStaticSupport;
			context.Expect((exact.semantic.evidenceMask & static_cast<std::uint64_t>(ShadowCasterEvidence::Selection)) != 0,
				"A proven owner and exact single cluster establish Selection evidence.");
			context.ExpectEqual(std::uint64_t{71}, exact.semantic.lifetimeGeneration, "Actual captured object lifetime takes precedence over persistent slot compatibility.");
			context.Expect(exact.semantic.segmentId == 19 && exact.semantic.worldPosition.cell[0] == 123
				&& exact.semantic.firstCluster == 83, "The pure helper merges exact canonical and selected cluster inputs.");
			context.Expect((missing.semantic.evidenceMask & static_cast<std::uint64_t>(ShadowCasterEvidence::Selection)) == 0,
				"A transient packet offset cannot prove selected cluster identity.");
			context.Expect((exact.semantic.evidenceMask & static_cast<std::uint64_t>(ShadowCasterEvidence::Deformation)) == 0
				&& exact.semantic.deformationPalette.empty() && exact.semantic.morphWeights.empty()
				&& exact.semantic.deformationContentIdLow == 0, "GPU work cannot retain stale exact CPU deformation proof.");
			context.ExpectEqual(std::uint32_t{16}, exact.semantic.selectionFlags, "Actual owner-deformation and selection flags affect shadow silhouette.");
			item.selectedVirtualClusterCount = 0;
			const auto ownerSpan = BuildShadowCasterStampInput(item, reference, 19);
			context.Expect((ownerSpan.semantic.evidenceMask & static_cast<std::uint64_t>(ShadowCasterEvidence::Selection)) != 0,
				"A virtual route drawing the complete resident owner span needs no sparse cluster identity.");
			item.selectedVirtualClusterCount = 2;
			const auto multiple = BuildShadowCasterStampInput(item, reference, 19, 83);
			context.Expect((multiple.semantic.evidenceMask & static_cast<std::uint64_t>(ShadowCasterEvidence::Selection)) == 0,
				"One cluster identity cannot prove multiple sparse selected rows.");
			item.selectedVirtualClusterCount = 1;
			item.selectedVirtualClusterOffset = 900; item.animation.paletteGeneration = 901; item.animation.animationPlanGeneration = 902;
			const auto republished = BuildShadowCasterStampInput(item, reference, 19, 83);
			context.Expect(exact.semantic == republished.semantic, "Packet offsets and palette/plan presentation epochs are excluded from semantic equality.");
			item.animation.source = static_cast<std::uint8_t>(AnimationRenderEvaluationSource::CpuPose);
			const auto cpu = BuildShadowCasterStampInput(item, reference, 19, 83);
			context.Expect(cpu.semantic.deformationPalette == base->deformationPalette && cpu.semantic.morphWeights == base->morphWeights,
				"CPU fallback retains owned exact shader input arrays beyond the content fingerprint.");
			item.shadowCasterInputs.reset(); item.geometryRoute = GeometryRoute::DirectMeshlet;
			const auto unowned = BuildShadowCasterStampInput(item, reference, 19);
			context.Expect((unowned.semantic.evidenceMask & static_cast<std::uint64_t>(ShadowCasterEvidence::Selection)) == 0,
				"A command without captured geometry ownership cannot manufacture Selection proof.");
		});
		registry.Add("Renderer.Efficiency.ShadowBoundary.SettingsCopyObservesOnlyProtectedCapture", [](TestContext& context) {
			RendererEfficiencyOptionsQueue queue; RenderFrameSnapshot captured;
			captured.efficiencyOptions = queue.Snapshot(); auto requested = captured.efficiencyOptions.options;
			requested.shadowReuse = RendererEfficiencyShadowReuse::AlwaysRefresh;
			const auto receipt = queue.Queue(requested);
			context.Expect(receipt.accepted && queue.Snapshot().options != requested, "Queued controls are not visible before protected capture.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "The existing protected settings boundary must publish queued controls.");
			context.Expect(captured.efficiencyOptions.options != requested, "A worker's prior immutable frame input cannot change retroactively.");
			RenderFrameSnapshot next; next.efficiencyOptions = queue.Snapshot();
			context.Expect(next.efficiencyOptions.options == requested && next.efficiencyOptions.settingsGeneration == receipt.requestedGeneration,
				"The first subsequent capture observes the exact queued generation without a scene flush.");
		});
		registry.Add("Renderer.Efficiency.ShadowBoundary.SchedulerKeepsImmutablePreparationAndExistingBoundaries", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			const auto declarations = RenderSystem::Instance().DeclareFrameWorkloads(0);
			context.ExpectEqual(declarations.size(), std::size_t{5}, "Shadow policy must reuse existing capture, preparation and commit workloads.");
			for(const auto& workload : declarations)
			{
				context.Expect(!workload.reads.empty() && !workload.writes.empty(), "Executable renderer work must declare resource ownership.");
				if(workload.workerEligible)
				{
					if (workload.workloadName == "VirtualGeometryPrepare")
					{
						context.Expect(Contains(workload.writes, "ShadowGroupedPreparedInputs"),
							"Worker preparation declares ownership of the full light-specific shadow stream.");
						context.Expect(Contains(workload.writes, "StaticShadowClassificationInputs"),
							"Worker preparation owns copied static classification inputs without changing live scene storage.");
					}
					context.Expect(Contains(workload.reads, "RendererEfficiencyOptions"), "Workers must read captured options, not the mutable ingress queue.");
					context.Expect(!Contains(workload.reads, "ObjectManager") && !Contains(workload.writes, "ObjectManager")
						&& !Contains(workload.writes, "SceneGraph") && !Contains(workload.writes, "SceneMutationQueue"), "Shadow preparation cannot mutate or reopen live scene storage.");
					context.Expect(!workload.sameFrameVisibilityRequired && !workload.safeSubwork.empty(), "Immutable shadow preparation adds no scene visibility flush.");
				}
				else context.Expect(workload.mainThreadRequired && workload.boundaryClassification == HardBoundaryClassification::Protected
					&& !workload.protectedCommit.empty(), "Live capture and backend publication remain explicitly protected.");
			}
			const auto commit = std::find_if(declarations.begin(), declarations.end(), [](const auto& row) { return row.workloadName == "BackendCommit"; });
			context.Expect(commit != declarations.end() && Contains(commit->reads, "ShadowGroupedPreparedInputs")
				&& Contains(commit->writes, "ShadowGroupedPhysicalOwners") && Contains(commit->writes, "ShadowGroupedExecutionEvidence"),
				"Native upload ownership and actual shadow command receipts must remain declared at the protected commit.");
			context.Expect(commit != declarations.end() && Contains(commit->reads, "StaticShadowClassificationInputs")
				&& Contains(commit->reads, "StaticShadowPhysicalOwners") && Contains(commit->writes, "StaticShadowPhysicalOwners")
				&& Contains(commit->writes, "StaticShadowExecutionEvidence"),
				"Protected commit owns physical static cache references and actual recording evidence, independently of worker classification.");
		});
	}
}
