// Exact typed projections of sealed presentation/backend values.
// Every overload reads named public members of actual production records.
// See Fixtures/frame-replay-projection-coverage.md for scope and exclusions.
#pragma once

#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/ShadowWorkPlan.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <concepts>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace TerraEngine::TestSupport::FrameReplay
{
	using ProjectionJson = nlohmann::json;

	inline ProjectionJson PublishedValue(const ::FrameLightCandidate& value);
	inline ProjectionJson PublishedValue(const ::FrameSkyData& value);
	inline ProjectionJson PublishedValue(const ::GPULight& value);
	inline ProjectionJson PublishedValue(const ::GeneratedVirtualGeometrySubmissionRecord& value);
	inline ProjectionJson PublishedValue(const ::GpuPacketCompactionDispatch& value);
	inline ProjectionJson PublishedValue(const ::GpuVirtualGeometryBatchDescriptor& value);
	inline ProjectionJson PublishedValue(const ::GpuVirtualGeometryPacketFrameSet& value);
	inline ProjectionJson PublishedValue(const ::GpuVirtualGeometryPacketRecord& value);
	inline ProjectionJson PublishedValue(const ::GroupedRenderSubmission& value);
	inline ProjectionJson PublishedValue(const ::RenderInstanceRecord& value);
	inline ProjectionJson PublishedValue(const ::RenderItemRecord& value);
	inline ProjectionJson PublishedValue(const ::RenderItemRecord::PublishedAnimationGeneration& value);
	inline ProjectionJson PublishedValue(const ::RenderOptimizationSummary& value);
	inline ProjectionJson PublishedValue(const ::RenderOutputRequest& value);
	inline ProjectionJson PublishedValue(const ::RenderSettings& value);
	inline ProjectionJson PublishedValue(const ::RenderViewCommandLane& value);
	inline ProjectionJson PublishedValue(const ::ShadowDrawList& value);
	inline ProjectionJson PublishedValue(const ::ShadowPreparedInstanceInputs& value);
	inline ProjectionJson PublishedValue(const ::SharedRenderGroup& value);
	inline ProjectionJson PublishedValue(const ::SharedRenderGroupKey& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationBatch& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationBatchGenerationKey& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationBatchKey& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationDispatchCommand& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationDispatchRecord& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationGenerationRecord& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPaletteRecord& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPlan& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPlanDiagnostics& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPlanDynamicLanes& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPlanLimits& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPushConstants& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationRenderIdentity& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationResidentPayloads& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationStageSpan& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationWorkPacket& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::AnimationBackendWorkPlan& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::AnimationBackendWorkRange& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::CpuAnimationDeformationData& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::CpuAnimationDeformationPlan& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::CpuAnimationDeformationRecord& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::DeformationConsumerWorkRecord& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::DeformationGeneration& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::DeformationWorkRecord& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::DeformedPageKey& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::GpuDeformationPlan& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::LightingDepthReduction::DepthRecipe& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::LightingDepthReduction::Extent& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::LightingDepthReduction::PixelOverride& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::PublishedAnimationRenderBinding& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RenderBackendWorkPublication& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RenderReferenceFrame& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RenderViewPresentationInput& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureCamera& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureCasterSelection& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureCorrelation& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureDepthControls& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureIntent& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureLightingControls& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureShadowBatchControls& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureShadowSelection& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureStaticShadowControls& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureViewOrderEntry& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureViewOrigin& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyOptions& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::ShadowCanonicalOrigin& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::ShadowCasterStamp& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::ShadowStaticSupport& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::ShadowTexturePublicationStamp& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::StaticPageKey& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Scene::WorldCell& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Scene::WorldDelta& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Scene::WorldPose& value);
	inline ProjectionJson PublishedValue(const ::TerraEngine::Scene::WorldPosition& value);
	inline ProjectionJson PublishedValue(const ::UIImageRun& value);
	inline ProjectionJson PublishedValue(const ::UIImmediateQuad& value);
	inline ProjectionJson PublishedValue(const ::UIRenderBatch& value);
	inline ProjectionJson PublishedValue(const ::UIStaticQuad& value);
	inline ProjectionJson PublishedValue(const ::UITextRun& value);
	inline ProjectionJson PublishedValue(const ::VirtualGeometryCounterReadbackRecord& value);
	inline ProjectionJson PublishedValue(const ::VirtualGeometryDecodedPageUploadRecord& value);
	inline ProjectionJson PublishedValue(const ::VirtualGeometryGpuFallbackRecord& value);
	inline ProjectionJson PublishedValue(const ::VirtualGeometryPageRetirementRecord& value);
	inline ProjectionJson PublishedValue(const ::VirtualGeometryQualityProfile& value);
	inline ProjectionJson PublishedValue(const ::VirtualGeometryRetainedGpuEvidence& value);

	template<class T> requires (std::is_integral_v<T> || std::is_enum_v<T>)
	ProjectionJson PublishedValue(T value)
	{
		if constexpr(std::is_enum_v<T>) return static_cast<std::underlying_type_t<T>>(value);
		else return value;
	}
	inline ProjectionJson PublishedValue(float value) { return {{"ieee754", std::bit_cast<std::uint32_t>(value)}}; }
	inline ProjectionJson PublishedValue(double value) { return {{"ieee754", std::bit_cast<std::uint64_t>(value)}}; }
	inline ProjectionJson PublishedValue(const std::string& value) { return value; }
	inline ProjectionJson PublishedValue(const TerraEngine::UUID& value) { return TerraEngine::ToHexString(value); }

	template<glm::length_t L, class T, glm::qualifier Q>
	ProjectionJson PublishedValue(const glm::vec<L, T, Q>& value)
	{
		ProjectionJson result = ProjectionJson::array();
		for(glm::length_t index = 0; index < L; ++index) result.push_back(PublishedValue(value[index]));
		return result;
	}
	template<glm::length_t C, glm::length_t R, class T, glm::qualifier Q>
	ProjectionJson PublishedValue(const glm::mat<C, R, T, Q>& value)
	{
		ProjectionJson result = ProjectionJson::array();
		for(glm::length_t column = 0; column < C; ++column) result.push_back(PublishedValue(value[column]));
		return result;
	}
	template<class T, glm::qualifier Q>
	ProjectionJson PublishedValue(const glm::qua<T, Q>& value)
	{
		return ProjectionJson::array({PublishedValue(value.x), PublishedValue(value.y), PublishedValue(value.z), PublishedValue(value.w)});
	}

	template<class T> ProjectionJson PublishedValue(const std::shared_ptr<T>& value);
	template<class T> ProjectionJson PublishedValue(const std::optional<T>& value);
	template<std::ranges::input_range T> requires (!std::is_same_v<T, std::string>)
	ProjectionJson PublishedValue(const T& values)
	{
		ProjectionJson result = ProjectionJson::array();
		for(const auto& value : values) result.push_back(PublishedValue(value));
		return result;
	}

	template<class T> ProjectionJson PublishedValue(const std::shared_ptr<T>& value)
	{
		if(!value) return nullptr;
		// Addresses are used only to reject a corrupt ownership cycle; they
		// are never emitted, hashed, or compared as publication values.
		static thread_local std::vector<const void*> active;
		const void* address = value.get();
		if(active.size() >= 128 || std::find(active.begin(), active.end(), address) != active.end())
			throw std::runtime_error("Cyclic or over-deep sealed publication ownership.");
		active.push_back(address);
		struct Pop { std::vector<const void*>& values; ~Pop() { values.pop_back(); } } pop{active};
		return PublishedValue(*value);
	}
	template<class T> ProjectionJson PublishedValue(const std::optional<T>& value)
	{
		return value ? PublishedValue(*value) : ProjectionJson(nullptr);
	}

	inline ProjectionJson PublishedValue(const ::FrameLightCandidate& value)
	{
		return {
			{"lightId", PublishedValue(value.lightId)},
			{"canonicalPosition", PublishedValue(value.canonicalPosition)},
			{"gpuLight", PublishedValue(value.gpuLight)},
			{"stabilityPreference", PublishedValue(value.stabilityPreference)},
			{"candidateFlags", PublishedValue(value.candidateFlags)}
		};
	}

	inline ProjectionJson PublishedValue(const ::FrameSkyData& value)
	{
		return {
			{"renderBackground", PublishedValue(value.renderBackground)},
			{"hasTexture", PublishedValue(value.hasTexture)},
			{"usingFallback", PublishedValue(value.usingFallback)},
			{"forceBlack", PublishedValue(value.forceBlack)},
			{"intensity", PublishedValue(value.intensity)},
			{"exposure", PublishedValue(value.exposure)},
			{"rotationOffsetDegrees", PublishedValue(value.rotationOffsetDegrees)},
			{"horizonTint", PublishedValue(value.horizonTint)},
			{"zenithTint", PublishedValue(value.zenithTint)},
			{"groundTint", PublishedValue(value.groundTint)},
			{"sunDirection", PublishedValue(value.sunDirection)},
			{"textureId", PublishedValue(value.textureId)}
		};
	}

	inline ProjectionJson PublishedValue(const ::GPULight& value)
	{
		return {
			{"positionAndType", PublishedValue(value.positionAndType)},
			{"directionAndRange", PublishedValue(value.directionAndRange)},
			{"colorAndIntensity", PublishedValue(value.colorAndIntensity)},
			{"coneAnglesAndArea", PublishedValue(value.coneAnglesAndArea)},
			{"shadowAndPadding", PublishedValue(value.shadowAndPadding)},
			{"shadowRegion", PublishedValue(value.shadowRegion)},
			{"extraParams", PublishedValue(value.extraParams)},
			{"shadowMatrixC0", PublishedValue(value.shadowMatrixC0)},
			{"shadowMatrixC1", PublishedValue(value.shadowMatrixC1)},
			{"shadowMatrixC2", PublishedValue(value.shadowMatrixC2)},
			{"shadowMatrixC3", PublishedValue(value.shadowMatrixC3)}
		};
	}

	inline ProjectionJson PublishedValue(const ::GeneratedVirtualGeometrySubmissionRecord& value)
	{
		return {
			{"recordId", PublishedValue(value.recordId)},
			{"viewId", PublishedValue(value.viewId)},
			{"viewType", PublishedValue(value.viewType)},
			{"frameId", PublishedValue(value.frameId)},
			{"batchId", PublishedValue(value.batchId)},
			{"packetOffset", PublishedValue(value.packetOffset)},
			{"packetCount", PublishedValue(value.packetCount)},
			{"indirectTaskCount", PublishedValue(value.indirectTaskCount)},
			{"materialId", PublishedValue(value.materialId)},
			{"meshId", PublishedValue(value.meshId)},
			{"counterOffset", PublishedValue(value.counterOffset)},
			{"source", PublishedValue(value.source)},
			{"validationState", PublishedValue(value.validationState)},
			{"fallbackReason", PublishedValue(value.fallbackReason)}
		};
	}

	inline ProjectionJson PublishedValue(const ::GpuPacketCompactionDispatch& value)
	{
		return {
			{"dispatchId", PublishedValue(value.dispatchId)},
			{"viewId", PublishedValue(value.viewId)},
			{"viewType", PublishedValue(value.viewType)},
			{"frameId", PublishedValue(value.frameId)},
			{"candidatePacketCount", PublishedValue(value.candidatePacketCount)},
			{"candidateBatchCount", PublishedValue(value.candidateBatchCount)},
			{"outputCapacity", PublishedValue(value.outputCapacity)},
			{"hiZBaselineState", PublishedValue(value.hiZBaselineState)},
			{"debugMode", PublishedValue(value.debugMode)},
			{"failOpenOnInvalid", PublishedValue(value.failOpenOnInvalid)},
			{"fallbackReason", PublishedValue(value.fallbackReason)}
		};
	}

	inline ProjectionJson PublishedValue(const ::GpuVirtualGeometryBatchDescriptor& value)
	{
		return {
			{"batchId", PublishedValue(value.batchId)},
			{"viewId", PublishedValue(value.viewId)},
			{"viewType", PublishedValue(value.viewType)},
			{"frameId", PublishedValue(value.frameId)},
			{"packetOffset", PublishedValue(value.packetOffset)},
			{"packetCount", PublishedValue(value.packetCount)},
			{"meshId", PublishedValue(value.meshId)},
			{"materialId", PublishedValue(value.materialId)},
			{"selectedMeshlets", PublishedValue(value.selectedMeshlets)},
			{"submittedMeshlets", PublishedValue(value.submittedMeshlets)},
			{"debugMode", PublishedValue(value.debugMode)},
			{"fallbackMode", PublishedValue(value.fallbackMode)},
			{"flags", PublishedValue(value.flags)}
		};
	}

	inline ProjectionJson PublishedValue(const ::GpuVirtualGeometryPacketFrameSet& value)
	{
		return {
			{"viewId", PublishedValue(value.viewId)},
			{"viewType", PublishedValue(value.viewType)},
			{"frameId", PublishedValue(value.frameId)},
			{"targetSize", PublishedValue(value.targetSize)},
			{"debugMode", PublishedValue(value.debugMode)},
			{"packetOffset", PublishedValue(value.packetOffset)},
			{"packetCount", PublishedValue(value.packetCount)},
			{"batchOffset", PublishedValue(value.batchOffset)},
			{"batchCount", PublishedValue(value.batchCount)},
			{"compactedPacketOffset", PublishedValue(value.compactedPacketOffset)},
			{"compactedPacketCount", PublishedValue(value.compactedPacketCount)},
			{"generatedSubmissionOffset", PublishedValue(value.generatedSubmissionOffset)},
			{"generatedSubmissionCount", PublishedValue(value.generatedSubmissionCount)},
			{"validity", PublishedValue(value.validity)},
			{"fallbackReason", PublishedValue(value.fallbackReason)}
		};
	}

	inline ProjectionJson PublishedValue(const ::GpuVirtualGeometryPacketRecord& value)
	{
		return {
			{"packetId", PublishedValue(value.packetId)},
			{"viewId", PublishedValue(value.viewId)},
			{"viewType", PublishedValue(value.viewType)},
			{"frameId", PublishedValue(value.frameId)},
			{"objectId", PublishedValue(value.objectId)},
			{"renderableIndex", PublishedValue(value.renderableIndex)},
			{"meshId", PublishedValue(value.meshId)},
			{"materialId", PublishedValue(value.materialId)},
			{"selectedLod", PublishedValue(value.selectedLod)},
			{"clusterId", PublishedValue(value.clusterId)},
			{"meshletOffset", PublishedValue(value.meshletOffset)},
			{"meshletCount", PublishedValue(value.meshletCount)},
			{"boundsMin", PublishedValue(value.boundsMin)},
			{"boundsMax", PublishedValue(value.boundsMax)},
			{"projectedBounds", PublishedValue(value.projectedBounds)},
			{"pageId", PublishedValue(value.pageId)},
			{"fallbackPageId", PublishedValue(value.fallbackPageId)},
			{"payloadSource", PublishedValue(value.payloadSource)},
			{"emittedSource", PublishedValue(value.emittedSource)},
			{"pagePayloadByteOffset", PublishedValue(value.pagePayloadByteOffset)},
			{"pagePayloadByteSize", PublishedValue(value.pagePayloadByteSize)},
			{"payloadBufferIndex", PublishedValue(value.payloadBufferIndex)},
			{"payloadDescriptorIndex", PublishedValue(value.payloadDescriptorIndex)},
			{"fallbackReason", PublishedValue(value.fallbackReason)},
			{"debugIdentity", PublishedValue(value.debugIdentity)},
			{"flags", PublishedValue(value.flags)}
		};
	}

	inline ProjectionJson PublishedValue(const ::GroupedRenderSubmission& value)
	{
		return {
			{"group", PublishedValue(value.group)},
			{"representativeCommandIndex", PublishedValue(value.representativeCommandIndex)}
		};
	}

	inline ProjectionJson PublishedValue(const ::RenderInstanceRecord& value)
	{
		return {
			{"modelMatrix", PublishedValue(value.modelMatrix)},
			{"editorTint", PublishedValue(value.editorTint)},
			{"objectIdentity", PublishedValue(value.objectIdentity)},
			{"materialAndTexture", PublishedValue(value.materialAndTexture)},
			{"animationSelectors", PublishedValue(value.animationSelectors)},
			{"geometrySelectors", PublishedValue(value.geometrySelectors)},
			{"virtualGeometrySelectors", PublishedValue(value.virtualGeometrySelectors)},
			{"virtualGeometryPayloadSelectors", PublishedValue(value.virtualGeometryPayloadSelectors)}
		};
	}

	inline ProjectionJson PublishedValue(const ::RenderItemRecord& value)
	{
		return {
			{"meshId", PublishedValue(value.meshId)},
			{"materialId", PublishedValue(value.materialId)},
			{"canonicalWorldPose", PublishedValue(value.canonicalWorldPose)},
			{"modelMatrix", PublishedValue(value.modelMatrix)},
			{"materialIndex", PublishedValue(value.materialIndex)},
			{"meshDataIndex", PublishedValue(value.meshDataIndex)},
			{"textureIndex", PublishedValue(value.textureIndex)},
			{"objectId", PublishedValue(value.objectId)},
			{"conventionalVisibilityIdentityIndex", PublishedValue(value.conventionalVisibilityIdentityIndex)},
			{"geometryRoute", PublishedValue(value.geometryRoute)},
			{"animation", PublishedValue(value.animation)},
			{"objectLifetimeGeneration", PublishedValue(value.objectLifetimeGeneration)},
			{"shadowCasterInputs", PublishedValue(value.shadowCasterInputs)},
			{"animationBinding", PublishedValue(value.animationBinding)},
			{"editorTint", PublishedValue(value.editorTint)},
			{"virtualMeshletOffset", PublishedValue(value.virtualMeshletOffset)},
			{"virtualMeshletCount", PublishedValue(value.virtualMeshletCount)},
			{"selectedVirtualLod", PublishedValue(value.selectedVirtualLod)},
			{"virtualGeometryFlags", PublishedValue(value.virtualGeometryFlags)},
			{"selectedVirtualClusterOffset", PublishedValue(value.selectedVirtualClusterOffset)},
			{"selectedVirtualClusterCount", PublishedValue(value.selectedVirtualClusterCount)},
			{"selectedVirtualPageNeedOffset", PublishedValue(value.selectedVirtualPageNeedOffset)},
			{"selectedVirtualPageNeedCount", PublishedValue(value.selectedVirtualPageNeedCount)},
			{"virtualGeometryPageId", PublishedValue(value.virtualGeometryPageId)},
			{"virtualGeometryFallbackPageId", PublishedValue(value.virtualGeometryFallbackPageId)},
			{"virtualGeometryPayloadSource", PublishedValue(value.virtualGeometryPayloadSource)},
			{"virtualGeometryEmittedSource", PublishedValue(value.virtualGeometryEmittedSource)},
			{"virtualGeometryPagePayloadByteOffset", PublishedValue(value.virtualGeometryPagePayloadByteOffset)},
			{"virtualGeometryPagePayloadByteSize", PublishedValue(value.virtualGeometryPagePayloadByteSize)},
			{"virtualGeometryPayloadBufferIndex", PublishedValue(value.virtualGeometryPayloadBufferIndex)},
			{"virtualGeometryPayloadDescriptorIndex", PublishedValue(value.virtualGeometryPayloadDescriptorIndex)},
			{"virtualGeometryPacketDescriptorIndex", PublishedValue(value.virtualGeometryPacketDescriptorIndex)},
			{"taskShaderCullingEnabled", PublishedValue(value.taskShaderCullingEnabled)}
		};
	}

	inline ProjectionJson PublishedValue(const ::RenderItemRecord::PublishedAnimationGeneration& value)
	{
		return {
			{"stableInstanceId", PublishedValue(value.stableInstanceId)},
			{"poseGeneration", PublishedValue(value.poseGeneration)},
			{"morphGeneration", PublishedValue(value.morphGeneration)},
			{"deformationContentIdLow", PublishedValue(value.deformationContentIdLow)},
			{"deformationContentIdHigh", PublishedValue(value.deformationContentIdHigh)},
			{"paletteGeneration", PublishedValue(value.paletteGeneration)},
			{"deformablePayloadGeneration", PublishedValue(value.deformablePayloadGeneration)},
			{"programGeneration", PublishedValue(value.programGeneration)},
			{"animationPlanGeneration", PublishedValue(value.animationPlanGeneration)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"firstGpuWorkPacket", PublishedValue(value.firstGpuWorkPacket)},
			{"gpuWorkPacketCount", PublishedValue(value.gpuWorkPacketCount)},
			{"gpuStageMask", PublishedValue(value.gpuStageMask)},
			{"source", PublishedValue(value.source)},
			{"complete", PublishedValue(value.complete)}
		};
	}

	inline ProjectionJson PublishedValue(const ::RenderOptimizationSummary& value)
	{
		// Four producer wall-clock fields are retained separately under the
		// receipt's diagnostic.timings with their exact frame/view identity.
		return {
			{"frameGeneration", PublishedValue(value.frameGeneration)},
			{"topologyGeneration", PublishedValue(value.topologyGeneration)},
			{"viewGeneration", PublishedValue(value.viewGeneration)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"visibleInstanceCount", PublishedValue(value.visibleInstanceCount)},
			{"groupedInstanceCount", PublishedValue(value.groupedInstanceCount)},
			{"fallbackInstanceCount", PublishedValue(value.fallbackInstanceCount)},
			{"groupedSubmissionCount", PublishedValue(value.groupedSubmissionCount)},
			{"fallbackSubmissionCount", PublishedValue(value.fallbackSubmissionCount)},
			{"avoidedSubmissionCount", PublishedValue(value.avoidedSubmissionCount)},
			{"instanceStreamBytes", PublishedValue(value.instanceStreamBytes)},
			{"instanceStreamCapacityBytes", PublishedValue(value.instanceStreamCapacityBytes)},
			{"topologyReuseHits", PublishedValue(value.topologyReuseHits)},
			{"topologyInvalidations", PublishedValue(value.topologyInvalidations)},
			{"viewEvidenceReuseHits", PublishedValue(value.viewEvidenceReuseHits)},
			{"viewEvidenceInvalidations", PublishedValue(value.viewEvidenceInvalidations)},
			{"rejectedIncompleteKeyCount", PublishedValue(value.rejectedIncompleteKeyCount)},
			{"rejectedUnsupportedVariantCount", PublishedValue(value.rejectedUnsupportedVariantCount)},
			{"rejectedOutOfRangeCount", PublishedValue(value.rejectedOutOfRangeCount)},
			{"rejectedStaleGenerationCount", PublishedValue(value.rejectedStaleGenerationCount)},
			{"fallbackAllocationFailureCount", PublishedValue(value.fallbackAllocationFailureCount)},
			{"fallbackRecoveryCount", PublishedValue(value.fallbackRecoveryCount)},
			{"valid", PublishedValue(value.valid)}
		};
	}

	inline ProjectionJson PublishedValue(const ::RenderOutputRequest& value)
	{
		return {
			{"kind", PublishedValue(value.kind)},
			{"priority", PublishedValue(value.priority)},
			{"diagnosticsView", PublishedValue(value.diagnosticsView)},
			{"targetSize", PublishedValue(value.targetSize)},
			{"outputId", PublishedValue(value.outputId)}
		};
	}

	inline ProjectionJson PublishedValue(const ::RenderSettings& value)
	{
		return {
			{"shadowResolution", PublishedValue(value.shadowResolution)},
			{"shadowFilteringSize", PublishedValue(value.shadowFilteringSize)},
			{"maxUploadedLights", PublishedValue(value.maxUploadedLights)},
			{"maxDynamicLights", PublishedValue(value.maxDynamicLights)},
			{"maxShadowLights", PublishedValue(value.maxShadowLights)},
			{"maxAreaLights", PublishedValue(value.maxAreaLights)},
			{"enableLTC", PublishedValue(value.enableLTC)},
			{"enableIBL", PublishedValue(value.enableIBL)},
			{"indirectStrength", PublishedValue(value.indirectStrength)},
			{"ambientOcclusionStrength", PublishedValue(value.ambientOcclusionStrength)},
			{"ambientOcclusionQuality", PublishedValue(value.ambientOcclusionQuality)},
			{"anisotropicFiltering", PublishedValue(value.anisotropicFiltering)},
			{"variableRateShading", PublishedValue(value.variableRateShading)},
			{"validationEnabled", PublishedValue(value.validationEnabled)},
			{"debugEnabled", PublishedValue(value.debugEnabled)},
			{"virtualGeometry", PublishedValue(value.virtualGeometry)}
		};
	}

	inline ProjectionJson PublishedValue(const ::RenderViewCommandLane& value)
	{
		return {
			{"materialIndex", PublishedValue(value.materialIndex)},
			{"meshDataIndex", PublishedValue(value.meshDataIndex)},
			{"textureIndex", PublishedValue(value.textureIndex)},
			{"virtualMeshletOffset", PublishedValue(value.virtualMeshletOffset)},
			{"virtualMeshletCount", PublishedValue(value.virtualMeshletCount)},
			{"virtualGeometryFlags", PublishedValue(value.virtualGeometryFlags)},
			{"selectedVirtualClusterOffset", PublishedValue(value.selectedVirtualClusterOffset)},
			{"selectedVirtualClusterCount", PublishedValue(value.selectedVirtualClusterCount)},
			{"selectedVirtualPageNeedOffset", PublishedValue(value.selectedVirtualPageNeedOffset)},
			{"selectedVirtualPageNeedCount", PublishedValue(value.selectedVirtualPageNeedCount)},
			{"virtualGeometryPageId", PublishedValue(value.virtualGeometryPageId)},
			{"virtualGeometryFallbackPageId", PublishedValue(value.virtualGeometryFallbackPageId)},
			{"virtualGeometryPayloadSource", PublishedValue(value.virtualGeometryPayloadSource)},
			{"virtualGeometryEmittedSource", PublishedValue(value.virtualGeometryEmittedSource)},
			{"virtualGeometryPagePayloadByteOffset", PublishedValue(value.virtualGeometryPagePayloadByteOffset)},
			{"virtualGeometryPagePayloadByteSize", PublishedValue(value.virtualGeometryPagePayloadByteSize)},
			{"virtualGeometryPayloadBufferIndex", PublishedValue(value.virtualGeometryPayloadBufferIndex)},
			{"virtualGeometryPayloadDescriptorIndex", PublishedValue(value.virtualGeometryPayloadDescriptorIndex)},
			{"virtualGeometryPacketDescriptorIndex", PublishedValue(value.virtualGeometryPacketDescriptorIndex)},
			{"taskShaderCullingEnabled", PublishedValue(value.taskShaderCullingEnabled)}
		};
	}

	inline ProjectionJson PublishedValue(const ::ShadowDrawList& value)
	{
		return {
			{"lightId", PublishedValue(value.lightId)},
			{"topologyGeneration", PublishedValue(value.topologyGeneration)},
			{"stableOrdinals", PublishedValue(value.stableOrdinals)},
			{"selectedLods", PublishedValue(value.selectedLods)},
			{"routeSelectors", PublishedValue(value.routeSelectors)},
			{"editorTints", PublishedValue(value.editorTints)},
			{"commandLanes", PublishedValue(value.commandLanes)},
			{"instanceInputs", PublishedValue(value.instanceInputs)}
		};
	}

	inline ProjectionJson PublishedValue(const ::ShadowPreparedInstanceInputs& value)
	{
		return {
			{"frameSerial", PublishedValue(value.frameSerial)},
			{"sessionGeneration", PublishedValue(value.sessionGeneration)},
			{"topologyGeneration", PublishedValue(value.topologyGeneration)},
			{"referenceFrame", PublishedValue(value.referenceFrame)},
			{"viewType", PublishedValue(value.viewType)},
			{"outputId", PublishedValue(value.outputId)},
			{"lightId", PublishedValue(value.lightId)},
			{"records", PublishedValue(value.records)},
			{"complete", PublishedValue(value.complete)}
		};
	}

	inline ProjectionJson PublishedValue(const ::SharedRenderGroup& value)
	{
		return {
			{"key", PublishedValue(value.key)},
			{"firstInstance", PublishedValue(value.firstInstance)},
			{"instanceCount", PublishedValue(value.instanceCount)},
			{"topologyGeneration", PublishedValue(value.topologyGeneration)},
			{"viewGeneration", PublishedValue(value.viewGeneration)},
			{"rejectionReason", PublishedValue(value.rejectionReason)},
			{"executable", PublishedValue(value.executable)}
		};
	}

	inline ProjectionJson PublishedValue(const ::SharedRenderGroupKey& value)
	{
		return {
			{"meshId", PublishedValue(value.meshId)},
			{"materialId", PublishedValue(value.materialId)},
			{"meshGeneration", PublishedValue(value.meshGeneration)},
			{"materialGeneration", PublishedValue(value.materialGeneration)},
			{"pipelineGeneration", PublishedValue(value.pipelineGeneration)},
			{"residencyGeneration", PublishedValue(value.residencyGeneration)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"geometryRoute", PublishedValue(value.geometryRoute)},
			{"animationExecutionClass", PublishedValue(value.animationExecutionClass)},
			{"renderPassVariant", PublishedValue(value.renderPassVariant)},
			{"viewVariant", PublishedValue(value.viewVariant)},
			{"qualityVariant", PublishedValue(value.qualityVariant)},
			{"debugVariant", PublishedValue(value.debugVariant)},
			{"pipelineFlags", PublishedValue(value.pipelineFlags)},
			{"reserved", PublishedValue(value.reserved)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationBatch& value)
	{
		return {
			{"key", PublishedValue(value.key)},
			{"generationKey", PublishedValue(value.generationKey)},
			{"firstWorkPacket", PublishedValue(value.firstWorkPacket)},
			{"workPacketCount", PublishedValue(value.workPacketCount)},
			{"stableBatchOrdinal", PublishedValue(value.stableBatchOrdinal)},
			{"reserved", PublishedValue(value.reserved)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationBatchGenerationKey& value)
	{
		return {
			{"programGeneration", PublishedValue(value.programGeneration)},
			{"controllerGeneration", PublishedValue(value.controllerGeneration)},
			{"skeletonGeneration", PublishedValue(value.skeletonGeneration)},
			{"clipGeneration", PublishedValue(value.clipGeneration)},
			{"meshGeneration", PublishedValue(value.meshGeneration)},
			{"resourceGeneration", PublishedValue(value.resourceGeneration)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"sceneGeneration", PublishedValue(value.sceneGeneration)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationBatchKey& value)
	{
		return {
			{"programContentId", PublishedValue(value.programContentId)},
			{"skeletonTopologyId", PublishedValue(value.skeletonTopologyId)},
			{"resourceLayoutId", PublishedValue(value.resourceLayoutId)},
			{"stageMask", PublishedValue(value.stageMask)},
			{"outputFlags", PublishedValue(value.outputFlags)},
			{"codecVersion", PublishedValue(value.codecVersion)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationDispatchCommand& value)
	{
		return {
			{"pushConstants", PublishedValue(value.pushConstants)},
			{"groupCountX", PublishedValue(value.groupCountX)},
			{"groupCountY", PublishedValue(value.groupCountY)},
			{"groupCountZ", PublishedValue(value.groupCountZ)},
			{"batchIndex", PublishedValue(value.batchIndex)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationDispatchRecord& value)
	{
		return {
			{"stableInstanceId", PublishedValue(value.stableInstanceId)},
			{"elementOffset", PublishedValue(value.elementOffset)},
			{"workPacketIndex", PublishedValue(value.workPacketIndex)},
			{"generationRecordIndex", PublishedValue(value.generationRecordIndex)},
			{"stage", PublishedValue(value.stage)},
			{"elementCount", PublishedValue(value.elementCount)},
			{"flags", PublishedValue(value.flags)},
			{"barrierFlags", PublishedValue(value.barrierFlags)},
			{"diagnosticIndex", PublishedValue(value.diagnosticIndex)},
			{"resourceLayoutId", PublishedValue(value.resourceLayoutId)},
			{"batchIndex", PublishedValue(value.batchIndex)},
			{"stableSubmissionOrdinal", PublishedValue(value.stableSubmissionOrdinal)},
			{"reserved0", PublishedValue(value.reserved0)},
			{"reserved1", PublishedValue(value.reserved1)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationGenerationRecord& value)
	{
		return {
			{"instanceSlot", PublishedValue(value.instanceSlot)},
			{"instanceGeneration", PublishedValue(value.instanceGeneration)},
			{"componentGeneration", PublishedValue(value.componentGeneration)},
			{"workGeneration", PublishedValue(value.workGeneration)},
			{"programGeneration", PublishedValue(value.programGeneration)},
			{"controllerGeneration", PublishedValue(value.controllerGeneration)},
			{"skeletonGeneration", PublishedValue(value.skeletonGeneration)},
			{"clipGeneration", PublishedValue(value.clipGeneration)},
			{"meshGeneration", PublishedValue(value.meshGeneration)},
			{"resourceGeneration", PublishedValue(value.resourceGeneration)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"sceneGeneration", PublishedValue(value.sceneGeneration)},
			{"frameIdLow", PublishedValue(value.frameIdLow)},
			{"frameIdHigh", PublishedValue(value.frameIdHigh)},
			{"flags", PublishedValue(value.flags)},
			{"reserved", PublishedValue(value.reserved)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPaletteRecord& value)
	{
		return {
			{"row0", PublishedValue(value.row0)},
			{"row1", PublishedValue(value.row1)},
			{"row2", PublishedValue(value.row2)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPlan& value)
	{
		return {
			{"generation", PublishedValue(value.generation)},
			{"buildLimits", PublishedValue(value.buildLimits)},
			{"residentPayloads", PublishedValue(value.residentPayloads)},
			{"sharedResidentPayloads", PublishedValue(value.sharedResidentPayloads)},
			{"generationRecords", PublishedValue(value.generationRecords)},
			{"workPackets", PublishedValue(value.workPackets)},
			{"stablePacketIndices", PublishedValue(value.stablePacketIndices)},
			{"evaluationSourcePacketIndices", PublishedValue(value.evaluationSourcePacketIndices)},
			{"renderIdentities", PublishedValue(value.renderIdentities)},
			{"dispatchRecords", PublishedValue(value.dispatchRecords)},
			{"dispatchCommands", PublishedValue(value.dispatchCommands)},
			{"batches", PublishedValue(value.batches)},
			{"stageSpans", PublishedValue(value.stageSpans)},
			{"diagnostics", PublishedValue(value.diagnostics)},
			{"retainedStaticPlan", PublishedValue(value.retainedStaticPlan)},
			{"retainedDynamicLanes", PublishedValue(value.retainedDynamicLanes)},
			{"dynamicGenerationRecords", PublishedValue(value.dynamicGenerationRecords)},
			{"dynamicSampleTicks", PublishedValue(value.dynamicSampleTicks)},
			{"dynamicRenderIdentities", PublishedValue(value.dynamicRenderIdentities)},
			{"topologyInputSignature", PublishedValue(value.topologyInputSignature)},
			{"topologyMembershipGeneration", PublishedValue(value.topologyMembershipGeneration)},
			{"topologyResourceGeneration", PublishedValue(value.topologyResourceGeneration)},
			{"topologyProgramGeneration", PublishedValue(value.topologyProgramGeneration)},
			{"topologySceneGeneration", PublishedValue(value.topologySceneGeneration)},
			{"topologyRendererGeneration", PublishedValue(value.topologyRendererGeneration)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPlanDiagnostics& value)
	{
		return {
			{"flags", PublishedValue(value.flags)},
			{"rejectedRequestCount", PublishedValue(value.rejectedRequestCount)},
			{"fallbackRequestCount", PublishedValue(value.fallbackRequestCount)},
			{"arenaFailureCount", PublishedValue(value.arenaFailureCount)},
			{"prefixSumFailureCount", PublishedValue(value.prefixSumFailureCount)},
			{"dispatchLimitFailureCount", PublishedValue(value.dispatchLimitFailureCount)},
			{"duplicateStableOrderCount", PublishedValue(value.duplicateStableOrderCount)},
			{"palettePartitionFailureCount", PublishedValue(value.palettePartitionFailureCount)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPlanDynamicLanes& value)
	{
		return {
			{"generationRecords", PublishedValue(value.generationRecords)},
			{"sampleTicks", PublishedValue(value.sampleTicks)},
			{"renderIdentities", PublishedValue(value.renderIdentities)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPlanLimits& value)
	{
		return {
			{"maximumPackets", PublishedValue(value.maximumPackets)},
			{"maximumBatches", PublishedValue(value.maximumBatches)},
			{"maximumDispatchRecords", PublishedValue(value.maximumDispatchRecords)},
			{"maximumDispatchCommands", PublishedValue(value.maximumDispatchCommands)},
			{"maximumElementsPerStage", PublishedValue(value.maximumElementsPerStage)},
			{"maximumGroupsX", PublishedValue(value.maximumGroupsX)},
			{"threadsPerGroup", PublishedValue(value.threadsPerGroup)},
			{"scratchAlignmentBytes", PublishedValue(value.scratchAlignmentBytes)},
			{"maximumPalettePartitions", PublishedValue(value.maximumPalettePartitions)},
			{"reserved0", PublishedValue(value.reserved0)},
			{"reserved1", PublishedValue(value.reserved1)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationPushConstants& value)
	{
		return {
			{"stage", PublishedValue(value.stage)},
			{"firstDispatchRecord", PublishedValue(value.firstDispatchRecord)},
			{"dispatchRecordCount", PublishedValue(value.dispatchRecordCount)},
			{"firstElementLow", PublishedValue(value.firstElementLow)},
			{"firstElementHigh", PublishedValue(value.firstElementHigh)},
			{"elementCount", PublishedValue(value.elementCount)},
			{"planGeneration", PublishedValue(value.planGeneration)},
			{"flags", PublishedValue(value.flags)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationRenderIdentity& value)
	{
		return {
			{"meshId", PublishedValue(value.meshId)},
			{"meshHeaderVersion", PublishedValue(value.meshHeaderVersion)},
			{"deformablePayloadGeneration", PublishedValue(value.deformablePayloadGeneration)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"sceneGeneration", PublishedValue(value.sceneGeneration)},
			{"skeletonCompatibilityFingerprint", PublishedValue(value.skeletonCompatibilityFingerprint)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationResidentPayloads& value)
	{
		return {
			{"programWords", PublishedValue(value.programWords)},
			{"packedClipBytes", PublishedValue(value.packedClipBytes)},
			{"skeletonTables", PublishedValue(value.skeletonTables)},
			{"maskTables", PublishedValue(value.maskTables)},
			{"morphInputs", PublishedValue(value.morphInputs)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationStageSpan& value)
	{
		return {
			{"totalElementCount", PublishedValue(value.totalElementCount)},
			{"stage", PublishedValue(value.stage)},
			{"firstCommand", PublishedValue(value.firstCommand)},
			{"commandCount", PublishedValue(value.commandCount)},
			{"firstDispatchRecord", PublishedValue(value.firstDispatchRecord)},
			{"dispatchRecordCount", PublishedValue(value.dispatchRecordCount)},
			{"barrierFlags", PublishedValue(value.barrierFlags)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Animation::GpuAnimationWorkPacket& value)
	{
		return {
			{"stableInstanceId", PublishedValue(value.stableInstanceId)},
			{"stableProgramId", PublishedValue(value.stableProgramId)},
			{"inputPoseOffsetBytes", PublishedValue(value.inputPoseOffsetBytes)},
			{"scratchOffsetBytes", PublishedValue(value.scratchOffsetBytes)},
			{"localPoseOffsetBytes", PublishedValue(value.localPoseOffsetBytes)},
			{"modelPoseOffsetBytes", PublishedValue(value.modelPoseOffsetBytes)},
			{"paletteOffsetBytes", PublishedValue(value.paletteOffsetBytes)},
			{"morphOutputOffsetBytes", PublishedValue(value.morphOutputOffsetBytes)},
			{"generationRecordIndex", PublishedValue(value.generationRecordIndex)},
			{"clipResourceIndex", PublishedValue(value.clipResourceIndex)},
			{"programResourceIndex", PublishedValue(value.programResourceIndex)},
			{"skeletonResourceIndex", PublishedValue(value.skeletonResourceIndex)},
			{"maskResourceIndex", PublishedValue(value.maskResourceIndex)},
			{"morphResourceIndex", PublishedValue(value.morphResourceIndex)},
			{"jointCount", PublishedValue(value.jointCount)},
			{"morphCount", PublishedValue(value.morphCount)},
			{"paletteEntryCount", PublishedValue(value.paletteEntryCount)},
			{"palettePartitionCount", PublishedValue(value.palettePartitionCount)},
			{"scratchSizeBytes", PublishedValue(value.scratchSizeBytes)},
			{"stageMask", PublishedValue(value.stageMask)},
			{"flags", PublishedValue(value.flags)},
			{"diagnosticIndex", PublishedValue(value.diagnosticIndex)},
			{"resourceLayoutId", PublishedValue(value.resourceLayoutId)},
			{"boundsOutputIndex", PublishedValue(value.boundsOutputIndex)},
			{"stableSubmissionOrdinal", PublishedValue(value.stableSubmissionOrdinal)},
			{"sampleTick", PublishedValue(value.sampleTick)},
			{"reserved0", PublishedValue(value.reserved0)},
			{"reserved1", PublishedValue(value.reserved1)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::AnimationBackendWorkPlan& value)
	{
		return {
			{"frameId", PublishedValue(value.frameId)},
			{"ranges", PublishedValue(value.ranges)},
			{"gpuAnimatorCount", PublishedValue(value.gpuAnimatorCount)},
			{"cpuOrFallbackAnimatorCount", PublishedValue(value.cpuOrFallbackAnimatorCount)},
			{"fullImmutablePlanCoverage", PublishedValue(value.fullImmutablePlanCoverage)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"immutablePlan", PublishedValue(value.immutablePlan)},
			{"cpuDeformationPlan", PublishedValue(value.cpuDeformationPlan)},
			{"valid", PublishedValue(value.valid)},
			{"rejectionReason", PublishedValue(value.rejectionReason)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::AnimationBackendWorkRange& value)
	{
		return {
			{"stableInstanceId", PublishedValue(value.stableInstanceId)},
			{"animationPlanGeneration", PublishedValue(value.animationPlanGeneration)},
			{"paletteGeneration", PublishedValue(value.paletteGeneration)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"firstGpuWorkPacket", PublishedValue(value.firstGpuWorkPacket)},
			{"gpuWorkPacketCount", PublishedValue(value.gpuWorkPacketCount)},
			{"gpuStageMask", PublishedValue(value.gpuStageMask)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::CpuAnimationDeformationData& value)
	{
		return {
			{"paletteMatrices", PublishedValue(value.paletteMatrices)},
			{"morphWeights", PublishedValue(value.morphWeights)},
			{"jointCount", PublishedValue(value.jointCount)},
			{"contentIdLow", PublishedValue(value.contentIdLow)},
			{"contentIdHigh", PublishedValue(value.contentIdHigh)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::CpuAnimationDeformationPlan& value)
	{
		return {
			{"frameId", PublishedValue(value.frameId)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"records", PublishedValue(value.records)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::CpuAnimationDeformationRecord& value)
	{
		return {
			{"stableInstanceId", PublishedValue(value.stableInstanceId)},
			{"meshId", PublishedValue(value.meshId)},
			{"poseGeneration", PublishedValue(value.poseGeneration)},
			{"deformablePayloadGeneration", PublishedValue(value.deformablePayloadGeneration)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"skeletonCompatibilityFingerprint", PublishedValue(value.skeletonCompatibilityFingerprint)},
			{"data", PublishedValue(value.data)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::DeformationConsumerWorkRecord& value)
	{
		return {
			{"stableInstanceId", PublishedValue(value.stableInstanceId)},
			{"workIndex", PublishedValue(value.workIndex)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::DeformationGeneration& value)
	{
		return {
			{"stableInstanceId", PublishedValue(value.stableInstanceId)},
			{"objectGeneration", PublishedValue(value.objectGeneration)},
			{"animationPublicationGeneration", PublishedValue(value.animationPublicationGeneration)},
			{"skeletonGeneration", PublishedValue(value.skeletonGeneration)},
			{"morphGeneration", PublishedValue(value.morphGeneration)},
			{"meshPayloadGeneration", PublishedValue(value.meshPayloadGeneration)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"sceneGeneration", PublishedValue(value.sceneGeneration)},
			{"contentIdLow", PublishedValue(value.contentIdLow)},
			{"contentIdHigh", PublishedValue(value.contentIdHigh)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::DeformationWorkRecord& value)
	{
		return {
			{"key", PublishedValue(value.key)},
			{"sourceStableInstanceId", PublishedValue(value.sourceStableInstanceId)},
			{"primaryConsumerStableInstanceId", PublishedValue(value.primaryConsumerStableInstanceId)},
			{"additionalConsumerStableInstanceIds", PublishedValue(value.additionalConsumerStableInstanceIds)},
			{"sourceRecordIndex", PublishedValue(value.sourceRecordIndex)},
			{"vertexOffset", PublishedValue(value.vertexOffset)},
			{"vertexCount", PublishedValue(value.vertexCount)},
			{"meshletBoundsOffset", PublishedValue(value.meshletBoundsOffset)},
			{"meshletCount", PublishedValue(value.meshletCount)},
			{"clusterBoundsOffset", PublishedValue(value.clusterBoundsOffset)},
			{"clusterCount", PublishedValue(value.clusterCount)},
			{"requestedViewMask", PublishedValue(value.requestedViewMask)},
			{"historyValid", PublishedValue(value.historyValid)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::DeformedPageKey& value)
	{
		return {
			{"page", PublishedValue(value.page)},
			{"deformation", PublishedValue(value.deformation)},
			{"inputLayoutVersion", PublishedValue(value.inputLayoutVersion)},
			{"animationLod", PublishedValue(value.animationLod)},
			{"animationLodVersion", PublishedValue(value.animationLodVersion)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::GpuDeformationPlan& value)
	{
		return {
			{"frameId", PublishedValue(value.frameId)},
			{"rendererGeneration", PublishedValue(value.rendererGeneration)},
			{"work", PublishedValue(value.work)},
			{"orderedConsumers", PublishedValue(value.orderedConsumers)},
			{"totalVertexCount", PublishedValue(value.totalVertexCount)},
			{"totalMeshletBoundsCount", PublishedValue(value.totalMeshletBoundsCount)},
			{"totalClusterBoundsCount", PublishedValue(value.totalClusterBoundsCount)},
			{"reusedDemandCount", PublishedValue(value.reusedDemandCount)},
			{"valid", PublishedValue(value.valid)},
			{"sealedComplete", PublishedValue(value.sealedComplete)},
			{"rejectionReason", PublishedValue(value.rejectionReason)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::LightingDepthReduction::DepthRecipe& value)
	{
		return {
			{"extent", PublishedValue(value.extent)},
			{"fillBits", PublishedValue(value.fillBits)},
			{"overrides", PublishedValue(value.overrides)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::LightingDepthReduction::Extent& value)
	{
		return {
			{"width", PublishedValue(value.width)},
			{"height", PublishedValue(value.height)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::LightingDepthReduction::PixelOverride& value)
	{
		return {
			{"x", PublishedValue(value.x)},
			{"y", PublishedValue(value.y)},
			{"bits", PublishedValue(value.bits)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::PublishedAnimationRenderBinding& value)
	{
		return {
			{"animationAuthorityGeneration", PublishedValue(value.animationAuthorityGeneration)},
			{"posePublicationGeneration", PublishedValue(value.posePublicationGeneration)},
			{"deformablePayloadGeneration", PublishedValue(value.deformablePayloadGeneration)},
			{"skeletonCompatibilityFingerprint", PublishedValue(value.skeletonCompatibilityFingerprint)},
			{"source", PublishedValue(value.source)},
			{"status", PublishedValue(value.status)},
			{"cpuDeformation", PublishedValue(value.cpuDeformation)},
			{"cpuDeformationRecordIndex", PublishedValue(value.cpuDeformationRecordIndex)},
			{"safePose", PublishedValue(value.safePose)},
			{"pageLocalDeformationAvailable", PublishedValue(value.pageLocalDeformationAvailable)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RenderBackendWorkPublication& value)
	{
		return {
			{"frameId", PublishedValue(value.frameId)},
			{"animationWork", PublishedValue(value.animationWork)},
			{"deformationWork", PublishedValue(value.deformationWork)},
			{"valid", PublishedValue(value.valid)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RenderReferenceFrame& value)
	{
		return {
			{"frameId", PublishedValue(value.frameId)},
			{"viewId", PublishedValue(value.viewId)},
			{"outputId", PublishedValue(value.outputId)},
			{"canonicalOrigin", PublishedValue(value.canonicalOrigin)},
			{"originGeneration", PublishedValue(value.originGeneration)},
			{"spatialGeneration", PublishedValue(value.spatialGeneration)},
			{"residencyGeneration", PublishedValue(value.residencyGeneration)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RenderViewPresentationInput& value)
	{
		return {
			{"output", PublishedValue(value.output)},
			{"referenceFrame", PublishedValue(value.referenceFrame)},
			{"viewMatrix", PublishedValue(value.viewMatrix)},
			{"projectionMatrix", PublishedValue(value.projectionMatrix)},
			{"cameraLocalPosition", PublishedValue(value.cameraLocalPosition)},
			{"deltaTimeSeconds", PublishedValue(value.deltaTimeSeconds)},
			{"globalTaskShaderCullingEnabled", PublishedValue(value.globalTaskShaderCullingEnabled)},
			{"cullingEpsilon", PublishedValue(value.cullingEpsilon)},
			{"activePreset", PublishedValue(value.activePreset)},
			{"customOverrideActive", PublishedValue(value.customOverrideActive)},
			{"settings", PublishedValue(value.settings)},
			{"efficiencyOptions", PublishedValue(value.efficiencyOptions)},
			{"efficiencyFixtureIntent", PublishedValue(value.efficiencyFixtureIntent)},
			{"efficiencyDepthOracleInput", PublishedValue(value.efficiencyDepthOracleInput)},
			{"renderOptimization", PublishedValue(value.renderOptimization)},
			{"commandLanes", PublishedValue(value.commandLanes)},
			{"retainedCommandLanes", PublishedValue(value.retainedCommandLanes)},
			{"commandSelectionIndices", PublishedValue(value.commandSelectionIndices)},
			{"retainedCommandSelectionIndices", PublishedValue(value.retainedCommandSelectionIndices)},
			{"groupedInstanceRecords", PublishedValue(value.groupedInstanceRecords)},
			{"groupedSubmissions", PublishedValue(value.groupedSubmissions)},
			{"drawResourcePreparationCommandIndices", PublishedValue(value.drawResourcePreparationCommandIndices)},
			{"sky", PublishedValue(value.sky)},
			{"lightCandidates", PublishedValue(value.lightCandidates)},
			{"editorContextTint", PublishedValue(value.editorContextTint)},
			{"sceneViewGridVisible", PublishedValue(value.sceneViewGridVisible)},
			{"sceneViewOrthographic", PublishedValue(value.sceneViewOrthographic)},
			{"shadowDrawLists", PublishedValue(value.shadowDrawLists)},
			{"uiBatch", PublishedValue(value.uiBatch)},
			{"virtualGeometryPacketEvidenceSignature", PublishedValue(value.virtualGeometryPacketEvidenceSignature)},
			{"virtualGeometryVisibilitySetupSignature", PublishedValue(value.virtualGeometryVisibilitySetupSignature)},
			{"virtualGeometryPackets", PublishedValue(value.virtualGeometryPackets)},
			{"virtualGeometryBatches", PublishedValue(value.virtualGeometryBatches)},
			{"virtualGeometryFrameSets", PublishedValue(value.virtualGeometryFrameSets)},
			{"virtualGeometryVisibilityDiagnosticsRequested", PublishedValue(value.virtualGeometryVisibilityDiagnosticsRequested)},
			{"virtualGeometryCompactionDispatches", PublishedValue(value.virtualGeometryCompactionDispatches)},
			{"virtualGeometryGeneratedSubmissions", PublishedValue(value.virtualGeometryGeneratedSubmissions)},
			{"virtualGeometryCounterReadbacks", PublishedValue(value.virtualGeometryCounterReadbacks)},
			{"virtualGeometryFallbackRecords", PublishedValue(value.virtualGeometryFallbackRecords)},
			{"retainedVirtualGeometryGpuEvidence", PublishedValue(value.retainedVirtualGeometryGpuEvidence)},
			{"virtualGeometryPageRetirements", PublishedValue(value.virtualGeometryPageRetirements)},
			{"virtualGeometryDecodedPageUploads", PublishedValue(value.virtualGeometryDecodedPageUploads)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureCamera& value)
	{
		return {
			{"viewType", PublishedValue(value.viewType)},
			{"outputId", PublishedValue(value.outputId)},
			{"cameraPosition", PublishedValue(value.cameraPosition)},
			{"lookAtTarget", PublishedValue(value.lookAtTarget)},
			{"referenceOrigin", PublishedValue(value.referenceOrigin)},
			{"up", PublishedValue(value.up)},
			{"verticalFovDegrees", PublishedValue(value.verticalFovDegrees)},
			{"nearPlane", PublishedValue(value.nearPlane)},
			{"farPlane", PublishedValue(value.farPlane)},
			{"nativeExtent", PublishedValue(value.nativeExtent)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureCasterSelection& value)
	{
		return {
			{"viewType", PublishedValue(value.viewType)},
			{"outputId", PublishedValue(value.outputId)},
			{"objectIds", PublishedValue(value.objectIds)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureCorrelation& value)
	{
		return {
			{"operationId", PublishedValue(value.operationId)},
			{"fixtureStepId", PublishedValue(value.fixtureStepId)},
			{"stepFrame", PublishedValue(value.stepFrame)},
			{"mutationGeneration", PublishedValue(value.mutationGeneration)},
			{"settingsGeneration", PublishedValue(value.settingsGeneration)},
			{"rendererSessionGeneration", PublishedValue(value.rendererSessionGeneration)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureDepthControls& value)
	{
		return {
			{"oracleRecipe", PublishedValue(value.oracleRecipe)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureIntent& value)
	{
		return {
			{"correlation", PublishedValue(value.correlation)},
			{"holdFixedTick", PublishedValue(value.holdFixedTick)},
			{"sampleTick", PublishedValue(value.sampleTick)},
			{"injection", PublishedValue(value.injection)},
			{"referenceOriginDelta", PublishedValue(value.referenceOriginDelta)},
			{"shadowSelection", PublishedValue(value.shadowSelection)},
			{"viewOrder", PublishedValue(value.viewOrder)},
			{"viewOrigins", PublishedValue(value.viewOrigins)},
			{"cameras", PublishedValue(value.cameras)},
			{"casterSelections", PublishedValue(value.casterSelections)},
			{"lightingControls", PublishedValue(value.lightingControls)},
			{"depthControls", PublishedValue(value.depthControls)},
			{"shadowBatchControls", PublishedValue(value.shadowBatchControls)},
			{"staticShadowControls", PublishedValue(value.staticShadowControls)},
			{"controlGeneration", PublishedValue(value.controlGeneration)},
			{"controlsApplied", PublishedValue(value.controlsApplied)},
			{"controlReason", PublishedValue(value.controlReason)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureLightingControls& value)
	{
		return {
			{"candidateOrder", PublishedValue(value.candidateOrder)},
			{"maxUploadedLights", PublishedValue(value.maxUploadedLights)},
			{"maxDynamicLights", PublishedValue(value.maxDynamicLights)},
			{"maxShadowLights", PublishedValue(value.maxShadowLights)},
			{"snapshotSlot", PublishedValue(value.snapshotSlot)},
			{"frameSlot", PublishedValue(value.frameSlot)},
			{"snapshotViewType", PublishedValue(value.snapshotViewType)},
			{"snapshotOutputId", PublishedValue(value.snapshotOutputId)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureShadowBatchControls& value)
	{
		return {
			{"maxInstancesPerGroup", PublishedValue(value.maxInstancesPerGroup)},
			{"maxTaskGroupsX", PublishedValue(value.maxTaskGroupsX)},
			{"failInstanceUpload", PublishedValue(value.failInstanceUpload)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureShadowSelection& value)
	{
		return {
			{"kind", PublishedValue(value.kind)},
			{"objectId", PublishedValue(value.objectId)},
			{"meshId", PublishedValue(value.meshId)},
			{"lifetimeGeneration", PublishedValue(value.lifetimeGeneration)},
			{"resourceGeneration", PublishedValue(value.resourceGeneration)},
			{"lodIndex", PublishedValue(value.lodIndex)},
			{"meshletOffset", PublishedValue(value.meshletOffset)},
			{"meshletCount", PublishedValue(value.meshletCount)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureStaticShadowControls& value)
	{
		return {
			{"forceTransferUnavailable", PublishedValue(value.forceTransferUnavailable)},
			{"failAllocation", PublishedValue(value.failAllocation)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureViewOrderEntry& value)
	{
		return {
			{"viewType", PublishedValue(value.viewType)},
			{"outputId", PublishedValue(value.outputId)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyFixtureViewOrigin& value)
	{
		return {
			{"viewType", PublishedValue(value.viewType)},
			{"outputId", PublishedValue(value.outputId)},
			{"delta", PublishedValue(value.delta)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyOptions& value)
	{
		return {
			{"shadowReuse", PublishedValue(value.shadowReuse)},
			{"lightingUploads", PublishedValue(value.lightingUploads)},
			{"lightingDepth", PublishedValue(value.lightingDepth)},
			{"shadowSubmission", PublishedValue(value.shadowSubmission)},
			{"staticShadowCacheBudgetBytes", PublishedValue(value.staticShadowCacheBudgetBytes)},
			{"validationPreparation", PublishedValue(value.validationPreparation)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot& value)
	{
		return {
			{"options", PublishedValue(value.options)},
			{"settingsGeneration", PublishedValue(value.settingsGeneration)},
			{"optionsSessionGeneration", PublishedValue(value.optionsSessionGeneration)},
			{"rendererSessionGeneration", PublishedValue(value.rendererSessionGeneration)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::ShadowCanonicalOrigin& value)
	{
		return {
			{"cell", PublishedValue(value.cell)},
			{"offsetMetres", PublishedValue(value.offsetMetres)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::ShadowCasterStamp& value)
	{
		return {
			{"objectId", PublishedValue(value.objectId)},
			{"lifetimeGeneration", PublishedValue(value.lifetimeGeneration)},
			{"segmentId", PublishedValue(value.segmentId)},
			{"meshId", PublishedValue(value.meshId)},
			{"assetId", PublishedValue(value.assetId)},
			{"materialId", PublishedValue(value.materialId)},
			{"shaderId", PublishedValue(value.shaderId)},
			{"textureId", PublishedValue(value.textureId)},
			{"meshRevision", PublishedValue(value.meshRevision)},
			{"assetRevision", PublishedValue(value.assetRevision)},
			{"materialRevision", PublishedValue(value.materialRevision)},
			{"shaderRevision", PublishedValue(value.shaderRevision)},
			{"textureRevision", PublishedValue(value.textureRevision)},
			{"texturePublications", PublishedValue(value.texturePublications)},
			{"worldPosition", PublishedValue(value.worldPosition)},
			{"referenceOrigin", PublishedValue(value.referenceOrigin)},
			{"rotation", PublishedValue(value.rotation)},
			{"scale", PublishedValue(value.scale)},
			{"shadowBehavior", PublishedValue(value.shadowBehavior)},
			{"materialShadowParameters", PublishedValue(value.materialShadowParameters)},
			{"animationInstanceId", PublishedValue(value.animationInstanceId)},
			{"poseGeneration", PublishedValue(value.poseGeneration)},
			{"morphGeneration", PublishedValue(value.morphGeneration)},
			{"paletteGeneration", PublishedValue(value.paletteGeneration)},
			{"deformationGeneration", PublishedValue(value.deformationGeneration)},
			{"deformationProgramGeneration", PublishedValue(value.deformationProgramGeneration)},
			{"deformationContentIdLow", PublishedValue(value.deformationContentIdLow)},
			{"deformationContentIdHigh", PublishedValue(value.deformationContentIdHigh)},
			{"deformationPalette", PublishedValue(value.deformationPalette)},
			{"morphWeights", PublishedValue(value.morphWeights)},
			{"geometryRoute", PublishedValue(value.geometryRoute)},
			{"selectedLod", PublishedValue(value.selectedLod)},
			{"selectionFlags", PublishedValue(value.selectionFlags)},
			{"firstMeshlet", PublishedValue(value.firstMeshlet)},
			{"meshletCount", PublishedValue(value.meshletCount)},
			{"firstCluster", PublishedValue(value.firstCluster)},
			{"clusterCount", PublishedValue(value.clusterCount)},
			{"pageId", PublishedValue(value.pageId)},
			{"fallbackPageId", PublishedValue(value.fallbackPageId)},
			{"pageContentRevision", PublishedValue(value.pageContentRevision)},
			{"residencyGeneration", PublishedValue(value.residencyGeneration)},
			{"payloadContentRevision", PublishedValue(value.payloadContentRevision)},
			{"payloadByteOffset", PublishedValue(value.payloadByteOffset)},
			{"payloadByteSize", PublishedValue(value.payloadByteSize)},
			{"payloadSource", PublishedValue(value.payloadSource)},
			{"emittedSource", PublishedValue(value.emittedSource)},
			{"staticEligible", PublishedValue(value.staticEligible)},
			{"staticSupport", PublishedValue(value.staticSupport)},
			{"staticClassificationReason", PublishedValue(value.staticClassificationReason)},
			{"evidenceMask", PublishedValue(value.evidenceMask)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::ShadowStaticSupport& value)
	{
		return {
			{"authoredStatic", PublishedValue(value.authoredStatic)},
			{"knownDefaultShadowAbi", PublishedValue(value.knownDefaultShadowAbi)},
			{"ownerPublicationsComplete", PublishedValue(value.ownerPublicationsComplete)},
			{"materialShadowSupported", PublishedValue(value.materialShadowSupported)},
			{"geometrySupported", PublishedValue(value.geometrySupported)},
			{"deformationAbsent", PublishedValue(value.deformationAbsent)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::ShadowTexturePublicationStamp& value)
	{
		return {
			{"textureId", PublishedValue(value.textureId)},
			{"publicationGeneration", PublishedValue(value.publicationGeneration)},
			{"bindingIndex", PublishedValue(value.bindingIndex)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Renderer::StaticPageKey& value)
	{
		return {
			{"meshId", PublishedValue(value.meshId)},
			{"meshPayloadGeneration", PublishedValue(value.meshPayloadGeneration)},
			{"pageId", PublishedValue(value.pageId)},
			{"pageContentGeneration", PublishedValue(value.pageContentGeneration)},
			{"residencyGeneration", PublishedValue(value.residencyGeneration)},
			{"layoutVersion", PublishedValue(value.layoutVersion)},
			{"codecProfile", PublishedValue(value.codecProfile)},
			{"codecVersion", PublishedValue(value.codecVersion)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Scene::WorldCell& value)
	{
		return {
			{"x", PublishedValue(value.x)},
			{"y", PublishedValue(value.y)},
			{"z", PublishedValue(value.z)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Scene::WorldDelta& value)
	{
		return {
			{"cellDelta", PublishedValue(value.cellDelta)},
			{"offsetMetres", PublishedValue(value.offsetMetres)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Scene::WorldPose& value)
	{
		return {
			{"position", PublishedValue(value.position)},
			{"rotation", PublishedValue(value.rotation)},
			{"scale", PublishedValue(value.scale)},
			{"transformRevision", PublishedValue(value.transformRevision)},
			{"coordinateFrameRevision", PublishedValue(value.coordinateFrameRevision)}
		};
	}

	inline ProjectionJson PublishedValue(const ::TerraEngine::Scene::WorldPosition& value)
	{
		return {
			{"cell", PublishedValue(value.cell)},
			{"offsetMetres", PublishedValue(value.offsetMetres)}
		};
	}

	inline ProjectionJson PublishedValue(const ::UIImageRun& value)
	{
		return {
			{"texturePath", PublishedValue(value.texturePath)},
			{"position", PublishedValue(value.position)},
			{"size", PublishedValue(value.size)},
			{"color", PublishedValue(value.color)},
			{"cornerRadius", PublishedValue(value.cornerRadius)}
		};
	}

	inline ProjectionJson PublishedValue(const ::UIImmediateQuad& value)
	{
		return {
			{"position", PublishedValue(value.position)},
			{"size", PublishedValue(value.size)},
			{"color", PublishedValue(value.color)},
			{"uvRect", PublishedValue(value.uvRect)}
		};
	}

	inline ProjectionJson PublishedValue(const ::UIRenderBatch& value)
	{
		return {
			{"viewportSize", PublishedValue(value.viewportSize)},
			{"staticQuads", PublishedValue(value.staticQuads)},
			{"immediateQuads", PublishedValue(value.immediateQuads)},
			{"textRuns", PublishedValue(value.textRuns)},
			{"imageRuns", PublishedValue(value.imageRuns)}
		};
	}

	inline ProjectionJson PublishedValue(const ::UIStaticQuad& value)
	{
		return {
			{"position", PublishedValue(value.position)},
			{"size", PublishedValue(value.size)},
			{"color", PublishedValue(value.color)},
			{"cornerRadius", PublishedValue(value.cornerRadius)}
		};
	}

	inline ProjectionJson PublishedValue(const ::UITextRun& value)
	{
		return {
			{"text", PublishedValue(value.text)},
			{"position", PublishedValue(value.position)},
			{"bounds", PublishedValue(value.bounds)},
			{"color", PublishedValue(value.color)},
			{"fontSize", PublishedValue(value.fontSize)}
		};
	}

	inline ProjectionJson PublishedValue(const ::VirtualGeometryCounterReadbackRecord& value)
	{
		// Completion latency is diagnostic timing, but availability/reason and
		// every actual counter remain exact authoritative published values.
		return {
			{"viewId", PublishedValue(value.viewId)},
			{"viewType", PublishedValue(value.viewType)},
			{"frameId", PublishedValue(value.frameId)},
			{"source", PublishedValue(value.source)},
			{"candidatePackets", PublishedValue(value.candidatePackets)},
			{"compactedPackets", PublishedValue(value.compactedPackets)},
			{"submittedBatches", PublishedValue(value.submittedBatches)},
			{"rejectedPackets", PublishedValue(value.rejectedPackets)},
			{"failOpenPackets", PublishedValue(value.failOpenPackets)},
			{"selectedMeshlets", PublishedValue(value.selectedMeshlets)},
			{"dispatchedMeshlets", PublishedValue(value.dispatchedMeshlets)},
			{"culledMeshlets", PublishedValue(value.culledMeshlets)},
			{"culledByHiZ", PublishedValue(value.culledByHiZ)},
			{"unavailableReason", PublishedValue(value.unavailableReason)}
		};
	}

	inline ProjectionJson PublishedValue(const ::VirtualGeometryDecodedPageUploadRecord& value)
	{
		return {
			{"assetId", PublishedValue(value.assetId)},
			{"pageId", PublishedValue(value.pageId)},
			{"fallbackPageId", PublishedValue(value.fallbackPageId)},
			{"clusterId", PublishedValue(value.clusterId)},
			{"dependencyPageIds", PublishedValue(value.dependencyPageIds)},
			{"publicationGeneration", PublishedValue(value.publicationGeneration)},
			{"storedByteCount", PublishedValue(value.storedByteCount)},
			{"decodedByteCount", PublishedValue(value.decodedByteCount)},
			{"integrityValue", PublishedValue(value.integrityValue)},
			{"decodedBytes", PublishedValue(value.decodedBytes)}
		};
	}

	inline ProjectionJson PublishedValue(const ::VirtualGeometryGpuFallbackRecord& value)
	{
		return {
			{"viewId", PublishedValue(value.viewId)},
			{"viewType", PublishedValue(value.viewType)},
			{"frameId", PublishedValue(value.frameId)},
			{"reason", PublishedValue(value.reason)},
			{"affectedPackets", PublishedValue(value.affectedPackets)},
			{"affectedBatches", PublishedValue(value.affectedBatches)},
			{"coveragePolicy", PublishedValue(value.coveragePolicy)},
			{"diagnostic", PublishedValue(value.diagnostic)}
		};
	}

	inline ProjectionJson PublishedValue(const ::VirtualGeometryPageRetirementRecord& value)
	{
		return {
			{"meshId", PublishedValue(value.meshId)},
			{"pageId", PublishedValue(value.pageId)},
			{"frameId", PublishedValue(value.frameId)},
			{"byteSize", PublishedValue(value.byteSize)},
			{"protectedFallback", PublishedValue(value.protectedFallback)},
			{"retired", PublishedValue(value.retired)},
			{"reason", PublishedValue(value.reason)}
		};
	}

	inline ProjectionJson PublishedValue(const ::VirtualGeometryQualityProfile& value)
	{
		return {
			{"lodErrorScale", PublishedValue(value.lodErrorScale)},
			{"maxLodDrop", PublishedValue(value.maxLodDrop)},
			{"hiZOcclusionEnabled", PublishedValue(value.hiZOcclusionEnabled)},
			{"hiZSelfOcclusionGuardEnabled", PublishedValue(value.hiZSelfOcclusionGuardEnabled)},
			{"hiZNearPlaneGuardEnabled", PublishedValue(value.hiZNearPlaneGuardEnabled)},
			{"hiZThinGeometryGuardEnabled", PublishedValue(value.hiZThinGeometryGuardEnabled)},
			{"debugVisualizationMode", PublishedValue(value.debugVisualizationMode)},
			{"geometryBudgetBytes", PublishedValue(value.geometryBudgetBytes)},
			{"deformationCurrentBudgetBytes", PublishedValue(value.deformationCurrentBudgetBytes)},
			{"deformationHistoryBudgetBytes", PublishedValue(value.deformationHistoryBudgetBytes)},
			{"deformationBoundsBudgetBytes", PublishedValue(value.deformationBoundsBudgetBytes)},
			{"preloadBias", PublishedValue(value.preloadBias)},
			{"lodTransitionHysteresisPixels", PublishedValue(value.lodTransitionHysteresisPixels)},
			{"lodTransitionStableFrames", PublishedValue(value.lodTransitionStableFrames)}
		};
	}

	inline ProjectionJson PublishedValue(const ::VirtualGeometryRetainedGpuEvidence& value)
	{
		return {
			{"packetRecords", PublishedValue(value.packetRecords)},
			{"batchDescriptors", PublishedValue(value.batchDescriptors)},
			{"packetFrameSets", PublishedValue(value.packetFrameSets)},
			{"compactionDispatches", PublishedValue(value.compactionDispatches)},
			{"generatedSubmissions", PublishedValue(value.generatedSubmissions)},
			{"counterReadbacks", PublishedValue(value.counterReadbacks)},
			{"fallbackRecords", PublishedValue(value.fallbackRecords)}
		};
	}

}
