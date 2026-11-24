#include "RenderGraph.h"
#include "Renderer/GraphicsSettings/PostProcessAuthoringFrame.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsCatalog.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"
#include "RenderFramePublication.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace TerraEngine::Renderer {

	std::string ToString(RenderWorkPolicy policy)
	{
		switch (policy)
		{
		case RenderWorkPolicy::WorkerEligible:
			return "worker";
		case RenderWorkPolicy::SerialRequired:
			return "serial";
		case RenderWorkPolicy::FallbackOnly:
			return "fallback";
		default:
			return "unknown";
		}
	}

	std::string ToString(RenderPacketExecutionPolicy policy)
	{
		switch (policy)
		{
		case RenderPacketExecutionPolicy::Worker:
			return "worker";
		case RenderPacketExecutionPolicy::Serial:
			return "serial";
		case RenderPacketExecutionPolicy::Skipped:
			return "skipped";
		case RenderPacketExecutionPolicy::Failed:
			return "failed";
		default:
			return "unknown";
		}
	}

	std::string ToString(RenderFailurePolicy policy)
	{
		switch (policy)
		{
		case RenderFailurePolicy::Fatal: return "fatal";
		case RenderFailurePolicy::Degrade: return "degrade";
		case RenderFailurePolicy::Skip: return "skip";
		case RenderFailurePolicy::Retry: return "retry";
		case RenderFailurePolicy::DiagnosticsOnly: return "diagnostics-only";
		default: return "unknown";
		}
	}

	std::string ToString(RenderWorkBudgetCategory category)
	{
		switch (category)
		{
		case RenderWorkBudgetCategory::Upload: return "upload";
		case RenderWorkBudgetCategory::DrawPreparation: return "draw-preparation";
		case RenderWorkBudgetCategory::ShadowPreparation: return "shadow-preparation";
		case RenderWorkBudgetCategory::UiPreparation: return "ui-preparation";
		case RenderWorkBudgetCategory::PipelinePreparation: return "pipeline-preparation";
		case RenderWorkBudgetCategory::Readback: return "readback";
		case RenderWorkBudgetCategory::TransientMemory: return "transient-memory";
		case RenderWorkBudgetCategory::WorkerScratchMemory: return "worker-scratch-memory";
		case RenderWorkBudgetCategory::DeferredRetirement: return "deferred-retirement";
		default: return "unknown";
		}
	}

	std::string ToString(RenderBudgetOverflowPolicy policy)
	{
		switch (policy)
		{
		case RenderBudgetOverflowPolicy::Delay: return "delay";
		case RenderBudgetOverflowPolicy::Cancel: return "cancel";
		case RenderBudgetOverflowPolicy::Degrade: return "degrade";
		case RenderBudgetOverflowPolicy::Serialize: return "serialize";
		case RenderBudgetOverflowPolicy::Reject: return "reject";
		default: return "unknown";
		}
	}

	std::string ToString(RenderQueueCategory category)
	{
		switch (category)
		{
		case RenderQueueCategory::Transfer: return "transfer";
		case RenderQueueCategory::Compute: return "compute";
		case RenderQueueCategory::Graphics: return "graphics";
		case RenderQueueCategory::Presentation: return "presentation";
		default: return "unknown";
		}
	}

	std::string ToString(RenderQueueMode mode)
	{
		switch (mode)
		{
		case RenderQueueMode::Parallel: return "parallel";
		case RenderQueueMode::Serial: return "serial";
		case RenderQueueMode::Unavailable: return "unavailable";
		case RenderQueueMode::Fallback: return "fallback";
		default: return "unknown";
		}
	}

	bool IsWriteAccess(RenderAccessMode mode)
	{
		return mode == RenderAccessMode::Write
			|| mode == RenderAccessMode::ReadWrite
			|| mode == RenderAccessMode::Present
			|| mode == RenderAccessMode::Upload
			|| mode == RenderAccessMode::Resolve;
	}

	bool IsCoordinatorOwnedAccess(RenderAccessMode mode)
	{
		return mode == RenderAccessMode::Present;
	}

	bool IsRenderWorkOverBudget(const RenderWorkBudget& budget)
	{
		return budget.limit > 0u && budget.currentUsage > budget.limit;
	}

	bool IsCancellationTokenCurrent(const RenderCancellationToken& token, std::uint64_t ownerRevision, std::uint64_t frameId)
	{
		return token.ownerRevision == ownerRevision && token.frameId == frameId;
	}

	bool IsRenderWorkPacketStale(const RenderWorkPacket& packet, std::uint64_t ownerRevision, std::uint64_t frameId)
	{
		if (packet.state == RenderPacketState::Stale || packet.state == RenderPacketState::Cancelled)
		{
			return true;
		}
		return !IsCancellationTokenCurrent(packet.cancellationToken, ownerRevision, frameId);
	}

	namespace {
		std::uint64_t GraphViewIdentity(const RenderFrameViewRequest& view)
		{
			// Native recording is bounded to a handful of view snapshots. Seven
			// explicit instance bits allow 128 views per physical output while leaving
			// eight effect-resource bits and their high domain bit disjoint. Reject
			// excess instead of truncating an ordinal into another camera's resources.
			if(view.viewInstanceIndex > 127u) throw std::length_error("Render graph physical output exceeds its bounded view-instance identity capacity.");
			return (std::uint64_t(view.viewInstanceIndex) << 48u) | (std::uint64_t(view.outputKind) << 40u)
				| (std::uint64_t(view.viewType) << 32u) | std::uint64_t(view.outputId);
		}

		void HashCombine(std::uint64_t& seed, std::uint64_t value)
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
		}

		std::uint64_t HashUuid(const TerraEngine::UUID& id)
		{
			return static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(id));
		}

		std::uint64_t HashWorldPose(const TerraEngine::Scene::WorldPose& pose)
		{
			std::uint64_t hash = static_cast<std::uint64_t>(
				TerraEngine::Scene::WorldPositionHash{}(pose.position));
			const float values[] = {
				pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w,
				pose.scale.x, pose.scale.y, pose.scale.z};
			for (const float value : values)
			{
				std::uint32_t bits = 0u;
				std::memcpy(&bits, &value, sizeof(float));
				HashCombine(hash, bits);
			}
			return hash;
		}

		std::uint64_t HashVirtualGeometryRange(const VirtualGeometryDrawRange& range)
		{
			std::uint64_t hash = 1469598103934665603ull;
			HashCombine(hash, range.selectedLodIndex);
			HashCombine(hash, range.meshletOffset);
			HashCombine(hash, range.meshletCount);
			HashCombine(hash, range.virtualGeometryActive ? 1u : 0u);
			HashCombine(hash, range.failOpen ? 1u : 0u);
			HashCombine(hash, range.hiZSelfOcclusionGuardPassed ? 1u : 0u);
			HashCombine(hash, range.hiZNearPlaneGuardPassed ? 1u : 0u);
			HashCombine(hash, range.hiZThinGeometryGuardPassed ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(range.hiZInactiveReason)));
			HashCombine(hash, range.selectedClusterOffset);
			HashCombine(hash, range.selectedClusterCount);
			HashCombine(hash, range.pageNeedOffset);
			HashCombine(hash, range.pageNeedCount);
			HashCombine(hash, range.pageId);
			HashCombine(hash, range.fallbackPageId);
			HashCombine(hash, static_cast<std::uint64_t>(range.payloadSource));
			HashCombine(hash, static_cast<std::uint64_t>(range.emittedSource));
			HashCombine(hash, range.pagePayloadByteOffset);
			HashCombine(hash, range.pagePayloadByteSize);
			return hash;
		}
	}

	PreparedDrawPacketSignature BuildPreparedDrawPacketSignature(const RenderFrameRenderableRecord& record)
	{
		PreparedDrawPacketSignature signature;
		signature.objectId = record.objectId;
		signature.meshId = record.meshId;
		signature.materialId = record.materialId;
		signature.transformHash = HashWorldPose(record.canonicalWorldPose);
		signature.visibilityHash = record.taskShaderCullingEnabled ? 1u : 0u;
		signature.overrideHash = record.usingFallback ? 1u : 0u;
		signature.virtualGeometryHash = HashVirtualGeometryRange(record.virtualGeometry);
		return signature;
	}

	std::uint64_t HashPreparedDrawPacketSignature(const PreparedDrawPacketSignature& signature)
	{
		std::uint64_t hash = HashUuid(signature.objectId);
		HashCombine(hash, HashUuid(signature.meshId));
		HashCombine(hash, HashUuid(signature.materialId));
		HashCombine(hash, signature.transformHash);
		HashCombine(hash, signature.visibilityHash);
		HashCombine(hash, signature.overrideHash);
		HashCombine(hash, signature.virtualGeometryHash);
		return hash;
	}

	namespace {
		bool DependsOn(const RenderPassNode& pass, const std::string& dependency)
		{
			return std::find(pass.dependencies.begin(), pass.dependencies.end(), dependency) != pass.dependencies.end();
		}

		void AddDiagnostic(RenderGraphValidationResult& result, std::string code, std::string passId, std::string message, std::string action)
		{
			result.valid = false;
			RenderGraphValidationDiagnostic diagnostic;
			diagnostic.severity = "error";
			diagnostic.code = std::move(code);
			diagnostic.passId = std::move(passId);
			diagnostic.message = std::move(message);
			diagnostic.recommendedAction = std::move(action);
			result.diagnostics.push_back(std::move(diagnostic));
		}
	}

	RenderGraphValidationResult ValidateRenderGraph(const std::vector<RenderPassNode>& passes)
	{
		RenderGraphValidationResult result;
		std::set<std::string> passIds;
		for (const RenderPassNode& pass : passes)
		{
			if (pass.passId.empty())
			{
				AddDiagnostic(result, "missing-pass-id", pass.passId, "Render pass has no stable id.", "Assign a pass id before scheduling.");
				continue;
			}
			if (!passIds.insert(pass.passId).second)
			{
				AddDiagnostic(result, "duplicate-pass-id", pass.passId, "Render pass id is duplicated.", "Use deterministic unique ids for every pass.");
			}
			if (pass.workPolicy != RenderWorkPolicy::WorkerEligible && pass.fallbackReason.empty())
			{
				AddDiagnostic(result, "missing-serial-reason", pass.passId, "Serial or fallback pass has no reason.", "Explain the ownership or capability boundary.");
			}
			if (pass.ownership == RenderWorkerOwnership::CoordinatorOnly && pass.workPolicy == RenderWorkPolicy::WorkerEligible)
			{
				AddDiagnostic(result, "worker-owns-coordinator-pass", pass.passId, "Coordinator-owned pass is marked worker eligible.", "Serialize coordinator-only work.");
			}
			if (!pass.canOverlap && pass.workPolicy == RenderWorkPolicy::WorkerEligible && pass.dependencies.empty())
			{
				AddDiagnostic(result, "missing-non-overlap-dependency", pass.passId, "Non-overlapping worker pass has no ordering dependency.", "Add a dependency or mark the pass serial.");
			}
			for (const RenderResourceState& output : pass.outputs)
			{
				if (IsCoordinatorOwnedAccess(output.accessMode) && pass.ownership != RenderWorkerOwnership::CoordinatorOnly)
				{
					AddDiagnostic(result, "present-not-coordinator-owned", pass.passId, "Presentation output is not coordinator-owned.", "Keep presentation and final submit on the coordinator path.");
				}
			}
		}

		for (std::size_t leftIndex = 0u; leftIndex < passes.size(); ++leftIndex)
		{
			for (std::size_t rightIndex = leftIndex + 1u; rightIndex < passes.size(); ++rightIndex)
			{
				const RenderPassNode& left = passes[leftIndex];
				const RenderPassNode& right = passes[rightIndex];
				for (const RenderResourceState& leftOutput : left.outputs)
				{
					for (const RenderResourceState& rightInput : right.inputs)
					{
						if (leftOutput.resourceId == 0u || leftOutput.resourceId != rightInput.resourceId)
						{
							continue;
						}
						if (IsWriteAccess(leftOutput.accessMode) && !DependsOn(right, left.passId))
						{
							AddDiagnostic(result, "unordered-read-write", right.passId, "Pass reads a resource written by an earlier pass without a dependency.", "Add an explicit pass dependency or split the resource.");
						}
					}
					for (const RenderResourceState& rightOutput : right.outputs)
					{
						if (leftOutput.resourceId == 0u || leftOutput.resourceId != rightOutput.resourceId)
						{
							continue;
						}
						if ((IsWriteAccess(leftOutput.accessMode) || IsWriteAccess(rightOutput.accessMode)) && !DependsOn(right, left.passId))
						{
							AddDiagnostic(result, "unordered-write-conflict", right.passId, "Pass writes a resource that conflicts with another writer.", "Serialize the passes with graph dependencies.");
						}
					}
				}
			}
		}

		return result;
	}

	std::uint64_t BuildPostProcessGraphTopologySignature(
		const PostProcessGraphSettings& settings) noexcept
	{
		// Zero also represents an unavailable plan. Requested effects must never
		// create resources before their complete backend path is qualified.
		if(!settings.executionAvailable || !(settings.screenSpaceLighting
			|| settings.temporalResolve || settings.hdrCamera || settings.toneAndGrade
			|| settings.spatialAA || settings.presentationEffects || settings.coverageAA || settings.customStages || settings.outputDithering)) return 0u;
		std::uint64_t signature = 1469598103934665603ull;
		for(const bool enabled : {settings.screenSpaceLighting, settings.temporalResolve,
			settings.hdrCamera, settings.toneAndGrade, settings.spatialAA,
			settings.presentationEffects, settings.coverageAA, settings.motionBlur, settings.outputDithering}) HashCombine(signature, enabled ? 1u : 0u);
		HashCombine(signature, settings.antiAliasingMode);
		HashCombine(signature, settings.customStages);
		HashCombine(signature, settings.sampleCount);
		HashCombine(signature, settings.internalSize.x);
		HashCombine(signature, settings.internalSize.y);
		HashCombine(signature, settings.outputSize.x);
		HashCombine(signature, settings.outputSize.y);
		HashCombine(signature, settings.resourceFormatGeneration);
		return signature == 0u ? 1u : signature;
	}

	PostProcessGraphSettings BuildPostProcessGraphSettings(
		const GraphicsSettings::GraphicsSettingsSnapshot& snapshot) noexcept
	{
		PostProcessGraphSettings settings;
		settings.customStages = snapshot.customPostProcessStages;
		settings.internalSize = {snapshot.internalWidth, snapshot.internalHeight};
		settings.outputSize = {snapshot.outputWidth, snapshot.outputHeight};
		settings.sampleCount = snapshot.sampleCount;
		settings.resourceFormatGeneration = snapshot.lifecycle.backendResourceGeneration;
		for(const auto& entry : snapshot.values)
		{
			if(entry.id == GraphicsSettings::GraphicsSettingId::AntiAliasingMode)
			{
				if(const auto mode = std::get_if<std::uint32_t>(&entry.value))
					settings.antiAliasingMode = *mode;
			}
			if(entry.id == GraphicsSettings::GraphicsSettingId::FixedExposureCompensation)
			{
				if(const auto exposure = std::get_if<float>(&entry.value))
					settings.fixedExposureCompensation = *exposure;
			}
			if(entry.id == GraphicsSettings::GraphicsSettingId::InternalResolutionScale)
			{
				if(const auto scale = std::get_if<float>(&entry.value))
					settings.internalResolutionScale = *scale;
			}
		}
		const auto color = GraphicsSettings::ResolvePostProcessColorSettings(snapshot.values);
		settings.fixedExposureCompensation = color.compensationEV;
		settings.hdrCamera = color.HasExposure() || color.HasBloom();
		settings.toneAndGrade = color.HasToneAndGrading();
		GraphicsSettings::PostProcessEffectSettings effects;
		if(GraphicsSettings::ResolvePostProcessEffectSettings(snapshot.values, effects) == GraphicsSettings::GraphicsRequestReason::None)
		{
			settings.hdrCamera = settings.hdrCamera || effects.HasHdrCameraEffects();
			settings.presentationEffects = effects.presentation.Active();
			settings.outputDithering = effects.outputDithering;
			settings.motionBlur = effects.motionBlur.Active();
			settings.screenSpaceLighting = effects.HasScreenSpace();
		}
		settings.spatialAA = settings.antiAliasingMode >= 1u
			&& settings.antiAliasingMode <= 3u;
		GraphicsSettings::GraphicsAAConfiguration raster;
		const bool validAA = GraphicsSettings::ResolveGraphicsAAConfiguration(snapshot.values, raster) == GraphicsSettings::GraphicsRequestReason::None;
		settings.coverageAA = validAA
			&& raster.raster == GraphicsSettings::GraphicsGeometryRasterMode::Indexed
			&& raster.antiAliasing == GraphicsSettings::GraphicsAntiAliasingMode::MSAA
			&& settings.sampleCount == raster.sampleCount;
		settings.temporalResolve = validAA
			&& raster.raster == GraphicsSettings::GraphicsGeometryRasterMode::Indexed
			&& (raster.antiAliasing == GraphicsSettings::GraphicsAntiAliasingMode::TAA
				|| raster.upscaling == GraphicsSettings::GraphicsUpscalingMode::Temporal)
			&& settings.sampleCount == 1u;
		// Coverage resolves into the same native-extent image consumed by the
		// color and overlay stages. The backend certifies the exact attachments.
		settings.executionAvailable = snapshot.postProcessResourceGeneration != 0u
			&& snapshot.postProcessResourceGeneration == settings.resourceFormatGeneration
			&& (settings.coverageAA || settings.temporalResolve || ((settings.screenSpaceLighting || settings.hdrCamera || color.Active() || settings.spatialAA || settings.presentationEffects || settings.customStages || settings.outputDithering) && settings.sampleCount == 1u
				&& (settings.antiAliasingMode == 0u || settings.spatialAA)))
			&& (settings.internalResolutionScale == 1.0f || settings.temporalResolve);
		return settings;
	}

	void BindGraphicsCameraViewProvenance(RenderFrameViewRequest& view, const RenderFrameSnapshot& snapshot,
		std::optional<GraphicsSettings::GraphicsCameraTarget> camera)
	{
		view.graphicsCamera.reset(); view.graphicsView.reset();
		if(!camera || camera->cameraId == TerraEngine::EMPTY_UUID || camera->sceneGeneration == 0
			|| camera->objectLifetimeGeneration == 0 || camera->componentGeneration == 0 || camera->bindingGeneration == 0
			|| std::find(snapshot.graphicsCameraIdentities.begin(), snapshot.graphicsCameraIdentities.end(), *camera) == snapshot.graphicsCameraIdentities.end()) return;
		view.graphicsCamera = camera;
		for(const auto& prior : snapshot.publishedGraphicsCameraViews)
		{
			if(prior.target.scope != GraphicsSettings::GraphicsScope::Camera || prior.target.camera != camera || !prior.target.view
				|| prior.target.view->lifetimeGeneration == 0 || prior.target.view->outputKind != view.outputKind
				|| prior.target.view->viewType != view.viewType || prior.target.view->outputId != view.outputId
				|| prior.width == 0 || prior.height == 0) continue;
			// Standalone extent0 requests the current native swapchain, rather than
			// inventing a viewport size. Its protected registry already checks the
			// backend owner; every other view must match explicit dimensions.
			const bool nativeRuntimeExtent = view.outputKind == RenderOutputKind::MainRuntimeView
				&& view.targetSize.x == 0 && view.targetSize.y == 0;
			if(!nativeRuntimeExtent && (prior.width != view.targetSize.x || prior.height != view.targetSize.y)) continue;
			// Duplicate exact registrations cannot prove a unique lifetime.
			if(view.graphicsView) { view.graphicsView.reset(); return; }
			view.graphicsView = prior.target.view;
		}
		if(view.graphicsView || view.targetSize.x == 0 || view.targetSize.y == 0
			|| (view.outputKind != RenderOutputKind::OffscreenTexture && view.outputKind != RenderOutputKind::GameView)) return;
		// Protected collection plans private lifetimes for dedicated captures and
		// Game history before their first accepted draw. Zero is never C# visible.
		for(const auto& planned : snapshot.cameraGraphicsSettings)
		{
			if(!planned || planned->publishedFrameId != 0 || planned->target.scope != GraphicsSettings::GraphicsScope::Camera
				|| planned->target.camera != camera || !planned->target.view || planned->target.view->lifetimeGeneration == 0
				|| planned->target.view->outputKind != view.outputKind || planned->target.view->viewType != view.viewType
				|| planned->target.view->outputId != view.outputId || planned->lifecycle != snapshot.graphicsSettingsLifecycle
				|| planned->outputWidth != view.targetSize.x || planned->outputHeight != view.targetSize.y) continue;
			// A planned output can have a smaller color extent. Validate that
			// extent using the same typed configuration as the protected planner.
			GraphicsSettings::GraphicsAAConfiguration aa;
			if(GraphicsSettings::ResolveGraphicsAAConfiguration(planned->values, aa) != GraphicsSettings::GraphicsRequestReason::None
				|| planned->internalWidth != GraphicsSettings::ResolveGraphicsAAInternalDimension(view.targetSize.x, aa)
				|| planned->internalHeight != GraphicsSettings::ResolveGraphicsAAInternalDimension(view.targetSize.y, aa)) continue;
			if(view.graphicsView) { view.graphicsView.reset(); return; }
			view.graphicsView=planned->target.view;
		}
	}

	std::vector<PublishedGraphicsCameraView> CollectPublishedGraphicsCameraTargets(const RenderFramePublication& publication)
	{
		std::vector<PublishedGraphicsCameraView> targets;
		for(std::size_t publicationIndex=0; publicationIndex<publication.presentationInputs.size(); ++publicationIndex)
		{
			const auto& presentation=publication.presentationInputs[publicationIndex];
			const auto& camera = presentation.graphicsCamera;
			const auto& output = presentation.output;
			// Only the standalone swapchain request is allowed to defer extent
			// resolution to Vulkan. A missing editor/capture extent has no view.
			const bool nativeRuntimeExtent = output.kind == RenderOutputKind::MainRuntimeView
				&& output.targetSize.x == 0 && output.targetSize.y == 0;
			if(!nativeRuntimeExtent && (output.targetSize.x == 0 || output.targetSize.y == 0)) continue;
			if(!camera || camera->cameraId == TerraEngine::EMPTY_UUID || camera->sceneGeneration == 0
				|| camera->objectLifetimeGeneration == 0 || camera->componentGeneration == 0 || camera->bindingGeneration == 0) continue;
			PublishedGraphicsCameraView evidence;
			evidence.target.scope = GraphicsSettings::GraphicsScope::Camera;
			evidence.target.camera = camera;
			evidence.target.view = GraphicsSettings::GraphicsViewIdentity{output.diagnosticsView, output.outputId, 0, output.kind};
			if(presentation.graphicsView && presentation.graphicsView->outputKind == output.kind
				&& presentation.graphicsView->viewType == output.diagnosticsView && presentation.graphicsView->outputId == output.outputId)
				evidence.target.view = presentation.graphicsView;
			evidence.width = output.targetSize.x; evidence.height = output.targetSize.y;
			evidence.publicationIndex=static_cast<std::uint32_t>(publicationIndex);
			targets.push_back(std::move(evidence));
		}
		return targets;
	}

	std::shared_ptr<const GraphicsSettings::GraphicsSettingsSnapshot>
	SelectGraphicsSettingsForView(const RenderFrameSnapshot& snapshot, const RenderFrameViewRequest& view) noexcept
	{
		if(view.outputKind == RenderOutputKind::SceneView && snapshot.sceneGraphicsSettings)
			return snapshot.sceneGraphicsSettings;
		if(!view.graphicsCamera || !view.graphicsView || view.graphicsView->lifetimeGeneration == 0
			|| view.graphicsView->outputKind != view.outputKind || view.graphicsView->viewType != view.viewType
			|| view.graphicsView->outputId != view.outputId) return snapshot.graphicsSettings;
		// Verify the association again at selection so a caller cannot accidentally
		// retain a previously bound request after snapshot membership changes.
		RenderFrameViewRequest verified=view;
		BindGraphicsCameraViewProvenance(verified,snapshot,view.graphicsCamera);
		if(verified.graphicsView != view.graphicsView) return snapshot.graphicsSettings;
		for(const auto& authored : snapshot.cameraPostProcessAuthoring)
			if(authored && authored->target.camera == view.graphicsCamera && authored->target.view == view.graphicsView
				&& authored->lifecycle == snapshot.graphicsSettingsLifecycle && authored->settings) return authored->settings;
		for(const auto& candidate : snapshot.cameraGraphicsSettings)
		{
			// Camera UUID and all object/component/binding/view lifetimes must match.
			// A matching physical output number alone never authorizes an override.
			if(candidate && candidate->target.scope == GraphicsSettings::GraphicsScope::Camera
				&& candidate->target.camera == view.graphicsCamera && candidate->target.view == view.graphicsView
				&& (!snapshot.graphicsSettings || candidate->lifecycle == snapshot.graphicsSettings->lifecycle)) return candidate;
		}
		return snapshot.graphicsSettings;
	}

	void AppendPostProcessRenderPasses(std::vector<RenderPassNode>& passes,
		const RenderFrameViewRequest& view, const PostProcessGraphSettings& settings)
	{
		// Preserve both the legacy declarations and their storage on bypass.
		// This check precedes every string, vector, or graph-resource operation.
		if(BuildPostProcessGraphTopologySignature(settings) == 0u) return;
		const std::uint64_t viewIdentity = GraphViewIdentity(view);
		const std::string prefix = "view-" + std::to_string(viewIdentity);
		const std::string drawId = prefix + ".draw";
		auto draw = std::find_if(passes.begin(), passes.end(), [&drawId](const auto& pass) {
			return pass.passId == drawId;
		});
		if(draw == passes.end()) return;
		// A disjoint resource domain preserves every legacy resource identity and
		// leaves room for per-effect intermediates without four-bit lane aliases.
		const std::uint64_t resourceBase = (std::uint64_t{1} << 63u) | (viewIdentity << 8u);
		const std::uint64_t depthResource = (viewIdentity << 4u) | 3u;
		const std::uint64_t sceneColor = resourceBase | 1u;
		draw->outputs.push_back({sceneColor, RenderAccessMode::Write,
			RenderStage::Geometry, drawId, RenderResourceSemantic::SceneColor});
		std::string previousPass = drawId;
		std::uint64_t previousColor = sceneColor;
		std::uint64_t nextResource = 2u;
		struct Stage { PostProcessPassKind kind; const char* name; bool enabled; bool depth; };
		const std::array stages{
			Stage{PostProcessPassKind::ScreenSpaceLighting, "screen-space-lighting", settings.screenSpaceLighting, true},
			Stage{PostProcessPassKind::TemporalResolve, "temporal-resolve", settings.temporalResolve, true},
			Stage{PostProcessPassKind::CustomFullscreen, "custom-hdr", (settings.customStages & 1u) != 0u, false},
			Stage{PostProcessPassKind::HdrCamera, "hdr-camera", settings.hdrCamera, true},
			Stage{PostProcessPassKind::ToneAndGrade, "tone-and-grade", settings.toneAndGrade, false},
			Stage{PostProcessPassKind::CustomFullscreen, "custom-display", (settings.customStages & 2u) != 0u, false},
			Stage{PostProcessPassKind::SpatialAA, "spatial-aa", settings.spatialAA, false},
			Stage{PostProcessPassKind::PresentationEffects, "presentation-effects", settings.presentationEffects, false},
			Stage{PostProcessPassKind::CustomFullscreen, "custom-before-ui", (settings.customStages & 4u) != 0u, false},
			Stage{PostProcessPassKind::OverlayComposite, "overlay-composite", true, true}
		};
		for(const Stage& stage : stages)
		{
			if(!stage.enabled) continue;
			RenderPassNode pass;
			pass.passId = prefix + ".post." + stage.name;
			pass.viewType = view.viewType;
			pass.postProcessKind = stage.kind;
			pass.workPolicy = RenderWorkPolicy::SerialRequired;
			pass.ownership = RenderWorkerOwnership::CoordinatorOnly;
			pass.canOverlap = false;
			pass.fallbackReason = "Camera image stages share ordered view targets under the protected backend commit.";
			pass.profilingLabel = "RenderGraph: " + pass.passId;
			pass.dependencies.push_back(previousPass);
			const RenderStage pipelineStage = stage.kind == PostProcessPassKind::OverlayComposite
				? RenderStage::UI : RenderStage::Lighting;
			pass.inputs.push_back({previousColor, RenderAccessMode::Read, pipelineStage,
				pass.passId, previousColor == sceneColor ? RenderResourceSemantic::SceneColor
					: RenderResourceSemantic::ProcessedColor});
			if(stage.depth)
			{
				if(previousPass != drawId) pass.dependencies.push_back(drawId);
				pass.inputs.push_back({depthResource, RenderAccessMode::Read, pipelineStage,
					pass.passId, RenderResourceSemantic::SceneDepth});
			}
			previousColor = resourceBase | nextResource++;
			pass.outputs.push_back({previousColor, RenderAccessMode::Write, pipelineStage,
				pass.passId, stage.kind == PostProcessPassKind::OverlayComposite
					? RenderResourceSemantic::CompositedColor : RenderResourceSemantic::ProcessedColor});
			previousPass = pass.passId;
			passes.push_back(std::move(pass));
		}
	}

	std::vector<RenderPassNode> BuildDefaultRenderPasses(const RenderFrameSnapshot& snapshot, const BackendCapabilityProfile& capabilities)
	{
		std::vector<RenderPassNode> passes;
		const bool workerAllowed = capabilities.supportsParallelRecording && capabilities.maxRecordingWorkers > 1u;
		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			// The snapshot identity is (output kind, view type, full uint32 ID).
			// Fixed disjoint bit lanes avoid hashing collisions and uint32 addition
			// overflow. Four low bits identify resources; zero remains reserved.
			const std::uint64_t viewIdentity = GraphViewIdentity(view);
			const std::string viewPrefix = "view-" + std::to_string(viewIdentity);
			const std::uint64_t resourceBase = viewIdentity << 4u;
			const std::uint64_t collectedGeometryResource = resourceBase | 1u;
			const std::uint64_t virtualGeometryMainResource = resourceBase | 2u;
			const std::uint64_t depthResource = resourceBase | 3u;
			const std::uint64_t virtualGeometryPostResource = resourceBase | 4u;

			RenderPassNode collectPass;
			collectPass.passId = viewPrefix + ".collect";
			collectPass.viewType = view.viewType;
			collectPass.workPolicy = workerAllowed ? RenderWorkPolicy::WorkerEligible : RenderWorkPolicy::SerialRequired;
			collectPass.fallbackReason = workerAllowed ? "" : "Backend capability profile disables parallel recording.";
			collectPass.profilingLabel = "RenderGraph: Collect View";
			collectPass.outputs.push_back({ collectedGeometryResource, RenderAccessMode::Write, RenderStage::Cull, collectPass.passId });
			passes.push_back(std::move(collectPass));

			RenderPassNode virtualGeometryMainPass;
			virtualGeometryMainPass.passId = viewPrefix + ".virtual-geometry-main-occlusion";
			virtualGeometryMainPass.viewType = view.viewType;
			virtualGeometryMainPass.dependencies.push_back(passes.back().passId);
			virtualGeometryMainPass.workPolicy = workerAllowed ? RenderWorkPolicy::WorkerEligible : RenderWorkPolicy::SerialRequired;
			virtualGeometryMainPass.fallbackReason = workerAllowed ? "" : "Backend capability profile disables parallel recording.";
			virtualGeometryMainPass.profilingLabel = "RenderGraph: Virtual Geometry Main Occlusion";
			// Main occlusion is deliberately before draw. It uses only current-frame
			// conservative data, so a rejected or invalidated history cannot hide a
			// packet before the visible fallback draw has been prepared.
			virtualGeometryMainPass.inputs.push_back({ collectedGeometryResource, RenderAccessMode::Read, RenderStage::Cull, virtualGeometryMainPass.passId });
			virtualGeometryMainPass.outputs.push_back({ virtualGeometryMainResource, RenderAccessMode::Write, RenderStage::Cull, virtualGeometryMainPass.passId });
			passes.push_back(std::move(virtualGeometryMainPass));

			RenderPassNode drawPass;
			drawPass.passId = viewPrefix + ".draw";
			drawPass.viewType = view.viewType;
			drawPass.dependencies.push_back(passes.back().passId);
			// The draw reads collect's resource directly. Declare that proof too;
			// collect already precedes main, so this adds no execution boundary.
			drawPass.dependencies.push_back(viewPrefix + ".collect");
			drawPass.workPolicy = workerAllowed ? RenderWorkPolicy::WorkerEligible : RenderWorkPolicy::SerialRequired;
			drawPass.fallbackReason = workerAllowed ? "" : "Backend capability profile disables parallel recording.";
			drawPass.profilingLabel = "RenderGraph: Draw View";
			drawPass.inputs.push_back({ collectedGeometryResource, RenderAccessMode::Read, RenderStage::Geometry, drawPass.passId });
			drawPass.inputs.push_back({ virtualGeometryMainResource, RenderAccessMode::Read, RenderStage::Geometry, drawPass.passId });
			drawPass.outputs.push_back({ depthResource, RenderAccessMode::Write, RenderStage::Geometry, drawPass.passId });
			passes.push_back(std::move(drawPass));

			RenderPassNode virtualGeometryPostPass;
			virtualGeometryPostPass.passId = viewPrefix + ".virtual-geometry-post-occlusion";
			virtualGeometryPostPass.viewType = view.viewType;
			virtualGeometryPostPass.dependencies.push_back(passes.back().passId);
			virtualGeometryPostPass.workPolicy = workerAllowed ? RenderWorkPolicy::WorkerEligible : RenderWorkPolicy::SerialRequired;
			virtualGeometryPostPass.fallbackReason = workerAllowed ? "" : "Backend capability profile disables parallel recording.";
			virtualGeometryPostPass.profilingLabel = "RenderGraph: Virtual Geometry Post Occlusion";
			// Post occlusion consumes the depth written by this frame's draw pass.
			// VulkanHiZ may still reject its history, but the graph order preserves a
			// declared boundary for publishing the fresh pyramid and invalidation.
			virtualGeometryPostPass.inputs.push_back({ depthResource, RenderAccessMode::Read, RenderStage::Cull, virtualGeometryPostPass.passId });
			virtualGeometryPostPass.outputs.push_back({ virtualGeometryPostResource, RenderAccessMode::Write, RenderStage::Cull, virtualGeometryPostPass.passId });
			passes.push_back(std::move(virtualGeometryPostPass));
		}
		return passes;
	}
}
