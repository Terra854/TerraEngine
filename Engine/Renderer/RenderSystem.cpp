// RenderSystem.cpp — Render System Implementation
//
// Implements the RenderSystem class, coordinating the high-level rendering flow for each frame.
// It handles the transition between different rendering states, manages the active camera, and integrates UI and scene drawing.
// The implementation ensures that scene data is correctly prepared and submitted to the RHI for GPU execution.

#include "RenderSystem.h"
#include "UI/UISystem.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "RHI/Vulkan/VulkanRendererWorkCache.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/ShadowWorkPlan.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureCameras.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureDepthControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureShadowBatchControls.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometrySelection.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryRasterBins.h"
#include "Core/FatalErrorReport.h"
#include "Core/Logger/Logger.h"
#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Window/WindowSystem.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/TerrainComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyComponent.h"
#include "Components/AnimatorComponent.h"
#include "Animation/AnimationSystem.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/SceneManager.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Resource/ResourceManager.h"
#include "Resource/ShaderCollection.h"
#include "Resource/ResourceStreaming.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Utilities/PlatformUtils.h"
#include "ThreadPool/ThreadPool.h"
#include "ThreadPool/ScopedTaskSettlement.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "UI/UISystem.h"
#include <entt/entt.hpp>
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <algorithm>
#include <chrono>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <coroutine>
#include <exception>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace
{
	bool ValidateDepthFixtureIntent(const TerraEngine::Renderer::RendererEfficiencyFixtureIntent& intent, std::string& reason)
	{
		if (intent.staticShadowControls && intent.correlation.operationId.empty()) {
			reason = "Static shadow fault controls require an owned validation session."; return false;
		}
		if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureShadowBatchControls(
			intent.shadowBatchControls, !intent.correlation.operationId.empty(), reason)) return false;
		if (!intent.depthControls) { reason.clear(); return true; }
		std::array<TerraEngine::Renderer::LightingDepthReduction::Extent,2> extents{};
		if (intent.cameras.size()>extents.size()) { reason="Depth controls exceed the sealed camera capacity."; return false; }
		for (std::size_t index=0;index<intent.cameras.size();++index)
			extents[index]={intent.cameras[index].nativeExtent.x,intent.cameras[index].nativeExtent.y};
		return TerraEngine::Renderer::ValidateRendererEfficiencyFixtureDepthControls(intent.depthControls,
			!intent.correlation.operationId.empty(),std::span(extents.data(),intent.cameras.size()),reason);
	}
	const glm::vec3 kSkyReferenceSunDirection = glm::normalize(glm::vec3(0.0f, 0.35f, -1.0f));
	constexpr float kDirectionalShadowNear = 0.1f;
	constexpr float kDirectionalShadowFar = 180.0f;
	constexpr float kDirectionalShadowExtent = 40.0f;
	constexpr std::size_t kRenderParallelThreshold = 64u;
	constexpr std::uint64_t kRenderHashOffset = 1469598103934665603ull;
	constexpr std::uint64_t kRenderHashPrime = 1099511628211ull;
	constexpr const char* kDeterministicCapturePngBase64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=";
	constexpr float kVirtualGeometryClusterRefinementErrorPixels = 1.5f;
	constexpr std::size_t kCompatibleAnimationPoseCacheCapacity = 4096U;

	RenderItemRecord::PublishedAnimationGeneration
	BuildPublishedAnimationGeneration(
		const TerraEngine::Renderer::AnimationRenderBinding& source) noexcept
	{
		RenderItemRecord::PublishedAnimationGeneration result{};
		result.stableInstanceId = source.gpu.IsComplete()
			? source.gpu.stableInstanceId
			: (static_cast<std::uint64_t>(source.animatorSlotIndex) << 32U)
				| source.animatorGeneration;
		result.poseGeneration = source.posePublicationGeneration;
		result.morphGeneration = source.posePublicationGeneration;
		result.deformationContentIdLow = source.deformationContentIdLow;
		result.deformationContentIdHigh = source.deformationContentIdHigh;
		result.paletteGeneration = source.gpu.IsComplete()
			? source.gpu.paletteGeneration : source.posePublicationGeneration;
		result.deformablePayloadGeneration = source.deformablePayloadGeneration;
		result.programGeneration = source.gpu.programGeneration;
		result.animationPlanGeneration = source.gpu.planGeneration;
		result.rendererGeneration = source.gpu.IsComplete()
			? source.gpu.rendererGeneration : source.invalidation.renderer;
		result.firstGpuWorkPacket = source.gpu.workPacketIndex;
		result.gpuWorkPacketCount = source.gpu.IsComplete() ? 1U : 0U;
		result.gpuStageMask = source.gpu.stageMask;
		result.source = static_cast<std::uint8_t>(source.gpu.IsComplete()
			? (source.gpu.hybridCpuAuthority
				? TerraEngine::Renderer::AnimationRenderEvaluationSource::HybridAuthoritative
				: TerraEngine::Renderer::AnimationRenderEvaluationSource::GpuVisual)
			: (source.safePose
				? TerraEngine::Renderer::AnimationRenderEvaluationSource::SafeFallback
				: TerraEngine::Renderer::AnimationRenderEvaluationSource::CpuPose));
		result.complete = source.gpu.IsComplete();
		return result;
	}

	// These facts are copied from the immutable, loader-validated TEMFPRD1
	// directory before route preparation.  Route selection must not use optional
	// visibility bounds as a proxy for geometry: bounds can be intentionally
	// omitted while the canonical runtime layout and production descriptors stay
	// complete.
	struct CanonicalGeometryRouteFacts
	{
		bool complete = false;
		bool directUsable = false;
		bool virtualGeometryUsable = false;
		std::uint32_t representationFlags = 0u;
		std::uint32_t sourceTriangleCount = 0u;
		std::uint32_t packedMeshletCount = 0u;
		std::uint32_t physicalPageCount = 0u;
		std::uint32_t physicalBlockCount = 0u;
		std::uint64_t storedBytes = 0u;
		std::uint64_t decodedBytes = 0u;
		std::uint64_t calibrationIdentity = 0u;
	};

	bool FitsCanonicalDirectoryRange(
		const std::vector<std::uint8_t>& storage,
		std::uint64_t offset,
		std::uint32_t count,
		std::size_t elementSize) noexcept
	{
		// The production loader owns full validation.  This second bounded check
		// protects snapshot preparation if a synthetic Mesh reaches this layer.
		if(elementSize == 0u || offset > storage.size()) return false;
		const std::size_t begin = static_cast<std::size_t>(offset);
		return count <= (storage.size() - begin) / elementSize;
	}

	CanonicalGeometryRouteFacts BuildCanonicalGeometryRouteFacts(const Mesh& mesh)
	{
		CanonicalGeometryRouteFacts facts{};
		const bool generatedDirect = mesh.generatedDirectMeshletRoute.IsComplete();
		bool declaresDirect = generatedDirect;
		bool declaresVirtual = false;
		std::uint32_t logicalPageSizeBytes = 0u;
		bool directResidentDescriptors = true;
		if(generatedDirect) {
			// A procedural producer has no serialized TEMF directory. Its typed
			// publication receipt supplies only immutable identity and resident-byte
			// facts; the stream/range checks below still gate route eligibility.
			facts.representationFlags = kTemfRepresentationDirectMeshlets;
			facts.storedBytes = mesh.generatedDirectMeshletRoute.residentPayloadBytes;
			facts.decodedBytes = mesh.generatedDirectMeshletRoute.residentPayloadBytes;
			facts.calibrationIdentity =
				mesh.generatedDirectMeshletRoute.sourceContentIdentity
				^ mesh.generatedDirectMeshletRoute.generationIdentity;
			if(facts.calibrationIdentity == 0u) facts.calibrationIdentity = 1u;
		}
		else {
			if(mesh.storage.size() < sizeof(TemfProductionHeader)) return facts;
			TemfProductionHeader header{};
			std::memcpy(&header, mesh.storage.data(), sizeof(header));
			if(header.magic != kMeshMagicValue
				|| header.formatVersion != kMeshCurrentVersion
				|| header.minorVersion != kMeshCurrentMinorVersion
				|| header.headerBytes != kTemfProductionHeaderBytes
				|| header.formatContractId != kTemfProductionContractId
				|| (header.representationFlags & ~kTemfRepresentationKnownMask) != 0u
				|| !FitsCanonicalDirectoryRange(mesh.storage, header.blockTableOffset,
					header.blockCount, sizeof(TemfBlockDescriptor))
				|| !FitsCanonicalDirectoryRange(mesh.storage, header.pageTableOffset,
					header.pageCount, sizeof(TemfPageDescriptor))) {
				return facts;
			}
			declaresDirect =
				(header.representationFlags & kTemfRepresentationDirectMeshlets) != 0u;
			declaresVirtual =
				(header.representationFlags & kTemfRepresentationVirtualGeometry) != 0u;
			if(!declaresDirect && !declaresVirtual) return facts;

			for(std::uint32_t index = 0u; index < header.blockCount; ++index) {
				TemfBlockDescriptor block{};
				const std::size_t offset = static_cast<std::size_t>(header.blockTableOffset)
					+ static_cast<std::size_t>(index) * sizeof(block);
				std::memcpy(&block, mesh.storage.data() + offset, sizeof(block));
				if(block.encodedBytes == 0u || block.decodedBytes == 0u
					|| block.encodedBytes > std::numeric_limits<std::uint64_t>::max()
						- facts.storedBytes
					|| block.decodedBytes > std::numeric_limits<std::uint64_t>::max()
						- facts.decodedBytes) {
					return {};
				}
				facts.storedBytes += block.encodedBytes;
				facts.decodedBytes += block.decodedBytes;
				directResidentDescriptors = directResidentDescriptors
					&& (block.flags & kTemfBlockFlagDirectResident) != 0u
					&& block.logicalPageId == kTemfDirectResidentPageId;
			}
			facts.representationFlags = header.representationFlags;
			facts.physicalPageCount = header.pageCount;
			facts.physicalBlockCount = header.blockCount;
			logicalPageSizeBytes = header.logicalPageSizeBytes;
			facts.calibrationIdentity = header.buildRecipeHash
				^ header.generationIdentity
				^ (static_cast<std::uint64_t>(header.selectedCodecProfileId) << 32u)
				^ header.selectedQuantizationProfileId;
		}

		std::uint64_t primitiveCount = 0u;
		std::uint64_t generatedVertexOffset = 0u;
		std::uint64_t generatedPrimitiveOffset = 0u;
		for(const MeshletRecord& record : mesh.meshletRecords) {
			if(record.primitiveIndexCount == 0u
				|| record.primitiveIndexCount % 3u != 0u
				|| primitiveCount > std::numeric_limits<std::uint64_t>::max()
					- record.primitiveIndexCount
				|| (generatedDirect
					&& (record.vertexOffset != generatedVertexOffset
						|| record.primitiveIndexOffset != generatedPrimitiveOffset))) {
				return {};
			}
			primitiveCount += record.primitiveIndexCount;
			generatedVertexOffset += record.vertexCount;
			generatedPrimitiveOffset += record.primitiveIndexCount;
		}
		if(mesh.runtimeLayout.meshletCount == 0u
			|| mesh.runtimeLayout.meshletCount != mesh.meshletRecords.size()
			|| mesh.runtimeLayout.meshletPrimitiveIndexCount != primitiveCount
			|| mesh.meshletPrimitiveIndexData.size() != primitiveCount) {
			return {};
		}
		if(generatedDirect) {
			const std::size_t remapStride = mesh.runtimeLayout.meshletVertexRemapEncoding
				== static_cast<std::uint32_t>(MeshletRemapEncoding::Uint16)
				? sizeof(std::uint16_t)
				: mesh.runtimeLayout.meshletVertexRemapEncoding
					== static_cast<std::uint32_t>(MeshletRemapEncoding::Uint32)
					? sizeof(std::uint32_t) : 0u;
			const std::size_t expectedVertexBytes =
				static_cast<std::size_t>(mesh.runtimeLayout.meshletVertexCount)
				* mesh.runtimeLayout.layout.strideBytes;
			const std::size_t expectedRemapBytes =
				static_cast<std::size_t>(mesh.runtimeLayout.meshletVertexRemapCount)
				* remapStride;
			if(mesh.runtimeLayout.layout.strideBytes != kMeshletPackedVertexStrideBytes
				|| remapStride == 0u
				|| mesh.meshletVertexData.size() != expectedVertexBytes
				|| mesh.meshletVertexRemapData.size() != expectedRemapBytes
				|| mesh.meshletVertexRemapDecoded.size()
					!= mesh.runtimeLayout.meshletVertexRemapCount
				|| generatedVertexOffset != mesh.runtimeLayout.meshletVertexRemapCount
				|| mesh.runtimeLayout.meshletSectionCount != mesh.meshletSections.size()
				|| mesh.runtimeLayout.meshletBoundsCount != mesh.meshletBounds.size()
				|| mesh.meshletBounds.size() != mesh.meshletRecords.size()) {
				return {};
			}
			for(const MeshletSection& section : mesh.meshletSections) {
				const std::uint64_t sectionEnd =
					static_cast<std::uint64_t>(section.meshletOffset) + section.meshletCount;
				if(section.meshletCount == 0u
					|| sectionEnd > mesh.runtimeLayout.meshletCount) return {};
			}
		}

		facts.complete = true;
		// The direct payload may contain several resident LOD ranges. Route cost and
		// draw batching describe one selected range, not the sum of every alternative
		// detail packed beside it.
		const VirtualGeometryDetailLevelRecord* baseDetail =
			mesh.virtualGeometryDetailLevels.empty()
				? nullptr
				: &mesh.virtualGeometryDetailLevels.front();
		facts.packedMeshletCount = !declaresVirtual && baseDetail != nullptr
			? baseDetail->meshletCount
			: mesh.runtimeLayout.meshletCount;
		facts.sourceTriangleCount = baseDetail != nullptr
			? baseDetail->primitiveCount
			: static_cast<std::uint32_t>(primitiveCount / 3u);
		// Direct-only TEMFPRD1 cooks prove that every descriptor is resident;
		// hybrid cooks can share decoded runtime streams without that restriction.
		// Generated meshes instead carry the typed resident receipt validated above.
		facts.directUsable = declaresDirect
			&& (generatedDirect || declaresVirtual || directResidentDescriptors);
		facts.virtualGeometryUsable = declaresVirtual
			&& logicalPageSizeBytes != 0u
			&& facts.physicalPageCount != 0u
			&& mesh.hasVirtualGeometry
			&& !mesh.virtualGeometryClusters.empty();
		return facts;
	}

	TerraEngine::Renderer::AnimationRenderBindingStatus ValidateAnimatorPosePublication(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request,
		const TerraEngine::Animation::AnimationAuthorityRow& authorityRow) noexcept
	{
		using TerraEngine::Renderer::AnimationRenderBindingStatus;
		if(authorityRow.safeFallback
			|| authorityRow.status != TerraEngine::Animation::AnimationStatus::Ok) {
			return AnimationRenderBindingStatus::SafePose;
		}
		const TerraEngine::Animation::AnimatorInstanceHandle animator =
			authorityRow.key.Handle();
		if(!authorityRow.IsValid()
			|| authorityRow.ownerObjectId != request.animatorObjectId
			|| authorityRow.componentGeneration != request.componentGeneration
			|| !animator.IsValid()
			|| animator.slotIndex != request.animatorSlotIndex
			|| animator.generation != request.animatorGeneration
			|| !authorityRow.skeleton.IsValid()
			|| authorityRow.skeleton.assetId.value != request.skeletonAssetId
			|| authorityRow.skeleton.generation != request.skeletonGeneration) {
			return AnimationRenderBindingStatus::StaleGeneration;
		}
		if(authorityRow.skeletonCompatibilityFingerprint
			!= request.skeletonCompatibilityFingerprint) {
			return AnimationRenderBindingStatus::IncompatibleSkeleton;
		}
		if(authorityRow.snapshot == nullptr || authorityRow.snapshot->pose == nullptr
			|| authorityRow.snapshot->animator != animator
			|| authorityRow.snapshot->publicationGeneration == 0U
			|| authorityRow.snapshot->playback.latestStatus
				!= TerraEngine::Animation::AnimationStatus::Ok
			|| authorityRow.snapshot->pose->ValidateForPublication()
				!= TerraEngine::Animation::AnimationStatus::Ok) {
			return AnimationRenderBindingStatus::InvalidPose;
		}

		const TerraEngine::Animation::AnimationPoseGeneration& poseGeneration =
			authorityRow.snapshot->pose->Generation();
		if(poseGeneration.animator != animator
			|| poseGeneration.poseGeneration != authorityRow.snapshot->publicationGeneration
			|| poseGeneration.skeleton != authorityRow.skeleton
			|| poseGeneration.programGeneration == 0U) {
			return AnimationRenderBindingStatus::InvalidPose;
		}
		return AnimationRenderBindingStatus::Ready;
	}

	bool HeldAnimationBindingMatchesRequest(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request) noexcept
	{
		using namespace TerraEngine::Renderer;
		const AnimationRenderBinding& held = request.heldBinding;
		if(held.status != AnimationRenderBindingStatus::Ready || held.safePose
			|| !held.HasCpuPose() || held.posePublicationGeneration == 0U
			|| held.meshId != request.meshId
			|| held.animatorObjectId != request.animatorObjectId
			|| held.skeletonAssetId != request.skeletonAssetId
			|| held.animatorObjectGeneration != request.animatorObjectGeneration
			|| held.animationAuthorityGeneration != request.animationAuthorityGeneration
			|| held.animatorSlotIndex != request.animatorSlotIndex
			|| held.animatorGeneration != request.animatorGeneration
			|| held.deformablePayloadGeneration != request.deformablePayloadGeneration
			|| held.componentGeneration != request.componentGeneration
			|| held.skeletonGeneration != request.skeletonGeneration
			|| held.skeletonCompatibilityFingerprint
				!= request.skeletonCompatibilityFingerprint
			|| held.invalidation.asset != request.invalidation.asset
			|| held.invalidation.page != request.invalidation.page
			|| held.invalidation.material != request.invalidation.material
			|| held.invalidation.renderer != request.invalidation.renderer
			|| held.invalidation.residency != request.invalidation.residency) {
			return false;
		}
		return held.authorityRow != nullptr
			&& ValidateAnimatorPosePublication(request, *held.authorityRow)
				== AnimationRenderBindingStatus::Ready
			&& held.authorityRow->snapshot->publicationGeneration
				== held.posePublicationGeneration;
	}

	bool GpuVisualIdentityMatchesRequest(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request,
		const TerraEngine::Renderer::AnimationGpuRenderBinding& identity) noexcept
	{
		// GPU Visual is allowed to omit a CPU pose, but it is never allowed to
		// omit identity. Every value below is copied from one published packet and
		// compared as one bundle before a draw can reference its palette.
		if(!identity.IsComplete() || identity.hybridCpuAuthority
			|| request.meshId == TerraEngine::EMPTY_UUID
			|| request.animatorObjectId == TerraEngine::EMPTY_UUID
			|| request.animatorObjectGeneration == 0U
			|| request.animationAuthorityGeneration == 0U
			|| request.deformablePayloadGeneration == 0U
			|| request.componentGeneration == 0U
			|| request.skeletonCompatibilityFingerprint == 0U
			|| !request.expectedGpuRenderIdentity.IsComplete()
			|| !identity.renderIdentity.IsComplete()) {
			return false;
		}
		const std::uint64_t packedStableInstanceId =
			(static_cast<std::uint64_t>(identity.instanceSlot) << 32U)
			| static_cast<std::uint64_t>(identity.instanceGeneration);
		return identity.stableInstanceId == packedStableInstanceId
			&& identity.componentGeneration == request.componentGeneration
			&& identity.renderIdentity == request.expectedGpuRenderIdentity
			&& identity.renderIdentity.meshId == request.meshId
			&& identity.renderIdentity.deformablePayloadGeneration
				== request.deformablePayloadGeneration
			&& identity.renderIdentity.skeletonCompatibilityFingerprint
				== request.skeletonCompatibilityFingerprint
			&& identity.meshGeneration
				== identity.renderIdentity.meshHeaderVersion
			&& identity.rendererGeneration
				== identity.renderIdentity.rendererGeneration
			&& identity.sceneGeneration
				== identity.renderIdentity.sceneGeneration;
	}

	bool IsMorphOnlyIdentityDeformableMesh(const Mesh& mesh) noexcept
	{
		const TemfDeformableMetadata& metadata = mesh.deformableMetadata;
		// Morph-only cooks deliberately carry no skeleton fingerprint. Their one
		// identity joint exists solely because skinning and morph rendering share
		// the frozen deformable vertex layout; it must never be interpreted as a
		// mesh authored for an arbitrary skeleton.
		return metadata.skeletonCompatibilityFingerprint == 0U
			&& metadata.sourceSkeletonJointCount == 1U
			&& metadata.morphTargetCount != 0U
			&& metadata.morphTargetCount == mesh.deformableMorphs.size()
			&& metadata.palettePartitionCount == 1U
			&& mesh.deformablePalettes.size() == 1U
			&& mesh.deformablePalettes.front().jointCount == 1U
			&& mesh.deformablePaletteJoints.size() == 1U
			&& mesh.deformablePaletteJoints.front() == 0U;
	}

	TerraEngine::Animation::AnimationCompatibilityFingerprint
	ResolveDeformableSkeletonFingerprint(
		const Mesh& mesh,
		const TerraEngine::Animation::AnimationAuthorityRow* authorityRow) noexcept
	{
		const TerraEngine::Animation::AnimationCompatibilityFingerprint
			authoredFingerprint =
			mesh.deformableMetadata.skeletonCompatibilityFingerprint;
		if(authoredFingerprint != 0U) return authoredFingerprint;
		if(!IsMorphOnlyIdentityDeformableMesh(mesh) || authorityRow == nullptr
			|| authorityRow->skeletonCompatibilityFingerprint == 0U
			|| authorityRow->skeletonAsset == nullptr
			|| authorityRow->skeletonAsset->compatibilityFingerprint
				!= authorityRow->skeletonCompatibilityFingerprint
			|| authorityRow->skeletonAsset->joints.size() != 1U
			|| authorityRow->skeletonAsset->inverseBindMatrices.size() != 1U) {
			return 0U;
		}
		// A checked one-joint publication supplies lifecycle identity for the
		// shared CPU graphics bridge. The mesh remains skeleton-unbound on disk;
		// only its morph lanes are allowed to use this effective fingerprint.
		return authorityRow->skeletonCompatibilityFingerprint;
	}

	bool IsValidatedAnimationDeformableMesh(const Mesh& mesh) noexcept
	{
		const TemfDeformableMetadata& metadata = mesh.deformableMetadata;
		constexpr std::uint32_t requiredFlags = kTemfDeformableFlagConservativeMorphEnvelope
			| kTemfDeformableFlagZeroWeightMeansIdentity
			| kTemfDeformableFlagVertexPaletteIndices
			| kTemfDeformableFlagDenseMorphDeltas;
		// ResourceManager sets hasDeformablePayload only after full range validation.
		// Rechecking canonical counts here keeps synthetic in-memory Mesh values
		// from waking AnimationSystem for static content.
		return mesh.hasDeformablePayload
			&& mesh.deformablePayloadGeneration != 0U
			&& metadata.skinningVertexCount != 0U
			&& metadata.skinningVertexCount == mesh.runtimeLayout.meshletSkinningDataCount
			&& metadata.palettePartitionCount != 0U
			&& metadata.palettePartitionCount == mesh.deformablePalettes.size()
			&& mesh.deformableVertexPaletteIndices.size()
				== metadata.skinningVertexCount
			&& mesh.meshletSkinningData.size()
				== static_cast<std::size_t>(metadata.skinningVertexCount)
					* kTemfSkinningStrideBytes
			&& metadata.sourceSkeletonJointCount != 0U
			&& (metadata.skeletonCompatibilityFingerprint != 0U
				|| IsMorphOnlyIdentityDeformableMesh(mesh))
			&& metadata.morphTargetCount == mesh.deformableMorphs.size()
			&& metadata.flags == requiredFlags;
	}

	RenderViewCommandLane MakeRenderViewCommandLane(
		const RenderItemRecord& command) noexcept
	{
		RenderViewCommandLane lane{};
		lane.materialIndex = command.materialIndex;
		lane.meshDataIndex = command.meshDataIndex;
		lane.textureIndex = command.textureIndex;
		lane.virtualMeshletOffset = command.virtualMeshletOffset;
		lane.virtualMeshletCount = command.virtualMeshletCount;
		lane.virtualGeometryFlags = command.virtualGeometryFlags;
		lane.selectedVirtualClusterOffset = command.selectedVirtualClusterOffset;
		lane.selectedVirtualClusterCount = command.selectedVirtualClusterCount;
		lane.selectedVirtualPageNeedOffset = command.selectedVirtualPageNeedOffset;
		lane.selectedVirtualPageNeedCount = command.selectedVirtualPageNeedCount;
		lane.virtualGeometryPageId = command.virtualGeometryPageId;
		lane.virtualGeometryFallbackPageId = command.virtualGeometryFallbackPageId;
		lane.virtualGeometryPayloadSource = command.virtualGeometryPayloadSource;
		lane.virtualGeometryEmittedSource = command.virtualGeometryEmittedSource;
		lane.virtualGeometryPagePayloadByteOffset =
			command.virtualGeometryPagePayloadByteOffset;
		lane.virtualGeometryPagePayloadByteSize =
			command.virtualGeometryPagePayloadByteSize;
		lane.virtualGeometryPayloadBufferIndex =
			command.virtualGeometryPayloadBufferIndex;
		lane.virtualGeometryPayloadDescriptorIndex =
			command.virtualGeometryPayloadDescriptorIndex;
		lane.virtualGeometryPacketDescriptorIndex =
			command.virtualGeometryPacketDescriptorIndex;
		lane.taskShaderCullingEnabled = command.taskShaderCullingEnabled;
		return lane;
	}

	void ApplyRenderViewCommandLane(const RenderViewCommandLane& lane,
		std::uint32_t selectedLod, TerraEngine::Renderer::GeometryRoute route,
		const glm::vec4& editorTint, RenderItemRecord& command) noexcept
	{
		command.materialIndex = lane.materialIndex;
		command.meshDataIndex = lane.meshDataIndex;
		command.textureIndex = lane.textureIndex;
		command.virtualMeshletOffset = lane.virtualMeshletOffset;
		command.virtualMeshletCount = lane.virtualMeshletCount;
		command.selectedVirtualLod = selectedLod;
		command.geometryRoute = route;
		command.editorTint = editorTint;
		command.virtualGeometryFlags = lane.virtualGeometryFlags;
		command.selectedVirtualClusterOffset = lane.selectedVirtualClusterOffset;
		command.selectedVirtualClusterCount = lane.selectedVirtualClusterCount;
		command.selectedVirtualPageNeedOffset = lane.selectedVirtualPageNeedOffset;
		command.selectedVirtualPageNeedCount = lane.selectedVirtualPageNeedCount;
		command.virtualGeometryPageId = lane.virtualGeometryPageId;
		command.virtualGeometryFallbackPageId = lane.virtualGeometryFallbackPageId;
		command.virtualGeometryPayloadSource = lane.virtualGeometryPayloadSource;
		command.virtualGeometryEmittedSource = lane.virtualGeometryEmittedSource;
		command.virtualGeometryPagePayloadByteOffset =
			lane.virtualGeometryPagePayloadByteOffset;
		command.virtualGeometryPagePayloadByteSize =
			lane.virtualGeometryPagePayloadByteSize;
		command.virtualGeometryPayloadBufferIndex =
			lane.virtualGeometryPayloadBufferIndex;
		command.virtualGeometryPayloadDescriptorIndex =
			lane.virtualGeometryPayloadDescriptorIndex;
		command.virtualGeometryPacketDescriptorIndex =
			lane.virtualGeometryPacketDescriptorIndex;
		command.taskShaderCullingEnabled = lane.taskShaderCullingEnabled;
	}

	struct CachedDrawCommandRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		RenderItemRecord command;
	};

	struct DrawCommandCacheState
	{
		std::unordered_map<TerraEngine::UUID, CachedDrawCommandRecord> recordsByObjectId;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
		// Stale-object pruning is membership work, not frame work. These exact
		// protected revisions let a stable crowd skip rebuilding an alive UUID set
		// and rescanning the complete cache on every rendered frame.
		std::uint64_t observedSceneGeneration = 0U;
		std::uint64_t observedActiveMembershipRevision = 0U;
		std::uint64_t observedComponentRevision = 0U;
		std::uint64_t observedHierarchyRevision = 0U;
		std::uint64_t observedSpatialRevision = 0U;
		std::uint64_t observedMeshComponentRevision = 0U;
		bool baseInputsValid = false;
	};

	struct RenderableFrameIdentityCacheState
	{
		// Visible-cache animation refreshes need object-to-frame indices. Retain the
		// map while the exact deterministic object order is unchanged instead of
		// allocating and hashing thousands of UUIDs every frame.
		std::vector<TerraEngine::UUID> objectIds;
		std::vector<std::uint64_t> drawCommandSignatures;
		std::unordered_map<TerraEngine::UUID, std::size_t> indicesByObjectId;
		std::uint64_t worldSignature = 0U;
		// Static frame caches outlive a project reload. Require the protected scene
		// generation as well as matching UUID order before borrowing retained state.
		std::uint64_t sceneGeneration = 0U;
		bool valid = false;
	};

	struct RenderableDrawWorkingSet
	{
		// Base draw commands deliberately retain no animation publication. That
		// permits these coordinator-owned arrays to reuse capacity across frames
		// without extending pose lifetimes; visible commands attach the current
		// frame binding only when they are materialized below.
		std::vector<RenderItemRecord> commands;
		std::vector<std::uint64_t> signatures;
	};

	struct CachedLightCandidateRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		FrameLightCandidate candidate;
	};

	struct LightCandidateCacheState
	{
		std::unordered_map<TerraEngine::UUID, CachedLightCandidateRecord> recordsByLightId;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct VisibleCommandCacheRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		// Stable views retain canonical ordinals plus only their view-specific
		// selectors. Shared identity, material, and transform state is resolved from
		// the current working set and later from the sealed publication roots.
		std::vector<std::uint32_t> stableOrdinals;
		std::vector<std::uint32_t> selectedLods;
		std::vector<std::uint32_t> routeSelectors;
		std::vector<glm::vec4> editorTints;
		std::shared_ptr<const std::vector<RenderViewCommandLane>> commandLanes;
		// These two immutable arrays are populated lazily on the first cache hit,
		// then retained by sealed publications without another crowd-sized rebuild.
		mutable std::shared_ptr<const TerraEngine::Renderer::
			RenderViewSelectionPayload> retainedViewSelection;
		mutable std::shared_ptr<const std::vector<std::uint32_t>>
			retainedCommandSelectionIndices;
		// Cached direct-route commands never contribute a VG view draw. Retaining
		// only the exact contributing command indices prevents a large direct
		// crowd from being rescanned merely to republish an empty VG selection.
		std::vector<std::uint32_t> virtualGeometryCommandIndices;
		std::shared_ptr<const TerraEngine::Renderer::
			VirtualGeometryRetainedSelectionEvidence> retainedSelectionEvidence;
		std::shared_ptr<const VirtualGeometryRetainedGpuEvidence>
			retainedGpuEvidence;
		// View draw ranges are derived entirely from this revision-validated cut.
		// Retaining them avoids reconstructing the same per-command range records on
		// every cache hit, including valid direct-only cuts whose vector is empty.
		std::shared_ptr<const std::vector<
			TerraEngine::Renderer::VirtualGeometryViewDrawRecord>> viewDraws;
		bool hasViewDrawEvidence = false;
		// Page evidence depends only on selection and residency inputs already
		// covered by this record's signature. Cache it with the cut so stable views
		// do not rebuild hashes and diagnostic strings every frame.
		TerraEngine::Renderer::VisibleStreamingTouchSet streamingTouchSet;
		TerraEngine::Resource::TerrainVisibleTextureTouchSnapshot terrainTextureTouches;
		TerraEngine::Renderer::VisibleCommandReuseTelemetry reuseTelemetry;
		TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics;
	};

	struct VisibleCommandCacheState
	{
		std::unordered_map<std::uint64_t, VisibleCommandCacheRecord> recordsBySignature;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct ShadowDrawListCacheRecord
	{
		std::uint64_t signature = 0u;
		// Fixture cache selectors retain exact identity beyond the mismatch hash.
		RenderViewType controlledViewType = RenderViewType::Scene;
		std::uint32_t controlledOutputId = 0;
		std::uint64_t fixtureControlGeneration = 0;
		std::vector<TerraEngine::UUID> controlledCasterIds;
		bool controlledCasterSelection = false;
		std::uint64_t lastTouchedFrameId = 0u;
		std::vector<ShadowDrawList> shadowDrawLists;
	};

	struct ShadowDrawListCacheState
	{
		std::unordered_map<std::uint64_t, ShadowDrawListCacheRecord> recordsBySignature;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct VisibleCommandBuildResult
	{
		std::uint64_t packetEvidenceSignature = 0u;
		std::vector<RenderItemRecord> commands;
		std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord> selectedClusters;
		std::vector<TerraEngine::Renderer::VirtualGeometryPageNeedRecord> pageNeeds;
		std::vector<VirtualGeometryClusterPacket> clusterPackets;
		std::vector<ClusterWorkBatch> clusterBatches;
		std::vector<GpuVirtualGeometryPacketRecord> gpuPacketRecords;
		std::vector<GpuVirtualGeometryBatchDescriptor> gpuBatchDescriptors;
		std::vector<GpuVirtualGeometryPacketFrameSet> gpuPacketFrameSets;
		std::vector<GpuPacketCompactionDispatch> gpuCompactionDispatches;
		std::vector<GeneratedVirtualGeometrySubmissionRecord> generatedSubmissions;
		std::vector<VirtualGeometryCounterReadbackRecord> gpuCounterReadbacks;
		std::vector<VirtualGeometryGpuFallbackRecord> gpuFallbackRecords;
		std::vector<CompactedVisibilitySet> compactionResults;
		std::vector<FallbackCoverageChain> fallbackCoverage;
		std::vector<VirtualGeometryLODTransitionState> lodTransitionStates;
		TerraEngine::Renderer::VisibleStreamingTouchSet streamingTouchSet;
		TerraEngine::Resource::TerrainVisibleTextureTouchSnapshot terrainTextureTouches;
		TerraEngine::Renderer::VisibleCommandReuseTelemetry reuseTelemetry;
		TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics;
		// A cache hit borrows one stable unordered_map value until this result is
		// consumed immediately by the current view. Rehash preserves references,
		// and trimming cannot run again before the caller copies these spans into
		// its protected RenderViewBuildState.
		const VisibleCommandCacheRecord* cachedRecord = nullptr;
		bool cacheHit = false;

		template<typename RenderableContainer, typename CommandContainer>
		void PopulateSelectedItems(
			std::vector<RenderItemRecord>& destination,
			const RenderableContainer& currentRenderables,
			const CommandContainer& baseCommands,
			std::vector<std::uint8_t>* visibleAnimatedFlags,
			const std::unordered_map<TerraEngine::UUID, std::size_t>*
				currentRenderableIndices,
			std::uint64_t* groupingCompatibilitySignature) const
		{
			std::uint64_t animationCompatibilitySignature = 1469598103934665603ULL;
			const auto appendAnimationCompatibility =
				[&animationCompatibilitySignature](const RenderItemRecord& command)
			{
				const std::uint64_t authorityLane =
					static_cast<std::uint32_t>(command.animationBinding.source)
					| (static_cast<std::uint64_t>(
						command.animationBinding.RequiresConservativeCoarseVisibility()) << 32U)
					| (static_cast<std::uint64_t>(
						command.animationBinding.status) << 40U);
				animationCompatibilitySignature ^= authorityLane + 0x9e3779b97f4a7c15ULL
					+ (animationCompatibilitySignature << 6U)
					+ (animationCompatibilitySignature >> 2U);
				// Visibility setup resolves deformation bridge offsets by stable owner.
				// Fold that owner into the proof while the refreshed binding is hot.
				const std::uint64_t stableInstanceId =
					command.animation.stableInstanceId;
				animationCompatibilitySignature ^= stableInstanceId
					+ 0x9e3779b97f4a7c15ULL
					+ (animationCompatibilitySignature << 6U)
					+ (animationCompatibilitySignature >> 2U);
			};
			if(cachedRecord == nullptr || !commands.empty())
			{
				destination = commands;
				for(const RenderItemRecord& command : destination)
				{
					appendAnimationCompatibility(command);
					if(visibleAnimatedFlags == nullptr
						|| currentRenderableIndices == nullptr
						|| command.animationBinding.status
						!= TerraEngine::Renderer::AnimationRenderBindingStatus::Ready)
					{
						continue;
					}
					const auto current = currentRenderableIndices->find(command.objectId);
					if(current != currentRenderableIndices->end()
						&& current->second < visibleAnimatedFlags->size())
					{
						(*visibleAnimatedFlags)[current->second] = 1u;
					}
				}
			}
			else
			{
				destination.clear();
				const std::size_t bindingCount = cachedRecord->stableOrdinals.size();
				destination.reserve(bindingCount);
				for(std::size_t index = 0U; index < bindingCount; ++index)
				{
					const std::uint32_t renderableIndex =
						cachedRecord->stableOrdinals[index];
					if(renderableIndex < currentRenderables.size()
						&& renderableIndex < baseCommands.size()
						&& index < cachedRecord->selectedLods.size()
						&& index < cachedRecord->routeSelectors.size()
						&& index < cachedRecord->editorTints.size()
						&& cachedRecord->commandLanes != nullptr
						&& index < cachedRecord->commandLanes->size()) {
						RenderItemRecord command = baseCommands[renderableIndex];
						command.animation = BuildPublishedAnimationGeneration(
							currentRenderables[renderableIndex].animationBinding);
						command.animationBinding =
							TerraEngine::Renderer::PublishedAnimationRenderBinding(
								currentRenderables[renderableIndex].animationBinding);
						ApplyRenderViewCommandLane((*cachedRecord->commandLanes)[index],
							cachedRecord->selectedLods[index],
							static_cast<TerraEngine::Renderer::GeometryRoute>(
								cachedRecord->routeSelectors[index]),
							cachedRecord->editorTints[index], command);
						destination.push_back(std::move(command));
						if(visibleAnimatedFlags != nullptr
							&& renderableIndex < visibleAnimatedFlags->size()
							&& destination.back().animationBinding.status
								== TerraEngine::Renderer::AnimationRenderBindingStatus::Ready)
						{
							(*visibleAnimatedFlags)[renderableIndex] = 1u;
						}
						appendAnimationCompatibility(destination.back());
					}
				}
				if(bindingCount != destination.size())
				{
					// A partial cache-to-renderable mapping cannot prove the omitted
					// commands. Disable downstream reuse so the complete validation path
					// remains authoritative for malformed or stale cache records.
					animationCompatibilitySignature = 0u;
				}
			}
			if(groupingCompatibilitySignature != nullptr)
			{
				// Visibility evidence already covers every static grouping lane. Add
				// only the current animation authority classes while bindings are hot,
				// avoiding a second full command scan in grouped submission planning.
				*groupingCompatibilitySignature = packetEvidenceSignature;
				if(animationCompatibilitySignature == 0u)
				{
					*groupingCompatibilitySignature = 0u;
				}
				else
				{
					*groupingCompatibilitySignature ^= animationCompatibilitySignature
						+ 0x9e3779b97f4a7c15ULL
						+ (*groupingCompatibilitySignature << 6U)
						+ (*groupingCompatibilitySignature >> 2U);
				}
			}
		}
		[[nodiscard]] const std::vector<GpuVirtualGeometryPacketRecord>& GpuPacketRecords() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->packetRecords : gpuPacketRecords;
		}
		[[nodiscard]] const std::vector<GpuVirtualGeometryBatchDescriptor>& GpuBatchDescriptors() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->batchDescriptors : gpuBatchDescriptors;
		}
		[[nodiscard]] const std::vector<GpuVirtualGeometryPacketFrameSet>& GpuPacketFrameSets() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->packetFrameSets : gpuPacketFrameSets;
		}
		[[nodiscard]] const std::vector<GpuPacketCompactionDispatch>& GpuCompactionDispatches() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->compactionDispatches : gpuCompactionDispatches;
		}
		[[nodiscard]] const std::vector<GeneratedVirtualGeometrySubmissionRecord>& GeneratedSubmissions() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->generatedSubmissions : generatedSubmissions;
		}
		[[nodiscard]] const std::vector<VirtualGeometryCounterReadbackRecord>& GpuCounterReadbacks() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->counterReadbacks : gpuCounterReadbacks;
		}
		[[nodiscard]] const std::vector<VirtualGeometryGpuFallbackRecord>& GpuFallbackRecords() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->fallbackRecords : gpuFallbackRecords;
		}
	};

	struct GroupedCompatibilityBucket
	{
		std::uint32_t topologyTemplateIndex =
			PersistentRenderTopologyRecord::kInvalidGroupTemplateIndex;
		std::uint32_t representative = 0u;
		std::vector<std::uint32_t> members;
	};

	struct GroupedSubmissionMembershipCacheRecord
	{
		bool valid = false;
		std::uint64_t compatibilitySignature = 0U;
		std::uint64_t lastTouchedFrameId = 0U;
		std::vector<GroupedCompatibilityBucket> buckets;
		// These descriptors and preparation indices depend only on the stable
		// compatibility membership above. Frame, view, and renderer generations are
		// patched when the cache is consumed, so a warm frame does not rediscover the
		// same group layout or rebuild its fallback coverage.
		std::vector<GroupedRenderSubmission> submissionTemplates;
		std::vector<std::uint32_t> drawResourcePreparationCommandIndices;
	};

	struct GroupedSubmissionMembershipCacheState
	{
		const RenderSystem* owner = nullptr;
		std::unordered_map<std::uint64_t, GroupedSubmissionMembershipCacheRecord>
			recordsByView;
	};

	// Persistent renderer preparation belongs to RenderSystem, but its concrete
	// cache types remain private to this implementation file.
	struct RetainedRenderTopologyState
	{
		std::uint64_t stableSignature = 0u;
		PersistentRenderTopology topology{};
		// Slot ownership survives topology rebuilds. The immutable allocator value
		// is replaced only after a complete membership transaction commits, while
		// compact selections retain the exact lifetime key for recycled ordinals.
		TerraEngine::Renderer::RenderSlotAllocatorState slotAllocator{};
		std::unordered_map<TerraEngine::UUID,
			TerraEngine::Renderer::RenderSlotGenerationKey> slotKeysByObject;
		std::vector<TerraEngine::UUID> orderedObjectIds;
		// These two sidecars share render-frame snapshot order. Stable frames can
		// therefore recover both the one-based identity and the reverse ordinal
		// mapping without repeating thousands of UUID hash-table probes.
		std::vector<TerraEngine::Renderer::RenderSlotGenerationKey> orderedSlotKeys;
		std::vector<std::uint32_t> renderableIndexByOrdinal;
		std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityRecord>
			conventionalVisibilityIdentityTable;
		std::unordered_map<TerraEngine::UUID, std::uint32_t>
			conventionalVisibilityIdentityIndices;
		std::uint64_t sceneGeneration = 0u;
		std::uint64_t componentMutationRevision = 0u;
		std::uint64_t meshComponentRevision = 0u;
		std::uint64_t hierarchyRevision = 0u;
		std::uint64_t resourceRevision = 0u;
		std::uint64_t rendererGeneration = 0u;
		const RenderSystem* owner = nullptr;
	};

	struct SpatialPreparationCacheState
	{
		std::uint64_t signature = 0U;
		bool valid = false;
		std::uint64_t observedSceneGeneration = 0U;
		std::uint64_t observedSpatialRevision = 0U;
		std::uint64_t observedMeshComponentRevision = 0U;
		std::uint64_t observedMeshResourceRevision = 0U;
		std::size_t observedRenderableCount = 0U;
		std::vector<TerraEngine::Scene::SpatialObjectRecord> records;
		std::unordered_map<entt::entity, std::size_t> renderableLookup;
		std::vector<bool> conservativeAnimationBounds;
	};

	struct VirtualGeometryViewSelectionPacket
	{
		// RenderSystem prepares these vectors from immutable mesh/runtime data
		// before the protected GPU submit boundary. Streaming and diagnostics
		// consume the copied packet, not ResourceManager's live mesh cache.
		std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord> selectedClusters;
		std::vector<TerraEngine::Renderer::VirtualGeometryPageNeedRecord> pageNeeds;
		std::vector<VirtualGeometryClusterPacket> clusterPackets;
		std::vector<ClusterWorkBatch> clusterBatches;
		std::vector<GpuVirtualGeometryPacketRecord> gpuPacketRecords;
		std::vector<GpuVirtualGeometryBatchDescriptor> gpuBatchDescriptors;
		std::vector<GpuVirtualGeometryPacketFrameSet> gpuPacketFrameSets;
		std::vector<GpuPacketCompactionDispatch> gpuCompactionDispatches;
		std::vector<GeneratedVirtualGeometrySubmissionRecord> generatedSubmissions;
		std::vector<VirtualGeometryCounterReadbackRecord> gpuCounterReadbacks;
		std::vector<VirtualGeometryGpuFallbackRecord> gpuFallbackRecords;
		std::vector<CompactedVisibilitySet> compactionResults;
		std::vector<FallbackCoverageChain> fallbackCoverage;
		std::vector<VirtualGeometryLODTransitionState> lodTransitionStates;
	};

	VirtualGeometryAssetRuntime BuildVirtualGeometryRuntimeFromMesh(const TerraEngine::UUID& meshId, const Mesh& mesh)
	{
		VirtualGeometryAssetRuntime runtime{};
		const TerraEngine::Resource::VirtualGeometryPhysicalPageResidencySnapshot residency =
			TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance().ResidencySnapshot();
		const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication> publications =
			TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance().PublishedPages();
		runtime.assetId = meshId;
		runtime.cookStatus = mesh.hasVirtualGeometry ? VirtualGeometryCookStatus::Cooked : VirtualGeometryCookStatus::NeedsRebuild;
		runtime.sourceTriangleCount = mesh.hasVirtualGeometry
			? mesh.virtualGeometryHeader.sourceTriangleCount
			: (mesh.virtualGeometryDetailLevels.empty()
				? 0u : mesh.virtualGeometryDetailLevels.front().primitiveCount);
		runtime.sourceVertexCount = mesh.hasVirtualGeometry
			? mesh.virtualGeometryHeader.sourceVertexCount
			: (mesh.virtualGeometryDetailLevels.empty()
				? 0u : mesh.virtualGeometryDetailLevels.front().vertexCount);
		runtime.clusterCoverageValidated =
			(mesh.virtualGeometryHeader.flags & kVirtualGeometryValidationFlagClusterCoverageValidated) != 0u;

		// Convert the loader-owned TEMF tables into a compact runtime mirror.
		// Render preparation copies this mirror into immutable frame snapshots,
		// so workers can choose LOD/page ranges without touching ResourceManager
		// caches or live scene/component storage.
		runtime.requestedTargets.reserve(mesh.virtualGeometryRequestedTargets.size());
		for (const VirtualGeometryRequestedTargetRecord& target : mesh.virtualGeometryRequestedTargets)
		{
			VirtualGeometryRequestedTargetRuntime record{};
			record.targetIndex = target.targetIndex;
			record.requestedRatio = target.requestedRatio;
			record.desiredTriangleCount = target.desiredTriangleCount;
			record.desiredMeshletCount = target.desiredMeshletCount;
			record.status = static_cast<VirtualGeometryDetailTargetStatus>(target.status);
			runtime.requestedTargets.push_back(record);
		}
		runtime.detailLevels.reserve(mesh.virtualGeometryDetailLevels.size());
		for (const VirtualGeometryDetailLevelRecord& detail : mesh.virtualGeometryDetailLevels)
		{
			VirtualGeometryDetailRuntime record{};
			record.lodIndex = detail.lodIndex;
			record.meshletOffset = detail.meshletOffset;
			record.meshletCount = detail.meshletCount;
			record.triangleCount = detail.primitiveCount;
			record.geometricError = detail.geometricError;
			record.targetRatio = detail.targetRatio;
			record.boundsMin = glm::vec3(detail.boundsMin[0], detail.boundsMin[1], detail.boundsMin[2]);
			record.boundsMax = glm::vec3(detail.boundsMax[0], detail.boundsMax[1], detail.boundsMax[2]);
			record.requestedTargetIndex = detail.requestedTargetIndex;
			record.targetStatus = static_cast<VirtualGeometryDetailTargetStatus>(detail.targetStatus);
			runtime.detailLevels.push_back(record);

			VirtualGeometryGeneratedLevelRuntime generated{};
			generated.generatedLevelIndex = static_cast<std::uint32_t>(runtime.generatedLevels.size());
			generated.requestedTargetIndex = detail.requestedTargetIndex;
			generated.lodIndex = detail.lodIndex;
			generated.actualRatio = detail.targetRatio;
			generated.triangleCount = detail.primitiveCount;
			generated.meshletCount = detail.meshletCount;
			generated.geometricError = detail.geometricError;
			runtime.generatedLevels.push_back(generated);
		}
		runtime.skippedDetails.reserve(mesh.virtualGeometrySkippedDetails.size());
		for (const VirtualGeometrySkippedDetailRecord& skipped : mesh.virtualGeometrySkippedDetails)
		{
			VirtualGeometrySkippedDetailRuntime record{};
			record.requestedTargetIndex = skipped.targetIndex;
			record.requestedRatio = skipped.requestedRatio;
			record.status = static_cast<VirtualGeometryDetailTargetStatus>(skipped.status);
			record.reason = static_cast<VirtualGeometrySkippedDetailReason>(skipped.reason);
			record.nearestGeneratedLevelIndex = skipped.nearestGeneratedLodIndex;
			record.candidateTriangleCount = skipped.candidateTriangleCount;
			record.candidateMeshletCount = skipped.candidateMeshletCount;
			record.actualCandidateRatio = skipped.actualCandidateRatio;
			if (skipped.messageStringSize > 0u
				&& skipped.messageStringOffset + skipped.messageStringSize <= mesh.virtualGeometryDiagnosticStrings.size())
			{
				record.message = mesh.virtualGeometryDiagnosticStrings.substr(skipped.messageStringOffset, skipped.messageStringSize);
			}
			runtime.skippedDetails.push_back(std::move(record));
		}
		runtime.cullableGroups.reserve(mesh.virtualGeometryCullableGroups.size());
		for (const VirtualGeometryCullableGroupRecord& group : mesh.virtualGeometryCullableGroups)
		{
			const std::uint32_t groupOrdinal = static_cast<std::uint32_t>(runtime.cullableGroups.size());
			VirtualGeometryCullableGroupRuntime record{};
			record.groupIndex = group.groupIndex;
			record.lodIndex = group.lodIndex;
			// TEMF v1 stores one cullable group per meshlet. The group index is
			// therefore the meshlet table coordinate the task shader needs; the
			// vertex offset points into the remap stream and must never be used as
			// a meshlet dispatch offset.
			record.meshletIndex = group.groupIndex < mesh.meshletRecords.size() ? group.groupIndex : groupOrdinal;
			record.sectionId = group.sectionId;
			record.pageId = group.pageId;
			record.boundsCenter = glm::vec3(group.boundsCenter[0], group.boundsCenter[1], group.boundsCenter[2]);
			record.boundsRadius = group.boundsRadius;
			runtime.cullableGroups.push_back(record);
		}
		runtime.clusters.reserve(mesh.virtualGeometryClusters.size());
		for (const VirtualGeometryClusterRecord& cluster : mesh.virtualGeometryClusters)
		{
			VirtualGeometryClusterRuntime record{};
			record.clusterId = cluster.clusterId;
			record.parentClusterId = cluster.parentClusterId;
			record.firstChildClusterId = cluster.firstChildClusterId;
			record.childCount = cluster.childCount;
			record.lodIndex = cluster.lodIndex;
			record.firstGroupIndex = cluster.groupOffset;
			record.groupCount = cluster.groupCount;
			record.materialSectionId = cluster.materialSectionId;
			record.pageId = cluster.pageId;
			record.fallbackClusterId = cluster.fallbackClusterId;
			record.geometricError = cluster.geometricError;
			record.boundsCenter = glm::vec3(cluster.boundsCenter[0], cluster.boundsCenter[1], cluster.boundsCenter[2]);
			record.boundsRadius = cluster.boundsRadius;
			runtime.clusters.push_back(record);
		}
		// TEMFPRD1 has no aggregate-profile sidecar. Fallback coverage is proven
		// directly by the canonical cluster/page tables copied below; leaving this
		// vector empty deliberately routes missing pages through that typed chain
		// rather than reviving retired VGAP provenance records.
		runtime.coneSafety.reserve(mesh.virtualGeometryConeSafety.size());
		for (const VirtualGeometryConeSafetyRecord& cone : mesh.virtualGeometryConeSafety)
		{
			ConeSafetyMetadata metadata{};
			metadata.meshId = meshId;
			metadata.clusterId = cone.clusterId;
			metadata.materialSidedness = static_cast<VirtualGeometryConeMaterialSidedness>(cone.materialSidedness);
			metadata.windingState = static_cast<VirtualGeometryConeWindingState>(cone.windingState);
			metadata.alphaRisk = static_cast<VirtualGeometryConeAlphaRisk>(cone.alphaRisk);
			metadata.thinShellRisk = static_cast<VirtualGeometryConeThinShellRisk>(cone.thinShellRisk);
			metadata.coneValid = (cone.flags & kVirtualGeometryConeSafetyFlagConeValid) != 0u;
			std::string cookedDisabledReason;
			if (cone.disabledReasonStringSize > 0u
				&& cone.disabledReasonStringOffset + cone.disabledReasonStringSize <= mesh.virtualGeometryDiagnosticStrings.size())
			{
				cookedDisabledReason = mesh.virtualGeometryDiagnosticStrings.substr(cone.disabledReasonStringOffset, cone.disabledReasonStringSize);
			}
			metadata = NormalizeVirtualGeometryConeSafetyMetadata(std::move(metadata));
			if (!metadata.eligible && !cookedDisabledReason.empty())
			{
				metadata.disabledReason = std::move(cookedDisabledReason);
			}
			runtime.coneSafety.push_back(std::move(metadata));
		}
		runtime.pages.reserve(mesh.virtualGeometryPages.size());
		for (const VirtualGeometryPageRecord& page : mesh.virtualGeometryPages)
		{
			GeometryPageRuntime record{};
			record.pageId = page.pageId;
			record.firstLodIndex = page.firstLodIndex;
			record.lodCount = page.lodCount;
			record.groupOffset = page.groupOffset;
			record.groupCount = page.groupCount;
			record.byteSize = page.byteSize;
			record.priority = static_cast<float>(page.priority);
			record.fallbackPageId = page.fallbackPageId;
			record.fallbackPage = page.pageId == page.fallbackPageId;
			record.clusterOffset = page.clusterPageLinkOffset;
			record.clusterCount = page.clusterPageLinkCount;
				record.payloadByteOffset = page.byteOffset;
				record.payloadByteSize = page.byteSize;
				record.payloadBufferIndex = 0xffffffffu;
				record.payloadDescriptorIndex = 0xffffffffu;
				record.publicationGeneration = mesh.virtualGeometryPhysicalPagePublicationGeneration;
				if (page.pageId < mesh.virtualGeometryPhysicalPageStates.size()
					&& page.pageId < mesh.virtualGeometryPhysicalPagePublicationGenerations.size())
				{
					record.state = static_cast<GeometryPageState>(
						mesh.virtualGeometryPhysicalPageStates[page.pageId]);
					record.publicationGeneration =
						mesh.virtualGeometryPhysicalPagePublicationGenerations[page.pageId];
				}
				const auto streamed = std::find_if(residency.pages.begin(), residency.pages.end(),
					[meshId, pageId = page.pageId](const auto& row) { return row.assetId == meshId && row.pageId == pageId; });
				if(streamed != residency.pages.end()) {
					switch(streamed->state) {
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::Resident: record.state = GeometryPageState::Resident; break;
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::PendingIO: record.state = GeometryPageState::PendingIO; break;
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::PendingDecode: record.state = GeometryPageState::PendingIO; break;
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::PendingUpload: record.state = GeometryPageState::PendingUpload; break;
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::Failed: record.state = GeometryPageState::Failed; break;
					default: break;
					}
					record.publicationGeneration = streamed->residencyGeneration;
				}
				const auto publication = std::find_if(
					publications.begin(),
					publications.end(),
					[meshId, pageId = page.pageId](const auto& value)
					{
						return value.key.asset.assetId == meshId
							&& value.key.pageId == pageId
							&& value.IsVisible();
					});
				if (publication != publications.end())
				{
					record.state = GeometryPageState::Resident;
					record.payloadByteOffset = 0u;
					record.payloadByteSize = publication->payload.decodedByteCount;
					record.byteSize = publication->payload.decodedByteCount;
					record.payloadBufferIndex = publication->descriptor.bufferIndex;
					record.payloadDescriptorIndex = publication->descriptor.descriptorIndex;
					record.allocationId = publication->descriptor.allocationId;
					record.meshPayloadGeneration =
						publication->key.generations.geometryFormatGeneration;
					record.pageContentGeneration =
						publication->key.pageContentGeneration;
					record.residencyGeneration =
						publication->key.generations.residencyGeneration;
					record.publicationGeneration =
						publication->publicationGeneration;
				}
				// Missing publication data is intentionally fail-closed: the cluster
				// resolver will request this page and walk its resident ancestors.
				record.pinnedFallback = record.fallbackPage
					&& record.state == GeometryPageState::Pinned;
				record.protectedFallback = record.pinnedFallback;
				record.eligibleForNewWork = record.state == GeometryPageState::NotResident
					|| record.state == GeometryPageState::Requested
					|| record.state == GeometryPageState::PendingIO
					|| record.state == GeometryPageState::PendingUpload;
			runtime.pages.push_back(record);
		}
		return runtime;
	}

	bool GeometryPageCoversLod(const GeometryPageRuntime& page, std::uint32_t lodIndex)
	{
		if (page.lodCount == 0u)
		{
			return false;
		}
		const std::uint64_t first = page.firstLodIndex;
		const std::uint64_t lastExclusive = first + page.lodCount;
		return static_cast<std::uint64_t>(lodIndex) >= first && static_cast<std::uint64_t>(lodIndex) < lastExclusive;
	}

	bool GeometryPageIsResident(const GeometryPageRuntime& page)
	{
		return page.state == GeometryPageState::Resident
			|| page.state == GeometryPageState::Pinned
			|| page.state == GeometryPageState::PendingRetirement;
	}

	const GeometryPageRuntime* FindGeometryPageForLod(const VirtualGeometryAssetRuntime& asset, std::uint32_t lodIndex)
	{
		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (GeometryPageCoversLod(page, lodIndex))
			{
				return &page;
			}
		}
		return nullptr;
	}

	const GeometryPageRuntime* FindGeometryPageById(const VirtualGeometryAssetRuntime& asset, std::uint32_t pageId)
	{
		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (page.pageId == pageId)
			{
				return &page;
			}
		}
		return nullptr;
	}

	const VirtualGeometryClusterRuntime* FindVirtualGeometryClusterById(const VirtualGeometryAssetRuntime& asset, std::uint32_t clusterId)
	{
		for (const VirtualGeometryClusterRuntime& cluster : asset.clusters)
		{
			if (cluster.clusterId == clusterId)
			{
				return &cluster;
			}
		}
		return nullptr;
	}

	bool VirtualGeometryClusterPageIsResident(const VirtualGeometryAssetRuntime& asset, const VirtualGeometryClusterRuntime& cluster)
	{
		if (asset.pages.empty())
		{
			return true;
		}
		const GeometryPageRuntime* page = FindGeometryPageById(asset, cluster.pageId);
		return page != nullptr && GeometryPageIsResident(*page);
	}

	VirtualGeometryPagePayloadSource ResolveVirtualGeometryPayloadSource(const GeometryPageRuntime* page, bool fallbackPacket)
	{
		if (page == nullptr)
		{
			return VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
		}
		if (!GeometryPageIsResident(*page))
		{
			return VirtualGeometryPagePayloadSource::Missing;
		}
			if (fallbackPacket || page->fallbackPage || page->pinnedFallback || page->protectedFallback
				|| page->state == GeometryPageState::Pinned)
		{
			return VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		}
		return VirtualGeometryPagePayloadSource::ResidentPagePayload;
	}

	VirtualGeometryEmittedPageSource ResolveVirtualGeometryEmittedSource(VirtualGeometryPagePayloadSource source)
	{
		switch (source)
		{
		case VirtualGeometryPagePayloadSource::ResidentPagePayload:
			return VirtualGeometryEmittedPageSource::HighDetailPage;
		case VirtualGeometryPagePayloadSource::ResidentFallbackPayload:
			return VirtualGeometryEmittedPageSource::FallbackPage;
		case VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback:
		case VirtualGeometryPagePayloadSource::EmbeddedMeshPayload:
			return VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback;
		case VirtualGeometryPagePayloadSource::Missing:
		case VirtualGeometryPagePayloadSource::Invalid:
			return VirtualGeometryEmittedPageSource::Invalid;
		case VirtualGeometryPagePayloadSource::Unknown:
			break;
		}
		return VirtualGeometryEmittedPageSource::None;
	}

	struct VirtualGeometryClusterMeshletRange
	{
		std::uint32_t meshletOffset = 0u;
		std::uint32_t meshletCount = 0u;
		bool valid = false;
	};

	std::uint64_t ResolveVirtualGeometryPayloadBytes(const GeometryPageRuntime* page, const VirtualGeometryClusterMeshletRange& meshlets)
	{
		if (page != nullptr)
		{
			return page->payloadByteSize == 0u ? page->byteSize : page->payloadByteSize;
		}
		return static_cast<std::uint64_t>(meshlets.meshletCount);
	}

	std::pair<glm::vec3, glm::vec3> ResolveVirtualGeometryHiZPacketBounds(const VirtualGeometryClusterRuntime& cluster)
	{
		// Page streaming changes payload residency, not the conservative shape
		// tested by Hi-Z. Keep packet bounds tied to the selected cluster so a
		// broad fallback page cannot make the culler believe hidden coverage is
		// larger or smaller than the meshlet range being submitted.
		const glm::vec3 radius(cluster.boundsRadius);
		return {
			cluster.boundsCenter - radius,
			cluster.boundsCenter + radius
		};
	}

	void HashVirtualGeometryPageSourceKey(std::uint64_t& key, std::uint64_t value)
	{
		key ^= value;
		key *= kRenderHashPrime;
	}

	void PopulateVirtualGeometryPageSourceRecords(TerraEngine::Renderer::VirtualGeometryViewSelectionState& state, std::uint64_t frameId)
	{
		// Page payload records describe the resident source that the shader may
		// read. Emitted source records describe the visible result after fallback
		// substitution. Keeping both lets streaming request missing detail while
		// diagnostics still explain why the current frame remained visible.
		std::unordered_set<std::uint64_t> seenPayloads;
		seenPayloads.reserve(state.clusterPackets.size());
		for (const VirtualGeometryClusterPacket& packet : state.clusterPackets)
		{
			if (packet.payloadSource == VirtualGeometryPagePayloadSource::Missing
				|| packet.payloadSource == VirtualGeometryPagePayloadSource::Invalid
				|| packet.payloadSource == VirtualGeometryPagePayloadSource::Unknown)
			{
				continue;
			}
			std::uint64_t key = kRenderHashOffset;
			HashVirtualGeometryPageSourceKey(key, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(packet.meshId)));
			HashVirtualGeometryPageSourceKey(key, packet.pageId);
			HashVirtualGeometryPageSourceKey(key, static_cast<std::uint64_t>(packet.payloadSource));
			HashVirtualGeometryPageSourceKey(key, packet.pagePayloadByteOffset);
			HashVirtualGeometryPageSourceKey(key, packet.pagePayloadByteSize);
			HashVirtualGeometryPageSourceKey(key, packet.payloadBufferIndex);
			HashVirtualGeometryPageSourceKey(key, packet.payloadDescriptorIndex);
			if (seenPayloads.insert(key).second)
			{
				VirtualGeometryPagePayloadRecord payload;
				payload.meshId = packet.meshId;
				payload.pageId = packet.pageId;
				payload.fallbackPageId = packet.fallbackPageId;
				payload.source = packet.payloadSource;
				payload.selectedLod = packet.selectedLod;
				payload.meshletOffset = packet.meshletOffset;
				payload.meshletCount = packet.meshletCount;
				payload.payloadByteOffset = packet.pagePayloadByteOffset;
				payload.payloadByteSize = packet.pagePayloadByteSize;
				payload.payloadBufferIndex = packet.payloadBufferIndex;
				payload.payloadDescriptorIndex = packet.payloadDescriptorIndex;
				payload.resident = packet.payloadSource == VirtualGeometryPagePayloadSource::ResidentPagePayload
					|| packet.payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
				payload.pinnedFallback = packet.fallbackPacket || packet.payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
				payload.valid = true;
				payload.owningMeshFallbackReason = packet.fallbackReason;
				payload.diagnostic = packet.payloadSource == VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback
					? "Virtual geometry emitted conservative embedded mesh payload instead of a resident streamed page."
					: std::string();
				state.residentPagePayloads.push_back(std::move(payload));
			}

			VirtualGeometryEmittedPageSourceRecord emitted;
			emitted.viewId = state.outputId;
			emitted.viewType = state.viewType;
			emitted.frameId = frameId;
			emitted.objectId = packet.objectId;
			emitted.meshId = packet.meshId;
			emitted.materialId = packet.materialId;
			emitted.clusterId = packet.clusterId;
			emitted.selectedLod = packet.selectedLod;
			emitted.pageId = packet.pageId;
			emitted.fallbackPageId = packet.fallbackPageId;
			emitted.payloadSource = packet.payloadSource;
			emitted.emittedSource = packet.emittedSource;
			emitted.meshletOffset = packet.meshletOffset;
			emitted.meshletCount = packet.meshletCount;
			emitted.payloadByteOffset = packet.pagePayloadByteOffset;
			emitted.payloadByteSize = packet.pagePayloadByteSize;
			emitted.payloadBufferIndex = packet.payloadBufferIndex;
			emitted.payloadDescriptorIndex = packet.payloadDescriptorIndex;
			emitted.fallbackSubstitution = packet.fallbackPacket;
			emitted.conservativeEmbeddedFallback = packet.payloadSource == VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
			emitted.reason = packet.fallbackReason.empty() ? ToString(packet.emittedSource) : packet.fallbackReason;
			state.emittedPageSources.push_back(std::move(emitted));
		}
	}

	void PopulateVirtualGeometryPageRequestRecords(TerraEngine::Renderer::VirtualGeometryViewSelectionState& state)
	{
		// Requests are deduplicated per view/object/page before crossing into
		// ResourceStreaming. The renderer never mutates residency here; it only
		// publishes immutable need packets that protected resource code consumes.
		for (const TerraEngine::Renderer::VirtualGeometryPageNeedRecord& need : state.pageNeeds)
		{
			if (need.meshId == TerraEngine::EMPTY_UUID)
			{
				continue;
			}

			const auto appendRequest = [&](std::uint32_t pageId, bool fallbackRequest, VirtualGeometryPagePayloadSource source, VirtualGeometryEmittedPageSource emission, std::string reason) {
				if (pageId == std::numeric_limits<std::uint32_t>::max())
				{
					return;
				}
				const auto duplicate = std::find_if(state.pageRequests.begin(), state.pageRequests.end(), [&](const TerraEngine::Renderer::VirtualGeometryPageRequestRecord& existing) {
					return existing.meshId == need.meshId
						&& existing.objectId == need.objectId
						&& existing.pageId == pageId
						&& existing.fallbackRequest == fallbackRequest;
				});
				if (duplicate != state.pageRequests.end())
				{
					duplicate->priority = std::max(duplicate->priority, static_cast<std::int32_t>(need.screenImportance));
					return;
				}

				TerraEngine::Renderer::VirtualGeometryPageRequestRecord request;
				request.meshId = need.meshId;
				request.objectId = need.objectId;
				request.pageId = pageId;
				request.fallbackPageId = need.fallbackPageId;
				request.clusterId = need.clusterId;
				request.desiredLodIndex = need.desiredLodIndex;
				request.priority = static_cast<std::int32_t>(std::clamp(need.screenImportance, 0.0f, 100000.0f));
				request.screenImportance = need.screenImportance;
				request.fallbackRequired = need.fallbackRequired;
				request.fallbackRequest = fallbackRequest;
				request.payloadSource = source;
				request.expectedEmission = emission;
				request.reason = std::move(reason);
				state.pageRequests.push_back(std::move(request));
			};

			if (need.desiredPayloadSource == VirtualGeometryPagePayloadSource::Missing
				|| need.desiredPayloadSource == VirtualGeometryPagePayloadSource::Unknown)
			{
				appendRequest(
					need.pageId,
					false,
					need.desiredPayloadSource,
					VirtualGeometryEmittedPageSource::HighDetailPage,
					"visible virtual geometry cluster requested a non-resident high-detail page");
			}
			if (need.fallbackRequired
				&& (need.fallbackPayloadSource == VirtualGeometryPagePayloadSource::Missing
					|| need.fallbackPayloadSource == VirtualGeometryPagePayloadSource::Unknown))
			{
				appendRequest(
					need.fallbackPageId,
					true,
					need.fallbackPayloadSource,
					VirtualGeometryEmittedPageSource::FallbackPage,
					"visible virtual geometry cluster requested fallback coverage");
			}
		}
	}

	VirtualGeometryClusterMeshletRange ResolveVirtualGeometryClusterMeshletRange(
		const VirtualGeometryAssetRuntime& asset,
		const VirtualGeometryClusterRuntime& cluster)
	{
		VirtualGeometryClusterMeshletRange range{};
		if (cluster.groupCount == 0u || cluster.firstGroupIndex >= asset.cullableGroups.size())
		{
			return range;
		}

		const std::uint32_t firstGroup = cluster.firstGroupIndex;
		const std::uint32_t lastGroupExclusive = std::min<std::uint32_t>(
			static_cast<std::uint32_t>(asset.cullableGroups.size()),
			firstGroup + cluster.groupCount);
		std::uint32_t minMeshlet = std::numeric_limits<std::uint32_t>::max();
		std::uint32_t maxMeshlet = 0u;
		for (std::uint32_t groupIndex = firstGroup; groupIndex < lastGroupExclusive; ++groupIndex)
		{
			const VirtualGeometryCullableGroupRuntime& group = asset.cullableGroups[groupIndex];
			minMeshlet = std::min(minMeshlet, group.meshletIndex);
			maxMeshlet = std::max(maxMeshlet, group.meshletIndex);
		}

		if (minMeshlet == std::numeric_limits<std::uint32_t>::max() || maxMeshlet < minMeshlet)
		{
			return range;
		}

		// The current task shader accepts one contiguous meshlet range per draw.
		// If a future page layout stores non-contiguous group coverage, dispatch
		// the conservative envelope instead of dropping visible geometry; the
		// selected-cluster packet still records the sparse identity for tooling.
		range.meshletOffset = minMeshlet;
		range.meshletCount = (maxMeshlet - minMeshlet) + 1u;
		range.valid = true;
		return range;
	}

	std::size_t DetailIndexForLod(const VirtualGeometryAssetRuntime& asset, std::uint32_t lodIndex)
	{
		for (std::size_t index = 0u; index < asset.detailLevels.size(); ++index)
		{
			if (asset.detailLevels[index].lodIndex == lodIndex)
			{
				return index;
			}
		}
		return 0u;
	}

	std::size_t ResolveResidentVirtualGeometryDetailIndex(
		const VirtualGeometryAssetRuntime& asset,
		std::size_t desiredIndex,
		std::string& outFallbackReason)
	{
		if (asset.pages.empty() || desiredIndex >= asset.detailLevels.size())
		{
			return desiredIndex;
		}

		const std::uint32_t desiredLod = asset.detailLevels[desiredIndex].lodIndex;
		const GeometryPageRuntime* desiredPage = FindGeometryPageForLod(asset, desiredLod);
		if (desiredPage != nullptr && GeometryPageIsResident(*desiredPage))
		{
			return desiredIndex;
		}

		const std::uint32_t fallbackPageId = desiredPage != nullptr ? desiredPage->fallbackPageId : 0u;
		if (const GeometryPageRuntime* fallbackPage = FindGeometryPageById(asset, fallbackPageId);
			fallbackPage != nullptr && GeometryPageIsResident(*fallbackPage))
		{
			// Missing high-detail pages must fail open to a resident fallback.
			// Returning the fallback detail here preserves visibility while the
			// streaming system catches up or reports a page failure.
			outFallbackReason = "desired virtual geometry page is not resident; using resident fallback page";
			return DetailIndexForLod(asset, fallbackPage->firstLodIndex);
		}

		const GeometryPageRuntime* bestResidentFallback = nullptr;
		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (!GeometryPageIsResident(page) || page.firstLodIndex > desiredLod)
			{
				continue;
			}
			if (bestResidentFallback == nullptr || page.firstLodIndex > bestResidentFallback->firstLodIndex)
			{
				bestResidentFallback = &page;
			}
		}
		if (bestResidentFallback != nullptr)
		{
			outFallbackReason = "desired virtual geometry page is not resident; using best resident fallback page";
			return DetailIndexForLod(asset, bestResidentFallback->firstLodIndex);
		}

		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (page.fallbackPage && GeometryPageIsResident(page))
			{
				outFallbackReason = "desired virtual geometry page is not resident; using first resident fallback page";
				return DetailIndexForLod(asset, page.firstLodIndex);
			}
		}

		outFallbackReason = "desired virtual geometry page is missing and no resident fallback page is published";
		return 0u;
	}

	std::size_t ResolveBestResidentVirtualGeometryDetailIndex(
		const VirtualGeometryAssetRuntime& asset,
		std::string& outFallbackReason)
	{
		if (asset.detailLevels.empty() || asset.pages.empty())
		{
			return 0u;
		}

		// Walk details in cooked order so the highest usable detail wins when
		// virtual geometry is disabled for debugging but resident page metadata
		// is still available. This avoids selecting a non-resident high-detail
		// range that would immediately need another fallback later.
		for (std::size_t detailIndex = 0u; detailIndex < asset.detailLevels.size(); ++detailIndex)
		{
			const std::uint32_t lodIndex = asset.detailLevels[detailIndex].lodIndex;
			if (const GeometryPageRuntime* page = FindGeometryPageForLod(asset, lodIndex);
				page != nullptr && GeometryPageIsResident(*page))
			{
				outFallbackReason = "virtual geometry disabled by quality profile; using best resident detail";
				return detailIndex;
			}
		}

		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (page.fallbackPage && GeometryPageIsResident(page))
			{
				outFallbackReason = "virtual geometry disabled by quality profile; using resident fallback page";
				return DetailIndexForLod(asset, page.firstLodIndex);
			}
		}

		outFallbackReason = "virtual geometry disabled by quality profile; no resident fallback page is published";
		return 0u;
	}

	float EstimateProjectedMeshRadiusPixels(const Mesh& mesh, const glm::mat4& modelMatrix, const glm::vec3& cameraPosition, const glm::uvec2& targetSize)
	{
		if (!mesh.hasLocalBounds || targetSize.x == 0u || targetSize.y == 0u)
		{
			return 512.0f;
		}

		const glm::vec3 localMin(mesh.localBoundsMin[0], mesh.localBoundsMin[1], mesh.localBoundsMin[2]);
		const glm::vec3 localMax(mesh.localBoundsMax[0], mesh.localBoundsMax[1], mesh.localBoundsMax[2]);
		const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
		const float localRadius = glm::length(localMax - localMin) * 0.5f;
		const glm::vec3 worldCenter = glm::vec3(modelMatrix * glm::vec4(localCenter, 1.0f));
		const float worldScale = std::max(
			glm::length(glm::vec3(modelMatrix[0])),
			std::max(glm::length(glm::vec3(modelMatrix[1])), glm::length(glm::vec3(modelMatrix[2]))));
		const float distance = std::max(glm::length(worldCenter - cameraPosition), 1e-3f);
		const float viewportMax = static_cast<float>(std::max(targetSize.x, targetSize.y));
		const float projectedRadius = (localRadius * std::max(worldScale, 1e-6f) / distance) * viewportMax;
		return std::isfinite(projectedRadius) ? projectedRadius : 512.0f;
	}

	float EstimateProjectedClusterRadiusPixels(const VirtualGeometryClusterRuntime& cluster, const glm::mat4& modelMatrix, const glm::vec3& cameraPosition, const glm::uvec2& targetSize, float fallbackProjectedRadiusPixels)
	{
		// Cluster LOD is decided from the cluster's own conservative sphere.
		// When tests call the legacy overload without camera data, fall back to
		// the already-computed object radius so selection remains deterministic.
		if (targetSize.x == 0u || targetSize.y == 0u || cluster.boundsRadius <= 0.0f)
		{
			return fallbackProjectedRadiusPixels;
		}

		const glm::vec3 worldCenter = glm::vec3(modelMatrix * glm::vec4(cluster.boundsCenter, 1.0f));
		const float worldScale = std::max(
			glm::length(glm::vec3(modelMatrix[0])),
			std::max(glm::length(glm::vec3(modelMatrix[1])), glm::length(glm::vec3(modelMatrix[2]))));
		const float distance = std::max(glm::length(worldCenter - cameraPosition), 1e-3f);
		const float viewportMax = static_cast<float>(std::max(targetSize.x, targetSize.y));
		const float projectedRadius = (cluster.boundsRadius * std::max(worldScale, 1e-6f) / distance) * viewportMax;
		return std::isfinite(projectedRadius) ? projectedRadius : fallbackProjectedRadiusPixels;
	}

	std::uint64_t HashFloat(float value)
	{
		std::uint32_t bits = 0u;
		std::memcpy(&bits, &value, sizeof(bits));
		return static_cast<std::uint64_t>(bits);
	}

	void HashCombine(std::uint64_t& seed, std::uint64_t value)
	{
		seed ^= value;
		seed *= kRenderHashPrime;
	}

	void BuildCpuDeformationContentIdentity(
		TerraEngine::Renderer::CpuAnimationDeformationData& deformation) noexcept
	{
		// Hash the exact shader-facing bytes, including counts, with two unrelated
		// update functions. Sharing is admitted only when both values match, making
		// accidental aliasing substantially less likely than a single generation id.
		std::uint64_t low = kRenderHashOffset;
		std::uint64_t high = 0x9e3779b97f4a7c15ULL;
		const auto mixBytes = [&low, &high](const void* source, std::size_t byteCount)
		{
			const auto* bytes = static_cast<const std::uint8_t*>(source);
			for(std::size_t index = 0U; index < byteCount; ++index)
			{
				low = (low ^ bytes[index]) * kRenderHashPrime;
				high ^= static_cast<std::uint64_t>(bytes[index])
					+ 0x9e3779b97f4a7c15ULL + (high << 6U) + (high >> 2U);
			}
		};
		mixBytes(&deformation.jointCount, sizeof(deformation.jointCount));
		if(!deformation.paletteMatrices.empty())
		{
			mixBytes(deformation.paletteMatrices.data(),
				deformation.paletteMatrices.size()
					* sizeof(deformation.paletteMatrices.front()));
		}
		if(!deformation.morphWeights.empty())
		{
			mixBytes(deformation.morphWeights.data(),
				deformation.morphWeights.size() * sizeof(float));
		}
		deformation.contentIdLow = low == 0U ? 1U : low;
		deformation.contentIdHigh = high == 0U ? 1U : high;
	}

	void HashVec3(std::uint64_t& seed, const glm::vec3& value)
	{
		HashCombine(seed, HashFloat(value.x));
		HashCombine(seed, HashFloat(value.y));
		HashCombine(seed, HashFloat(value.z));
	}

	void HashVec4(std::uint64_t& seed, const glm::vec4& value)
	{
		HashCombine(seed, HashFloat(value.x));
		HashCombine(seed, HashFloat(value.y));
		HashCombine(seed, HashFloat(value.z));
		HashCombine(seed, HashFloat(value.w));
	}

	void HashMat4(std::uint64_t& seed, const glm::mat4& value)
	{
		for (int column = 0; column < 4; ++column)
		{
			HashVec4(seed, value[column]);
		}
	}

	void HashUVec2(std::uint64_t& seed, const glm::uvec2& value)
	{
		HashCombine(seed, static_cast<std::uint64_t>(value.x));
		HashCombine(seed, static_cast<std::uint64_t>(value.y));
	}

	void HashQuat(std::uint64_t& seed, const glm::quat& value)
	{
		HashCombine(seed, HashFloat(value.x));
		HashCombine(seed, HashFloat(value.y));
		HashCombine(seed, HashFloat(value.z));
		HashCombine(seed, HashFloat(value.w));
	}

	std::uint64_t BuildDrawCommandSignature(
		const TerraEngine::UUID& meshId,
		const TerraEngine::UUID& materialId,
		const TerraEngine::Scene::WorldPose& transform,
		bool taskShaderCullingEnabled)
	{
		// The cache key covers every RenderItemRecord input that affects model-space
		// or resource binding. Scene View tint is applied later to the visible
		// command list, so it intentionally does not invalidate the base cache.
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId)));
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(materialId)));
		HashCombine(seed, static_cast<std::uint64_t>(
			TerraEngine::Scene::WorldPositionHash{}(transform.position)));
		HashQuat(seed, transform.rotation);
		HashVec3(seed, transform.scale);
		HashCombine(seed, taskShaderCullingEnabled ? 1u : 0u);
		return seed;
	}

	std::uint64_t BuildStableAnimationInstanceId(
		const TerraEngine::Renderer::AnimationRenderBinding& binding,
		const TerraEngine::UUID& objectId)
	{
		// Animator slot plus generation is the same stable value used by animation
		// planning. The object hash is only a nonzero fallback for an explicit safe
		// bind pose that has no live instance identity.
		if(binding.animatorGeneration != 0U
			&& binding.animatorSlotIndex != TerraEngine::Animation::kInvalidAnimatorInstanceSlot) {
			return (static_cast<std::uint64_t>(binding.animatorSlotIndex) << 32U)
				| static_cast<std::uint64_t>(binding.animatorGeneration);
		}
		return std::max<std::uint64_t>(1U,
			static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(objectId)));
	}

	std::uint64_t BuildLightCandidateSignature(
		const TerraEngine::Scene::WorldPose& transform,
		const LightComponent& light)
	{
		// The packed light cache key covers transform and every authoring field
		// used to produce the GPULight payload. The renderer can reuse the
		// previous candidate until one of those inputs changes.
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(
			TerraEngine::Scene::WorldPositionHash{}(transform.position)));
		HashQuat(seed, transform.rotation);
		HashCombine(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(light.type)));
		HashCombine(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(light.unit)));
		HashCombine(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(light.appearance)));
		HashVec3(seed, light.color);
		HashCombine(seed, HashFloat(light.temperature));
		HashCombine(seed, HashFloat(light.intensity));
		HashCombine(seed, HashFloat(light.range));
		HashCombine(seed, HashFloat(light.innerConeAngle));
		HashCombine(seed, HashFloat(light.outerConeAngle));
		HashCombine(seed, HashFloat(light.width));
		HashCombine(seed, HashFloat(light.height));
		HashCombine(seed, light.castsShadows ? 1u : 0u);
		HashCombine(seed, HashFloat(light.shadowStrength));
		HashCombine(seed, HashFloat(light.shadowBias));
		HashCombine(seed, HashFloat(light.indirectMultiplier));
		HashCombine(seed, HashFloat(light.authoredPriority));
		HashCombine(seed, HashFloat(light.stabilityPreference));
		return seed;
	}

	std::uint64_t BuildRenderSettingsSignature(const RenderSettings& settings, QualityPreset preset, bool taskShaderCullingEnabled)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(preset));
		HashCombine(seed, taskShaderCullingEnabled ? 1u : 0u);
		HashCombine(seed, settings.shadowResolution);
		HashCombine(seed, settings.shadowFilteringSize);
		HashCombine(seed, settings.maxUploadedLights);
		HashCombine(seed, settings.maxDynamicLights);
		HashCombine(seed, settings.maxShadowLights);
		HashCombine(seed, settings.maxAreaLights);
		HashCombine(seed, settings.enableLTC);
		HashCombine(seed, settings.enableIBL);
		HashCombine(seed, HashFloat(settings.indirectStrength));
		HashCombine(seed, HashFloat(settings.ambientOcclusionStrength));
		HashCombine(seed, settings.ambientOcclusionQuality);
		HashCombine(seed, settings.anisotropicFiltering);
		HashCombine(seed, settings.variableRateShading);
		// Virtual-geometry debug and quality settings affect the selected meshlet
		// range stored in cached draw commands. Include them in the cache key so
		// editor dropdown changes are visible immediately, without waiting for a
		// camera move to invalidate the view-projection portion of the key.
		HashCombine(seed, BuildVirtualGeometryQualityProfileSignature(settings.virtualGeometry));
		return seed;
	}

	std::uint64_t BuildVisibleCommandCacheKey(
		std::uint64_t sceneRevision,
		std::uint64_t settingsSignature,
		std::uint64_t renderableVisibilitySignature,
		const glm::mat4& viewProjection,
		const glm::uvec2& targetSize,
		RenderOutputKind outputKind,
		RenderViewType viewType,
		std::uint32_t outputId,
		std::uint64_t transitionStateSignature,
		std::uint64_t virtualGeometryResidencyGeneration,
		std::uint64_t rendererResourceGeneration,
		std::uint64_t rendererGeneration)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, sceneRevision);
		HashCombine(seed, settingsSignature);
		// Candidate admission depends on stable geometry plus the small set of
		// animation states that change coarse visibility or owner fallback. Do not
		// include the pose publication generation here: it advances every animated
		// frame even though the same objects, LODs, and geometry routes stay visible.
		HashCombine(seed, renderableVisibilitySignature);
		HashMat4(seed, viewProjection);
		HashUVec2(seed, targetSize);
		// Output kind separates Scene, Game, runtime, shadow, and capture frame
		// identities even when a caller happens to reuse a view type and output id.
		HashCombine(seed, static_cast<std::uint64_t>(outputKind));
		HashCombine(seed, static_cast<std::uint64_t>(viewType));
		HashCombine(seed, outputId);
		HashCombine(seed, transitionStateSignature);
		// Cached page needs and selected fallback clusters are valid only for the
		// physical residency snapshot that produced them. Publication changes the
		// generation at a protected boundary and forces one deterministic rebuild.
		HashCombine(seed, virtualGeometryResidencyGeneration);
		// Conventional and direct draws also retain renderer-owned resources. A
		// successful hot reload must therefore miss this cache even when virtual
		// geometry page residency did not change.
		HashCombine(seed, rendererResourceGeneration);
		// Device recovery publishes a new render-session generation. Cached packets
		// and selection evidence must never cross that protected lifetime boundary.
		HashCombine(seed, rendererGeneration);
		// Selection depends on the camera, output target, topology, resources, and
		// residency above, not on the caller's capture request string. Readback keeps
		// its exact request/frame/renderer/target generations separately, so repeated
		// captures may safely retain this immutable geometry evidence.
		return seed;
	}

	std::uint64_t BuildVirtualGeometryTransitionKey(
		RenderOutputKind outputKind,
		RenderViewType viewType,
		std::uint32_t outputId,
		const glm::uvec2& targetSize,
		const TerraEngine::UUID& objectId,
		const TerraEngine::UUID& meshId)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(outputKind));
		HashCombine(seed, static_cast<std::uint64_t>(viewType));
		HashCombine(seed, outputId);
		HashCombine(seed, (static_cast<std::uint64_t>(targetSize.x) << 32u)
			| targetSize.y);
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(objectId)));
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId)));
		return seed;
	}

	std::uint64_t BuildVirtualGeometryDebugModeKey(RenderViewType viewType, std::uint32_t outputId)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(viewType));
		HashCombine(seed, outputId);
		return seed;
	}

	std::uint64_t BuildVirtualGeometrySelectionSignature(const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult& selection)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, selection.range.selectedLodIndex);
		HashCombine(seed, selection.range.meshletOffset);
		HashCombine(seed, selection.range.meshletCount);
		HashCombine(seed, selection.range.virtualGeometryActive ? 1u : 0u);
		HashCombine(seed, selection.range.failOpen ? 1u : 0u);
		HashCombine(seed, selection.range.pageId);
		HashCombine(seed, selection.range.fallbackPageId);
		HashCombine(seed, static_cast<std::uint64_t>(selection.range.payloadSource));
		HashCombine(seed, static_cast<std::uint64_t>(selection.range.emittedSource));
		HashCombine(seed, selection.range.pagePayloadByteOffset);
		HashCombine(seed, selection.range.pagePayloadByteSize);
		HashCombine(seed, selection.invalidSelectionReasons);
		HashCombine(seed, static_cast<std::uint64_t>(selection.selectedClusters.size()));
		for (const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& cluster : selection.selectedClusters)
		{
			HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(cluster.objectId)));
			HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(cluster.meshId)));
			HashCombine(seed, cluster.clusterId);
			HashCombine(seed, cluster.lodIndex);
			HashCombine(seed, cluster.pageId);
			HashCombine(seed, cluster.fallbackClusterId);
			HashCombine(seed, cluster.usingFallback ? 1u : 0u);
		}
		HashCombine(seed, static_cast<std::uint64_t>(selection.pageNeeds.size()));
		for (const TerraEngine::Renderer::VirtualGeometryPageNeedRecord& need : selection.pageNeeds)
		{
			HashCombine(seed, need.pageId);
			HashCombine(seed, need.clusterId);
			HashCombine(seed, need.desiredLodIndex);
			HashCombine(seed, need.fallbackRequired ? 1u : 0u);
		}
		return seed;
	}

	struct VirtualGeometryTransitionMetrics
	{
		float candidateMaxErrorPixels = 0.0f;
		float transitionMarginPixels = std::numeric_limits<float>::max();
		std::uint32_t transitionCandidateCount = 0u;
	};

	VirtualGeometryTransitionMetrics EvaluateVirtualGeometryTransitionMetrics(
		const VirtualGeometryAssetRuntime& asset,
		const VirtualGeometryQualityProfile& quality,
		float projectedRadiusPixels,
		const glm::mat4& modelMatrix,
		const glm::vec3& cameraPosition,
		const glm::uvec2& targetSize)
	{
		VirtualGeometryTransitionMetrics metrics;
		const float finiteProjectedRadiusPixels = std::isfinite(projectedRadiusPixels)
			? std::max(projectedRadiusPixels, 0.0f)
			: 512.0f;
		for (const VirtualGeometryClusterRuntime& cluster : asset.clusters)
		{
			if (cluster.childCount == 0u)
			{
				continue;
			}
			const float clusterProjectedRadiusPixels = EstimateProjectedClusterRadiusPixels(cluster, modelMatrix, cameraPosition, targetSize, finiteProjectedRadiusPixels);
			const float errorPixels = clusterProjectedRadiusPixels
				* std::max(cluster.geometricError, 0.0f)
				* quality.lodErrorScale;
			if (!std::isfinite(errorPixels))
			{
				continue;
			}
			metrics.candidateMaxErrorPixels = std::max(metrics.candidateMaxErrorPixels, errorPixels);
			metrics.transitionMarginPixels = std::min(metrics.transitionMarginPixels, std::abs(errorPixels - kVirtualGeometryClusterRefinementErrorPixels));
			++metrics.transitionCandidateCount;
		}
		if (metrics.transitionCandidateCount == 0u)
		{
			metrics.transitionMarginPixels = 0.0f;
		}
		return metrics;
	}

	std::uint64_t BuildVirtualGeometryTransitionStateSignature(
		const std::unordered_map<std::uint64_t, TerraEngine::Renderer::VirtualGeometryClusterSelectionResult>& transitionCache,
		RenderOutputKind outputKind,
		RenderViewType viewType,
		std::uint32_t outputId)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(outputKind));
		HashCombine(seed, static_cast<std::uint64_t>(viewType));
		HashCombine(seed, outputId);
		for (const auto& [key, selection] : transitionCache)
		{
			const VirtualGeometryLODTransitionState& state = selection.transitionState;
			if (!state.valid || state.viewType != viewType || state.outputId != outputId)
			{
				continue;
			}
			HashCombine(seed, key);
			HashCombine(seed, state.lastStableSelectionSignature);
			HashCombine(seed, state.candidateSelectionSignature);
			HashCombine(seed, state.stableFrameCount);
			HashCombine(seed, state.heldFrameCount);
			HashCombine(seed, state.heldPreviousSelection ? 1u : 0u);
			HashCombine(seed, state.resetThisFrame ? 1u : 0u);
		}
		return seed;
	}

	void PruneStaleVirtualGeometryTransitionCache(
		std::unordered_map<std::uint64_t, TerraEngine::Renderer::VirtualGeometryClusterSelectionResult>& transitionCache,
		const std::unordered_set<TerraEngine::UUID>& aliveObjectIds)
	{
		for (auto iterator = transitionCache.begin(); iterator != transitionCache.end();)
		{
			const VirtualGeometryLODTransitionState& state = iterator->second.transitionState;
			if (!state.valid || !aliveObjectIds.contains(state.objectId))
			{
				iterator = transitionCache.erase(iterator);
			}
			else
			{
				++iterator;
			}
		}
	}

	std::uint64_t BuildLightSetSignature(std::span<const FrameLightCandidate> lights)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(lights.size()));
		for (const FrameLightCandidate& light : lights)
		{
			HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(light.lightId)));
			HashVec4(seed, light.gpuLight.positionAndType);
			HashVec4(seed, light.gpuLight.directionAndRange);
			HashVec4(seed, light.gpuLight.colorAndIntensity);
			HashVec4(seed, light.gpuLight.coneAnglesAndArea);
			HashVec4(seed, light.gpuLight.extraParams);
			HashCombine(seed, light.candidateFlags);
		}
		return seed;
	}

	std::uint64_t BuildShadowDrawListCacheKey(
		std::uint64_t sceneRevision,
		std::uint64_t settingsSignature,
		std::uint64_t renderableWorldSignature,
		std::uint64_t lightSetSignature,
		const glm::vec3& cameraLocalPosition)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, sceneRevision);
		HashCombine(seed, settingsSignature);
		HashCombine(seed, renderableWorldSignature);
		HashCombine(seed, lightSetSignature);
		HashVec3(seed, cameraLocalPosition);
		return seed;
	}

	void PruneStaleDrawCommandCache(DrawCommandCacheState& cache, const std::unordered_set<TerraEngine::UUID>& aliveObjectIds)
	{
		for (auto iterator = cache.recordsByObjectId.begin(); iterator != cache.recordsByObjectId.end();)
		{
			iterator = aliveObjectIds.contains(iterator->first)
				? std::next(iterator)
				: cache.recordsByObjectId.erase(iterator);
		}
	}

	void PruneStaleLightCandidateCache(LightCandidateCacheState& cache, const std::unordered_set<TerraEngine::UUID>& aliveLightIds)
	{
		for (auto iterator = cache.recordsByLightId.begin(); iterator != cache.recordsByLightId.end();)
		{
			iterator = aliveLightIds.contains(iterator->first)
				? std::next(iterator)
				: cache.recordsByLightId.erase(iterator);
		}
	}

	template<typename Cache>
	void TrimSignatureCache(Cache& cache, std::size_t limit)
	{
		while (cache.recordsBySignature.size() > limit)
		{
			cache.recordsBySignature.erase(cache.recordsBySignature.begin());
		}
	}

	template<typename Fn>
	void RunRenderChunkedWork(std::size_t itemCount, std::size_t workerLimit, std::size_t parallelThreshold, const char* taskLabel, Fn&& work)
	{
		if (itemCount == 0u)
		{
			return;
		}
		const std::size_t workerCount = std::min(itemCount, std::max<std::size_t>(1u, workerLimit));
		if (workerCount <= 1u || itemCount < parallelThreshold)
		{
			work(0u, itemCount);
			return;
		}

		const std::size_t chunkSize = (itemCount + workerCount - 1u) / workerCount;
		std::vector<std::future<void>> futures;
		futures.reserve(workerCount - 1U);
		// Children borrow the callback and its immutable input spans. Settle every
		// admitted child before an inline, queue, or result exception can unwind them.
		TerraEngine::Tasks::ScopedTaskSettlement<void> settlement(futures);
		// This helper is called from an existing scheduler worker. Queue every
		// remaining disjoint range, then execute range zero on the current worker so
		// large renderer batches do not pay for an avoidable future and handoff.
		for (std::size_t taskIndex = 1u; taskIndex < workerCount; ++taskIndex)
		{
			const std::size_t begin = taskIndex * chunkSize;
			if (begin >= itemCount)
			{
				break;
			}
			const std::size_t end = std::min(itemCount, begin + chunkSize);
			settlement.CreateAndEnqueue(ThreadPool::instance(), [begin, end, taskLabel, &work]() {
				TERRAENGINE_PROFILE_DYNAMIC_SCOPE(taskLabel, TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
				work(begin, end);
			});
		}
		{
			TERRAENGINE_PROFILE_DYNAMIC_SCOPE(taskLabel,
				TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
			settlement.RunInline([&]() { work(0U, std::min(itemCount, chunkSize)); });
		}
		settlement.GetAll();
	}

	// ResolveRuntimeSettingsDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path ResolveRuntimeSettingsDirectory()
	{
		auto& resourceManager = ResourceManager::Instance();
		// Exported builds keep writable settings beside the packaged runtime,
		// while editor/project sessions must write next to Assets so authored
		// ProjectSettings stay in <project>/ProjectSettings.
		if (resourceManager.IsExportRuntime())
		{
			return resourceManager.GetAssetRoot() / "Settings";
		}

		return resourceManager.GetProjectRoot() / "ProjectSettings";
	}

	// ResolveLegacyAssetSettingsPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path ResolveLegacyAssetSettingsPath()
	{
		auto& resourceManager = ResourceManager::Instance();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (resourceManager.IsExportRuntime())
		{
			return {};
		}

		// Older builds incorrectly wrote render settings inside Assets. Loading
		// that file as a fallback preserves a user's settings once, and the next
		// save writes them back to the corrected project-level directory.
		return resourceManager.GetAssetRoot() / "ProjectSettings" / "RenderSettings.json";
	}

	// SanitizeRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	RenderSettings SanitizeRenderSettings(const RenderSettings& input)
	{
		RenderSettings sanitized = input;
		sanitized.maxUploadedLights = std::max(sanitized.maxUploadedLights, 1u);
		sanitized.maxDynamicLights = std::min(sanitized.maxDynamicLights, sanitized.maxUploadedLights);
		sanitized.maxAreaLights = std::min(sanitized.maxAreaLights, sanitized.maxDynamicLights);
		sanitized.maxShadowLights = std::min(sanitized.maxShadowLights, sanitized.maxDynamicLights);
		// Shadow maps are allocated lazily by the Vulkan backend, so authoring
		// budgets may exceed four lights without paying for unused slots.
		sanitized.maxShadowLights = std::min(sanitized.maxShadowLights, kMaxShadowResourceSlots);
		sanitized.indirectStrength = std::clamp(sanitized.indirectStrength, 0.0f, 8.0f);
		sanitized.ambientOcclusionStrength = std::clamp(sanitized.ambientOcclusionStrength, 0.0f, 4.0f);
		sanitized.ambientOcclusionQuality = std::min(sanitized.ambientOcclusionQuality, 3u);
		sanitized.enableIBL = sanitized.enableIBL > 0u ? 1u : 0u;
		sanitized.variableRateShading = sanitized.variableRateShading > 0u ? 1u : 0u;
		sanitized.validationEnabled = sanitized.validationEnabled > 0u ? 1u : 0u;
		sanitized.debugEnabled = sanitized.debugEnabled > 0u ? 1u : 0u;
		sanitized.virtualGeometry = SanitizeVirtualGeometryQualityProfile(sanitized.virtualGeometry);
		return sanitized;
	}

	TerraEngine::Scheduling::EcsComponentQueryDeclaration MakeProtectedRenderQuery(
		std::string queryName,
		bool includeDiagnosticsMetadata,
		std::initializer_list<const char*> reads,
		std::initializer_list<const char*> readResources,
		std::initializer_list<const char*> writeResources,
		TerraEngine::Scheduling::EcsQueryChunkWork work)
	{
		TerraEngine::Scheduling::EcsComponentQueryDeclaration query;
		query.queryName = std::move(queryName);
		if (includeDiagnosticsMetadata)
		{
			query.access.ownerSystem = "RenderSystem";
			query.access.queryName = query.queryName;
			query.access.readComponents.assign(reads.begin(), reads.end());
			query.access.readResources.assign(readResources.begin(), readResources.end());
			query.access.writeResources.assign(writeResources.begin(), writeResources.end());
			query.access.workerEligible = false;
			query.access.mainThreadReason = "Renderer collection reads live EnTT hierarchy/component storage, protected resources, and frame allocator containers before publishing immutable worker inputs.";
		}
		query.preserveMatchingObjectOrder = true;
		query.copyObjectIdsIntoChunks = false;
		query.runOnceWithoutObjectSnapshot = true;
		query.work = std::move(work);
		return query;
	}

	Transform ReflectTransformComponent(const Transform& transform)
	{
		// Render collection runs as protected main-thread work after scripting,
		// so it copies the canonical pose and bounded parent-relative pose directly. Avoiding the
		// reflection getter here removes repeated property lookup, variant
		// wrapping, and shared-lock overhead from every visible object each
		// frame.
		return transform;
	}

	// SafeNormalizeDirection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 SafeNormalizeDirection(const glm::vec3& direction, const glm::vec3& fallback)
	{
		if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z))
		{
			return fallback;
		}

		const float length = glm::length(direction);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (length <= 0.0001f)
		{
			return fallback;
		}

		return direction / length;
	}

	// ApplySkyYawOffset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 ApplySkyYawOffset(const glm::vec3& direction, float degrees)
	{
		const glm::quat yawRotation = glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
		return SafeNormalizeDirection(yawRotation * direction, kSkyReferenceSunDirection);
	}

	// SafeNormalizeShadowDirection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 SafeNormalizeShadowDirection(const glm::vec3& value, const glm::vec3& fallback)
	{
		const float length = glm::length(value);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::isfinite(length) || length <= 0.00001f)
		{
			return fallback;
		}

		return value / length;
	}

	// ComputeDirectionalShadowCenter is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 ComputeDirectionalShadowCenter(const GPULight& light, const glm::vec3& cameraLocalPosition)
	{
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float texelWorldSize = (kDirectionalShadowExtent * 2.0f) / 1024.0f;

		// Matching the Vulkan light-space snap keeps CPU culling aligned with the
		// exact shadow map coverage that the GPU will render.
		const glm::vec3 provisionalLightPosition = cameraLocalPosition - lightDirection * 60.0f;
		const glm::mat4 provisionalView = glm::lookAtRH(provisionalLightPosition, cameraLocalPosition, worldUp);
		glm::vec4 snappedCenterLightSpace = provisionalView * glm::vec4(cameraLocalPosition, 1.0f);
		snappedCenterLightSpace.x = std::floor(snappedCenterLightSpace.x / texelWorldSize) * texelWorldSize;
		snappedCenterLightSpace.y = std::floor(snappedCenterLightSpace.y / texelWorldSize) * texelWorldSize;
		return glm::vec3(glm::inverse(provisionalView) * snappedCenterLightSpace);
	}

	// BuildDirectionalShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildDirectionalShadowViewProjection(const GPULight& light, const glm::vec3& cameraLocalPosition)
	{
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 snappedCenterWorld = ComputeDirectionalShadowCenter(light, cameraLocalPosition);
		const glm::vec3 lightPosition = snappedCenterWorld - lightDirection * 60.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, snappedCenterWorld, worldUp);
		glm::mat4 projection = glm::orthoRH_ZO(-kDirectionalShadowExtent, kDirectionalShadowExtent, -kDirectionalShadowExtent, kDirectionalShadowExtent, kDirectionalShadowNear, kDirectionalShadowFar);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	// BuildSpotShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildSpotShadowViewProjection(const GPULight& light)
	{
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const float outerConeCos = std::clamp(light.coneAnglesAndArea.y, -0.999f, 0.999f);
		const float outerAngleRadians = std::max(0.1f, std::acos(outerConeCos) * 2.0f);
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + lightDirection, worldUp);
		glm::mat4 projection = glm::perspectiveRH_ZO(outerAngleRadians, 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	// BuildAreaShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildAreaShadowViewProjection(const GPULight& light)
	{
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const float width = std::max(light.coneAnglesAndArea.z, 0.5f);
		const float height = std::max(light.coneAnglesAndArea.w, 0.5f);
		const float fov = std::atan2(std::max(width, height), range) * 2.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + lightDirection, worldUp);
		glm::mat4 projection = glm::perspectiveRH_ZO(fov, 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

}

namespace {
	// A nonblocking admission gate also detects same-thread reentry. Its owner
	// lives on RenderSystem, so replacing per-scene state cannot replace the gate.
	class RenderExecutionBoundary final {
	public:
		explicit RenderExecutionBoundary(std::atomic_flag& flag) : flag_(flag) {
			if(flag_.test_and_set(std::memory_order_acquire))
				throw std::logic_error("Render frame execution is already active");
		}
		~RenderExecutionBoundary() { flag_.clear(std::memory_order_release); }
		RenderExecutionBoundary(const RenderExecutionBoundary&) = delete;
		RenderExecutionBoundary& operator=(const RenderExecutionBoundary&) = delete;
	private:
		std::atomic_flag& flag_;
	};
}

// A render-frame coroutine keeps stack-owned immutable preparation state alive
// between scheduler callbacks. It never schedules itself: the scheduler resumes
// it at declared protected/worker boundaries, preserving normal exception flow.
struct RenderFrameExecution
{
	struct promise_type
	{
		std::exception_ptr failure;

		RenderFrameExecution get_return_object()
		{
			return RenderFrameExecution{
				std::coroutine_handle<promise_type>::from_promise(*this)};
		}
		std::suspend_always initial_suspend() const noexcept { return {}; }
		std::suspend_always final_suspend() const noexcept { return {}; }
		void return_void() const noexcept {}
		void unhandled_exception() noexcept { failure = std::current_exception(); }
	};

	using Handle = std::coroutine_handle<promise_type>;
	Handle handle{};

	RenderFrameExecution() = default;
	explicit RenderFrameExecution(Handle value) : handle(value) {}
	RenderFrameExecution(const RenderFrameExecution&) = delete;
	RenderFrameExecution& operator=(const RenderFrameExecution&) = delete;
	RenderFrameExecution(RenderFrameExecution&& other) noexcept
		: handle(std::exchange(other.handle, {})) {}
	RenderFrameExecution& operator=(RenderFrameExecution&& other) noexcept
	{
		if(this == &other) return *this;
		if(handle) handle.destroy();
		handle = std::exchange(other.handle, {});
		return *this;
	}
	~RenderFrameExecution()
	{
		if(handle) handle.destroy();
	}

	// Returns true when another scheduler stage remains. Exceptions are rethrown
	// at the owning callback so the scheduler reports the correct failed phase.
	bool Resume()
	{
		if(!handle || handle.done()) return false;
		handle.resume();
		if(handle.promise().failure) std::rethrow_exception(handle.promise().failure);
		return !handle.done();
	}
};

// One implementation-owned aggregate replaces every former function-static
// cache. Project switches and renderer shutdown can now release all retained
// roots and scratch capacity together without leaking state across sessions.
struct RenderSystemLifetimeState
{
	struct FramePhaseState
	{
		float deltaTime = 0.0F;
		std::uint64_t captureSequence = 0U;
		std::unique_ptr<RenderFrameExecution> execution;
		bool captureReady = false;
		bool routeReady = false;
		bool virtualGeometryReady = false;
		bool backendComplete = false;
		bool publicationComplete = false;
	};
	// Scheduler callbacks may cross worker and protected executors. This mutex
	// guards the small immutable-root handoff only; no live ECS or GPU work runs
	// while it is held.
	std::mutex framePhaseMutex;
	// Declared before the continuation: reverse destruction retires its vectors
	// and strong lease before this lifetime can release the stable scratch owner.
	std::shared_ptr<TerraEngine::Renderer::RenderFrameScratch> frameScratch;
	FramePhaseState framePhase;
	RetainedRenderTopologyState retainedRenderTopology;
	GroupedSubmissionMembershipCacheState groupedSubmissionMembershipCache;
	TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache rendererPreparationCache;
	VisibleCommandCacheState visibleCommandCache;
	ShadowDrawListCacheState shadowDrawListCache;
	SpatialPreparationCacheState spatialPreparationCache;
	RenderableDrawWorkingSet renderableDrawWorkingSet;
	DrawCommandCacheState drawCommandCache;
	RenderableFrameIdentityCacheState renderableFrameIdentityCache;
	LightCandidateCacheState lightCandidateCache;
	// View commands are fully lowered into immutable selection and presentation
	// records before the next view starts. Retaining this temporary vector avoids
	// allocating crowd-sized command storage again for every view and frame.
	std::vector<RenderItemRecord> viewCommandScratch;
};

// RenderSystem::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderSystem& RenderSystem::Instance()
{
	// Provide a singleton instance so the main engine loop can register the
	// system without managing heap allocations or ownership semantics.
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem()
	// objManager is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: lifetimeState(std::make_unique<RenderSystemLifetimeState>()),
	objManager(ObjectManager::instance())
{
	latestGameStats.viewType = RenderViewType::Game;
	latestSceneStats.viewType = RenderViewType::Scene;
	latestGameBudgetDiagnostics.viewType = RenderViewType::Game;
	latestSceneBudgetDiagnostics.viewType = RenderViewType::Scene;
	latestGameLightingDiagnostics.viewType = RenderViewType::Game;
	latestSceneLightingDiagnostics.viewType = RenderViewType::Scene;
	latestRendererRecoverySnapshot = TerraEngine::Renderer::MakeActiveRecoverySnapshot();
}

RenderSystem::~RenderSystem() = default;

void RenderSystem::SetSceneViewContextTint(const std::unordered_set<TerraEngine::UUID>& focusedObjectIds)
{
	// The editor calls this once per Scene View frame while prefab context mode
	// is active. An empty set disables the cue so stale selections do not tint
	// unrelated views on later frames.
	sceneViewContextFocusedObjectIds = focusedObjectIds;
	sceneViewContextTintEnabled = !sceneViewContextFocusedObjectIds.empty();
}

void RenderSystem::SetRendererRecoveryPolicy(const TerraEngine::Renderer::RendererRecoveryPolicy& policy)
{
	rendererRecoveryPolicy = policy;
}

TerraEngine::Renderer::RendererRecoveryPolicy RenderSystem::GetRendererRecoveryPolicy() const
{
	return rendererRecoveryPolicy;
}

void RenderSystem::ReportFatalRendererRecoveryFailure(TerraEngine::Renderer::RendererRecoverySnapshot snapshot)
{
	if (!rhi || !snapshot.fatalDialogReportId.empty())
	{
		return;
	}

	const bool exportRuntime = ResourceManager::Instance().IsExportRuntime();
	TerraEngine::FatalErrorDialogReport report = TerraEngine::BuildFatalErrorDialogReport(
		exportRuntime ? TerraEngine::FatalErrorCategory::ExportFatal : TerraEngine::FatalErrorCategory::RendererRecoveryFailed,
		rendererRecoveryPolicy.fatalMessage,
		std::string("In-process Vulkan device recreation is unavailable after graphics device loss. First failure stage=")
			+ TerraEngine::Renderer::ToString(snapshot.firstFailureStage)
			+ ", result="
			+ snapshot.firstFailureResult
			+ ", lastError="
			+ snapshot.lastError);
	report.incidentId = snapshot.incidentId;
	report.recoverySessionId = snapshot.sessionId;
	const std::string crashProductName = exportRuntime
		? ResourceManager::Instance().GetProductName()
		: std::string(TerraEngine::kEditorFatalErrorProductName);
	report = TerraEngine::PlatformUtils::WriteCrashDumpForReport(report, crashProductName);

	TerraEngine::Renderer::RendererRecoverySnapshot reported = TerraEngine::Renderer::AttachFatalDialogReport(
		snapshot,
		report.reportId,
		report.crashDumpPath.generic_string(),
		report.crashSummaryPath.generic_string(),
		TerraEngine::ToString(report.crashDumpStatus));
	reported.fatalDialogShown = true;
	if (exportRuntime)
	{
		reported.state = TerraEngine::Renderer::RendererRecoveryStateKind::FatalExportFailure;
		reported.finalOutcome = TerraEngine::Renderer::RendererRecoveryOutcome::FatalExportFailure;
	}
	rhi->PublishRendererRecoverySnapshot(reported);
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRendererRecoverySnapshot = reported;
	}
	// The platform dialog returns only after OK is acknowledged, then exits with
	// a failure code without entering Vulkan's ordinary wait-idle teardown.
	WindowSystem::Instance().ShowFatalErrorDialog(report);
}

bool RenderSystem::RequestsEngineShutdown() const
{
	// Editors keep their process alive so their renderer-recovery UI can remain
	// available. A packaged game instead terminates after its fatal dialog has
	// published the final incident report and crash-dump paths.
	if (!initialized || !ResourceManager::Instance().IsExportRuntime())
	{
		return false;
	}

	const TerraEngine::Renderer::RendererRecoverySnapshot recovery = GetLatestRendererRecoverySnapshot();
	if (recovery.state != TerraEngine::Renderer::RendererRecoveryStateKind::FatalExportFailure)
	{
		return false;
	}

	TERRAENGINE_LOG(LogLevel::Critical, "Export runtime stopping after graphics device loss. incident="
		<< recovery.incidentId
		<< " report="
		<< recovery.fatalDialogReportId
		<< " dump="
		<< recovery.crashDumpPath);
	return true;
}

std::vector<TerraEngine::Renderer::Terrain::TerrainPreparedRenderPacket>
RenderSystem::PrepareCommittedTerrainPackets(
	std::span<const TerraEngine::Renderer::Terrain::TerrainViewPacket> views,
	std::span<const TerraEngine::Renderer::Terrain::TerrainPageCookResult> cooks,
	std::span<const TerraEngine::Resource::VirtualGeometryPagePublication> committedPages)
{
	std::vector<TerraEngine::Renderer::Terrain::TerrainPreparedRenderPacket> packets;
	packets.reserve(views.size());
	const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication> committedSnapshot(
		committedPages.begin(), committedPages.end());
	for (const TerraEngine::Renderer::Terrain::TerrainViewPacket& view : views)
	{
		// A terrain view carries the authored root identity captured by the
		// protected terrain snapshot. Select only a cook with the same identity;
		// this prevents an old worker result from being mixed into a new frame.
		const auto cook = std::find_if(cooks.begin(), cooks.end(), [&view](const auto& candidate)
		{
			return candidate.assetId == view.assetId
				&& candidate.area == view.area
				&& candidate.authoredHash == view.authoredHash
				&& candidate.authoredGeneration == view.authoredGeneration;
		});
		if (cook != cooks.end())
		{
			if (const auto prepared = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, *cook, committedSnapshot))
				packets.push_back(*prepared);
			continue;
		}

		// No current cook exists. Pass an intentionally incomplete value to the
		// packet helper, which can select a matching committed last-known-good page
		// but will not fabricate a fresh candidate from stale data.
		TerraEngine::Renderer::Terrain::TerrainPageCookResult missingCook{};
		if (const auto fallback = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, missingCook, committedSnapshot))
			packets.push_back(*fallback);
	}
	return packets;
}

TerraEngine::Renderer::Terrain::TerrainAreaRenderPacketResult
RenderSystem::PrepareCommittedTerrainAreaPackets(
	const TerraEngine::Renderer::Terrain::TerrainAssetRenderSnapshot& snapshot,
	std::span<const TerraEngine::Renderer::Terrain::TerrainAreaRenderRequest> requests,
	std::span<const TerraEngine::Renderer::Terrain::TerrainPageResidencyEntry> residency)
{
	// The pure packet builder owns route parity, own-ancestor fallback, page
	// demand coalescing, and all-or-nothing cut admission. Keeping this wrapper on
	// RenderSystem makes the production frame path and focused tests share it.
	return TerraEngine::Renderer::Terrain::PrepareTerrainAreaRenderPackets(
		snapshot, requests, residency);
}

bool RenderSystem::SubmitPreparedTerrainPackets(
	std::span<const TerraEngine::Renderer::Terrain::TerrainPreparedRenderPacket> packets)
{
	if (packets.empty()) return true;
	// One batch is intentionally limited to one exact view/source/generation.
	// Callers split multi-view LOD choices before this boundary, preserving each
	// camera's independent cut while still giving VG a complete typed producer.
	const auto& first = packets.front();
	TerraEngine::Resource::TerrainVirtualGeometryProducerBatch batch{};
	batch.assetId = first.view.assetId;
	batch.sourceIdentity = first.view.pageSourceIdentity;
	batch.authoredGeneration = first.view.authoredGeneration;
	batch.viewId = first.view.viewId;
	batch.regionalRootPageId = first.view.pageId;
	batch.candidates.reserve(packets.size());
	for (const auto& packet : packets)
	{
		if (packet.usesLastKnownGood || packet.view.assetId != batch.assetId
			|| packet.view.pageSourceIdentity != batch.sourceIdentity
			|| packet.view.authoredGeneration != batch.authoredGeneration
			|| packet.view.viewId != batch.viewId) return false;
		batch.regionalRootPageId = std::min(batch.regionalRootPageId, packet.view.pageId);
		batch.candidates.push_back(packet.candidate);
	}
	return TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance().SubmitTerrainProducerBatch(std::move(batch));
}

bool RenderSystem::IsDirectMeshletRouteEligible(const Mesh& mesh)
{
	// Keep diagnostics on the exact production decision. A parallel terrain-only
	// approximation could pass tests while the frame router still rejects draws.
	return BuildCanonicalGeometryRouteFacts(mesh).directUsable;
}

std::vector<VirtualGeometryClusterPacket> RenderSystem::BuildVirtualGeometryClusterPackets(
	const VirtualGeometryAssetRuntime& asset,
	const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters,
	const TerraEngine::UUID& materialId,
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint32_t packetIdBase)
{
	std::vector<VirtualGeometryClusterPacket> packets;
	packets.reserve(selectedClusters.size());
	for (const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& selectedCluster : selectedClusters)
	{
		const VirtualGeometryClusterRuntime* cluster = FindVirtualGeometryClusterById(asset, selectedCluster.clusterId);
		if (cluster == nullptr)
		{
			continue;
		}

		const VirtualGeometryClusterMeshletRange meshlets = ResolveVirtualGeometryClusterMeshletRange(asset, *cluster);
		if (!meshlets.valid)
		{
			continue;
		}

		VirtualGeometryClusterPacket packet;
		packet.packetId = packetIdBase + static_cast<std::uint32_t>(packets.size());
		packet.viewId = viewId;
		packet.viewType = viewType;
		packet.objectId = selectedCluster.objectId;
		packet.meshId = selectedCluster.meshId;
		packet.materialId = materialId;
		packet.selectedLod = cluster->lodIndex;
		packet.clusterId = cluster->clusterId;
		packet.meshletOffset = meshlets.meshletOffset;
		packet.meshletCount = meshlets.meshletCount;
		const auto [boundsMin, boundsMax] = ResolveVirtualGeometryHiZPacketBounds(*cluster);
		packet.boundsMin = boundsMin;
		packet.boundsMax = boundsMax;
		packet.pageId = selectedCluster.pageId;
		packet.fallbackPacket = selectedCluster.usingFallback;
		const GeometryPageRuntime* page = FindGeometryPageById(asset, cluster->pageId);
		packet.fallbackPageId = page != nullptr ? page->fallbackPageId : (selectedCluster.usingFallback ? cluster->pageId : 0u);
		packet.payloadSource = ResolveVirtualGeometryPayloadSource(page, packet.fallbackPacket);
		packet.emittedSource = ResolveVirtualGeometryEmittedSource(packet.payloadSource);
		packet.pagePayloadByteOffset = page != nullptr ? page->payloadByteOffset : 0u;
		packet.pagePayloadByteSize = ResolveVirtualGeometryPayloadBytes(page, meshlets);
		packet.payloadBufferIndex = page != nullptr ? page->payloadBufferIndex : 0xffffffffu;
		packet.payloadDescriptorIndex = page != nullptr ? page->payloadDescriptorIndex : 0xffffffffu;

		// The debug identity is deliberately stable and source-data based. It
		// lets visualizers distinguish packet/batch ownership without depending
		// on pointer addresses or frame-local vector storage.
		std::uint64_t debugHash = kRenderHashOffset;
		HashCombine(debugHash, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(packet.objectId)));
		HashCombine(debugHash, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(packet.meshId)));
		HashCombine(debugHash, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(packet.materialId)));
		HashCombine(debugHash, packet.clusterId);
		HashCombine(debugHash, packet.selectedLod);
		HashCombine(debugHash, packet.meshletOffset);
		packet.debugIdentity = static_cast<std::uint32_t>(debugHash ^ (debugHash >> 32u));
		packets.push_back(packet);
	}
	return packets;
}

std::vector<ClusterWorkBatch> RenderSystem::BuildVirtualGeometryClusterBatches(
	const std::vector<VirtualGeometryClusterPacket>& packets,
	std::uint32_t packetOffsetBase,
	std::uint32_t batchIdBase)
{
	std::vector<ClusterWorkBatch> batches;
	if (packets.empty())
	{
		return batches;
	}

	auto canMergePacket = [](const VirtualGeometryClusterPacket& previous, const VirtualGeometryClusterPacket& next) {
		const auto isPageLocalSource = [](VirtualGeometryPagePayloadSource source) {
			return source == VirtualGeometryPagePayloadSource::ResidentPagePayload
				|| source == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		};
		const bool pageLocalRun = isPageLocalSource(previous.payloadSource)
			&& isPageLocalSource(next.payloadSource);
		const bool compatibleOwner =
			previous.viewId == next.viewId
			&& previous.viewType == next.viewType
			&& previous.objectId == next.objectId
			&& previous.meshId == next.meshId
			&& previous.materialId == next.materialId
			&& previous.selectedLod == next.selectedLod
			&& previous.payloadSource == next.payloadSource
			&& previous.emittedSource == next.emittedSource
			&& previous.fallbackPacket == next.fallbackPacket
			// Raster route and shading-bin policy are protected-boundary
			// execution state. A CPU-expanded batch must not span a route
			// transition that the GPU descriptor path intentionally splits.
			&& previous.rasterRoute == next.rasterRoute
			&& previous.materialBin == next.materialBin
			&& previous.packetId + 1u == next.packetId;
		// Embedded draws still share one payload binding for their entire run.
		// Page-local draws carry one descriptor id per emitted meshlet, so adjacent
		// packets may safely cross page boundaries without multiplying CPU draws.
		const bool compatiblePayload = pageLocalRun
			|| (previous.pageId == next.pageId
				&& previous.fallbackPageId == next.fallbackPageId
				&& previous.pagePayloadByteOffset == next.pagePayloadByteOffset
				&& previous.pagePayloadByteSize == next.pagePayloadByteSize
				&& previous.payloadBufferIndex == next.payloadBufferIndex
				&& previous.payloadDescriptorIndex == next.payloadDescriptorIndex);
		const bool contiguousMeshlets = previous.meshletOffset + previous.meshletCount == next.meshletOffset;
		return compatibleOwner && compatiblePayload && contiguousMeshlets;
	};

	auto startBatch = [&](std::size_t packetIndex) {
		const VirtualGeometryClusterPacket& packet = packets[packetIndex];
		ClusterWorkBatch batch;
		batch.batchId = batchIdBase + static_cast<std::uint32_t>(batches.size());
		batch.viewId = packet.viewId;
		batch.viewType = packet.viewType;
		batch.meshId = packet.meshId;
		batch.materialId = packet.materialId;
		batch.packetOffset = packetOffsetBase + static_cast<std::uint32_t>(packetIndex);
		batch.packetCount = 1u;
		batch.selectedMeshlets = packet.meshletCount;
		batch.submittedMeshlets = packet.meshletCount;
		batch.batchReason = "single packet started a compatible batch";
		return batch;
	};

	batches.push_back(startBatch(0u));
	for (std::size_t packetIndex = 1u; packetIndex < packets.size(); ++packetIndex)
	{
		ClusterWorkBatch& currentBatch = batches.back();
		const VirtualGeometryClusterPacket& previousPacket = packets[packetIndex - 1u];
		const VirtualGeometryClusterPacket& packet = packets[packetIndex];
		if (canMergePacket(previousPacket, packet))
		{
			++currentBatch.packetCount;
			currentBatch.selectedMeshlets += packet.meshletCount;
			currentBatch.submittedMeshlets += packet.meshletCount;
			currentBatch.batchReason = "contiguous meshlets share view, mesh, material, LOD, page, and fallback state";
			continue;
		}
		batches.push_back(startBatch(packetIndex));
	}

	for (ClusterWorkBatch& batch : batches)
	{
		if (batch.packetCount == 1u)
		{
			batch.fallbackMode = VirtualGeometryBatchFallbackMode::SinglePacketDraw;
			batch.fallbackReason = "Packet could not merge with a compatible contiguous neighbor, so it keeps the exact single-packet draw.";
		}
	}
	return batches;
}

std::vector<GpuVirtualGeometryPacketRecord> RenderSystem::BuildGpuVirtualGeometryPacketRecords(
	const std::vector<VirtualGeometryClusterPacket>& packets,
	std::uint64_t frameId,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::uint32_t packetIdBase)
{
	(void)packetIdBase;
	std::vector<GpuVirtualGeometryPacketRecord> records;
	records.reserve(packets.size());
	for (const VirtualGeometryClusterPacket& packet : packets)
	{
		GpuVirtualGeometryPacketRecord record;
		// Page-local geometry payloads are keyed by the stable CPU packet id.
		// Re-numbering compacted GPU records here would make the task/mesh
		// shader decode a different packet's stream after culling removes a
		// preceding packet from the submitted set.
		record.packetId = packet.packetId;
		record.viewId = packet.viewId;
		record.viewType = packet.viewType;
		record.frameId = frameId;
		record.objectId = packet.objectId;
		record.renderableIndex = packet.renderableIndex;
		record.meshId = packet.meshId;
		record.materialId = packet.materialId;
		record.selectedLod = packet.selectedLod;
		record.clusterId = packet.clusterId;
		record.meshletOffset = packet.meshletOffset;
		record.meshletCount = packet.meshletCount;
		record.boundsMin = packet.boundsMin;
		record.boundsMax = packet.boundsMax;
		record.projectedBounds = packet.projectedBounds;
		record.pageId = packet.pageId;
		record.fallbackPageId = packet.fallbackPageId;
		record.payloadSource = packet.payloadSource;
		record.emittedSource = packet.emittedSource;
		record.pagePayloadByteOffset = packet.pagePayloadByteOffset;
		record.pagePayloadByteSize = packet.pagePayloadByteSize;
		record.payloadBufferIndex = packet.payloadBufferIndex;
		record.payloadDescriptorIndex = packet.payloadDescriptorIndex;
		record.fallbackReason = packet.fallbackReason;
		record.debugIdentity = packet.debugIdentity;
		// Bit 0 marks fail-open coverage; higher bits mirror the active debug
		// mode so generated submissions can reject stale visualizer ownership.
		record.flags = (packet.fallbackPacket ? 1u : 0u)
			| (static_cast<std::uint32_t>(NormalizeVirtualGeometryDebugVisualizationMode(debugMode)) << 8u)
			| ((packet.rasterRoute & kVirtualGeometryRasterRouteMask)
				<< kVirtualGeometryRasterRouteShift)
			| ((packet.materialBin & kVirtualGeometryMaterialBinMask)
				<< kVirtualGeometryMaterialBinShift);
		records.push_back(record);
	}
	return records;
}

std::optional<VirtualGeometryVisibilityPick>
RenderSystem::PickVirtualGeometryVisibilityRecord(
	std::span<const GpuVirtualGeometryPacketRecord> records,
	const glm::vec2& normalizedViewportPosition)
{
	if(!std::isfinite(normalizedViewportPosition.x)
		|| !std::isfinite(normalizedViewportPosition.y)
		|| normalizedViewportPosition.x < 0.0f
		|| normalizedViewportPosition.x > 1.0f
		|| normalizedViewportPosition.y < 0.0f
		|| normalizedViewportPosition.y > 1.0f)
	{
		return std::nullopt;
	}

	const GpuVirtualGeometryPacketRecord* bestRecord = nullptr;
	float bestArea = std::numeric_limits<float>::max();
	for(const GpuVirtualGeometryPacketRecord& record : records)
	{
		if(record.objectId == TerraEngine::EMPTY_UUID
			|| record.meshId == TerraEngine::EMPTY_UUID
			|| record.frameId == 0u
			|| record.meshletCount == 0u
			|| record.projectedBounds == glm::vec4(0.0f))
		{
			continue;
		}

		const float minimumX = std::clamp(
			std::min(record.projectedBounds.x, record.projectedBounds.z),
			0.0f,
			1.0f);
		const float maximumX = std::clamp(
			std::max(record.projectedBounds.x, record.projectedBounds.z),
			0.0f,
			1.0f);
		const float minimumY = std::clamp(
			std::min(record.projectedBounds.y, record.projectedBounds.w),
			0.0f,
			1.0f);
		const float maximumY = std::clamp(
			std::max(record.projectedBounds.y, record.projectedBounds.w),
			0.0f,
			1.0f);
		if(normalizedViewportPosition.x < minimumX
			|| normalizedViewportPosition.x > maximumX
			|| normalizedViewportPosition.y < minimumY
			|| normalizedViewportPosition.y > maximumY)
		{
			continue;
		}

		const float area = (maximumX - minimumX) * (maximumY - minimumY);
		const bool winsStableTie = bestRecord != nullptr
			&& area == bestArea
			&& record.debugIdentity < bestRecord->debugIdentity;
		if(bestRecord != nullptr && area > bestArea && !winsStableTie)
		{
			continue;
		}
		if(bestRecord != nullptr && area == bestArea && !winsStableTie)
		{
			continue;
		}
		bestRecord = &record;
		bestArea = area;
	}

	if(bestRecord == nullptr)
	{
		return std::nullopt;
	}

	VirtualGeometryVisibilityPick pick;
	pick.objectId = bestRecord->objectId;
	pick.meshId = bestRecord->meshId;
	pick.materialId = bestRecord->materialId;
	pick.frameId = bestRecord->frameId;
	pick.packetId = bestRecord->packetId;
	pick.clusterId = bestRecord->clusterId;
	pick.meshletId = bestRecord->meshletOffset;
	pick.selectedLod = bestRecord->selectedLod;
	pick.debugIdentity = bestRecord->debugIdentity;
	pick.fallback = (bestRecord->flags & 0x1u) != 0u;
	return pick;
}

bool RenderSystem::RequestConventionalVisibilityIdentityPick(
	RenderViewType viewType, const glm::uvec2& pixel,
	bool dedicatedCaptureTarget)
{
	if (!rhi) return false;
	TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackRequest request{};
	request.viewType = viewType;
	request.pixel = pixel;
	request.dedicatedCaptureTarget = dedicatedCaptureTarget;
	const auto targetMetadata =
		rhi->GetConventionalVisibilityIdentityTargetMetadata(
			viewType, dedicatedCaptureTarget);
	if(!targetMetadata || !targetMetadata->IsComplete()) return false;
	CaptureConventionalVisibilityIdentitySnapshot requestSnapshot{};
	std::vector<GpuVirtualGeometryPacketRecord> requestVisibilityRecords;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		// Conventional typed identity is valid for direct meshes as well as VG
		// packets. A dedicated capture target can outlive ordinary Scene/Game
		// frames, so select its own retained immutable table instead of asking the
		// Vulkan target to match a newer normal-frame id.
		const CaptureConventionalVisibilityIdentitySnapshot* captureSnapshot =
			viewType == RenderViewType::Scene
				? &latestSceneCaptureIdentitySnapshot : &latestGameCaptureIdentitySnapshot;
		if(dedicatedCaptureTarget)
		{
			requestSnapshot = *captureSnapshot;
			requestVisibilityRecords = viewType == RenderViewType::Scene
				? latestSceneCaptureVisibilityRecords
				: latestGameCaptureVisibilityRecords;
		}
		else
		{
			requestSnapshot.frameId = latestRenderFrameSnapshot.frameId;
			requestSnapshot.rendererGeneration =
				latestRendererRecoverySnapshot.renderSessionGeneration;
			requestSnapshot.targetGeneration = targetMetadata->targetGeneration;
			requestSnapshot.originGeneration = targetMetadata->originGeneration;
			requestSnapshot.spatialGeneration = targetMetadata->spatialGeneration;
			requestSnapshot.residencyGeneration = targetMetadata->residencyGeneration;
			requestSnapshot.table =
				latestRenderFrameSnapshot.conventionalVisibilityIdentityTable;
			requestVisibilityRecords = viewType == RenderViewType::Scene
				? latestSceneVirtualGeometryVisibilityRecords
				: latestGameVirtualGeometryVisibilityRecords;
		}
		if (!requestSnapshot.IsUsable()
			|| requestSnapshot.frameId != targetMetadata->frameId
			|| requestSnapshot.rendererGeneration != targetMetadata->rendererGeneration
			|| requestSnapshot.targetGeneration != targetMetadata->targetGeneration
			|| requestSnapshot.originGeneration != targetMetadata->originGeneration
			|| requestSnapshot.spatialGeneration != targetMetadata->spatialGeneration
			|| requestSnapshot.residencyGeneration != targetMetadata->residencyGeneration) {
			return false;
		}
		request.frameId = requestSnapshot.frameId;
		request.rendererGeneration = requestSnapshot.rendererGeneration;
		request.targetGeneration = requestSnapshot.targetGeneration;
		request.originGeneration = requestSnapshot.originGeneration;
		request.spatialGeneration = requestSnapshot.spatialGeneration;
		request.residencyGeneration = requestSnapshot.residencyGeneration;
	}
	if (!rhi->QueueConventionalVisibilityIdentityReadback(request)) {
		std::lock_guard<std::mutex> lock(statsMutex);
		std::string& failureReason = viewType == RenderViewType::Scene
			? latestSceneAuthoritativeIdentityFailureReason
			: latestGameAuthoritativeIdentityFailureReason;
		failureReason = dedicatedCaptureTarget
			? "Vulkan rejected the dedicated capture identity request; the retained capture frame no longer matches its target."
			: "Vulkan rejected the viewport identity request; the rendered frame no longer matches its target.";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		// A fresh GPU request invalidates any older value for this view. The
		// replacement is visible only when DrainConventional... observes its fence.
		if (viewType == RenderViewType::Scene) {
			pendingSceneAuthoritativeIdentityPixel = pixel;
			pendingSceneAuthoritativeIdentityFromCapture = dedicatedCaptureTarget;
			pendingSceneIdentitySnapshot = std::move(requestSnapshot);
			pendingSceneIdentityVisibilityRecords = std::move(requestVisibilityRecords);
			latestSceneAuthoritativeIdentityPick.reset();
			latestSceneAuthoritativeIdentityFailureReason.clear();
		} else {
			pendingGameAuthoritativeIdentityPixel = pixel;
			pendingGameAuthoritativeIdentityFromCapture = dedicatedCaptureTarget;
			pendingGameIdentitySnapshot = std::move(requestSnapshot);
			pendingGameIdentityVisibilityRecords = std::move(requestVisibilityRecords);
			latestGameAuthoritativeIdentityPick.reset();
			latestGameAuthoritativeIdentityFailureReason.clear();
		}
	}
	return true;
}

std::optional<VirtualGeometryVisibilityPick>
RenderSystem::GetLatestAuthoritativeConventionalVisibilityIdentityPick(
	RenderViewType viewType, const glm::uvec2& pixel) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	const std::optional<VirtualGeometryVisibilityPick>& pick =
		viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityPick : latestGameAuthoritativeIdentityPick;
	const glm::uvec2 storedPixel = viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityPixel : latestGameAuthoritativeIdentityPixel;
	if (storedPixel != pixel || !pick || !pick->IsValid()) return std::nullopt;
	return pick;
}

TerraEngine::Renderer::AuthoritativeVisibilityPickState
RenderSystem::GetConventionalVisibilityIdentityPickState(
	RenderViewType viewType, const glm::uvec2& pixel,
	bool dedicatedCaptureTarget) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	TerraEngine::Renderer::AuthoritativeVisibilityPickState state{};
	state.pixel = pixel;
	state.dedicatedCaptureTarget = dedicatedCaptureTarget;
	const std::optional<VirtualGeometryVisibilityPick>& completed =
		viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityPick : latestGameAuthoritativeIdentityPick;
	const glm::uvec2 completedPixel = viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityPixel : latestGameAuthoritativeIdentityPixel;
	const bool completedFromCapture = viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityFromCapture : latestGameAuthoritativeIdentityFromCapture;
	const std::optional<glm::uvec2>& pending = viewType == RenderViewType::Scene
		? pendingSceneAuthoritativeIdentityPixel : pendingGameAuthoritativeIdentityPixel;
	const bool pendingFromCapture = viewType == RenderViewType::Scene
		? pendingSceneAuthoritativeIdentityFromCapture : pendingGameAuthoritativeIdentityFromCapture;
	const std::string& failureReason = viewType == RenderViewType::Scene
		? latestSceneAuthoritativeIdentityFailureReason : latestGameAuthoritativeIdentityFailureReason;
	if (completedPixel == pixel && completed && completed->IsValid()
		&& completedFromCapture == dedicatedCaptureTarget) {
		state.status = TerraEngine::Renderer::AuthoritativeVisibilityPickStatus::Ready;
		state.pick = completed;
		state.dedicatedCaptureTarget = completedFromCapture;
		return state;
	}
	if (pending && *pending == pixel && pendingFromCapture == dedicatedCaptureTarget) {
		state.status = TerraEngine::Renderer::AuthoritativeVisibilityPickStatus::Pending;
		state.reason = "The typed Vulkan identity copy is waiting for its frame fence.";
		return state;
	}
	state.status = TerraEngine::Renderer::AuthoritativeVisibilityPickStatus::Unavailable;
	state.reason = failureReason.empty()
		? "No authoritative Vulkan identity result is available for this pixel."
		: failureReason;
	return state;
}

std::optional<TerraEngine::Renderer::RenderReferenceFrame>
RenderSystem::GetLatestRenderReferenceFrame(RenderViewType viewType) const
{
	const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> publication =
		renderFramePublicationStore.Load();
	if (!publication)
		return std::nullopt;
	const std::size_t count = std::min(publication->views.size(), publication->referenceFrames.size());
	for (std::size_t index = 0U; index < count; ++index)
	{
		if (publication->views[index].viewType == viewType
			&& publication->referenceFrames[index].IsValid())
			return publication->referenceFrames[index];
	}
	return std::nullopt;
}

bool RenderSystem::SetVirtualGeometryValidationRouteOverride(
	TerraEngine::Renderer::VirtualGeometryRasterRoute route,
	std::string* outReason)
{
	if (route == TerraEngine::Renderer::VirtualGeometryRasterRoute::Fallback) {
		if (outReason) *outReason = "Fallback is not a validation route; request Indexed explicitly.";
		return false;
	}
	if (!rhi) {
		if (outReason) *outReason = "The Vulkan renderer is unavailable.";
		return false;
	}
	const TerraEngine::Renderer::BackendCapabilityProfile capabilities =
		rhi->GetBackendCapabilityProfile();
	const bool supported = route == TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed
		|| (route == TerraEngine::Renderer::VirtualGeometryRasterRoute::Hardware
			&& rhi->SupportsVirtualGeometryMeshShaderRoute())
		|| (route == TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute
			&& capabilities.supportsVirtualGeometryGpuPacketBuffers);
	if (!supported) {
		if (outReason) *outReason = "The active Vulkan adapter does not support the requested validation route.";
		return false;
	}
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	virtualGeometryValidationRouteOverride = route;
	virtualGeometryHybridValidationSceneRequested = false;
	if (outReason) outReason->clear();
	return true;
}

void RenderSystem::SetVirtualGeometryHybridValidationSceneRequested(bool requested)
{
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	// Hybrid remains an intentional mixture of real packet routes. It never
	// becomes a synthetic backend enum or a force-route value in Vulkan work.
	virtualGeometryValidationRouteOverride.reset();
	virtualGeometryHybridValidationSceneRequested = requested;
}

void RenderSystem::ClearVirtualGeometryValidationRouteOverride()
{
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	virtualGeometryValidationRouteOverride.reset();
	virtualGeometryHybridValidationSceneRequested = false;
}

std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute>
RenderSystem::GetVirtualGeometryValidationRouteOverride() const
{
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	return virtualGeometryValidationRouteOverride;
}

bool RenderSystem::IsVirtualGeometryHybridValidationSceneRequested() const
{
	std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
	return virtualGeometryHybridValidationSceneRequested;
}

std::vector<GpuVirtualGeometryBatchDescriptor> RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(
	const std::vector<GpuVirtualGeometryPacketRecord>& packets,
	std::uint64_t frameId,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::uint32_t batchIdBase)
{
	std::vector<GpuVirtualGeometryBatchDescriptor> batches;
	if (packets.empty())
	{
		return batches;
	}

	auto canMergePacket = [](const GpuVirtualGeometryPacketRecord& previous, const GpuVirtualGeometryPacketRecord& next) {
		const auto isPageLocalSource = [](VirtualGeometryPagePayloadSource source) {
			return source == VirtualGeometryPagePayloadSource::ResidentPagePayload
				|| source == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		};
		const bool pageLocalRun = isPageLocalSource(previous.payloadSource)
			&& isPageLocalSource(next.payloadSource);
		const bool compatibleOwner = previous.viewId == next.viewId
			&& previous.viewType == next.viewType
			&& previous.objectId == next.objectId
			&& previous.meshId == next.meshId
			&& previous.materialId == next.materialId
			&& previous.selectedLod == next.selectedLod
			&& previous.payloadSource == next.payloadSource
			&& previous.emittedSource == next.emittedSource
			&& previous.flags == next.flags
			&& previous.packetId + 1u == next.packetId;
		// Match the CPU batching contract. Page-local packet rows own their stream
		// bindings independently; non-page-local runs still require one binding.
		const bool compatiblePayload = pageLocalRun
			|| (previous.pageId == next.pageId
				&& previous.fallbackPageId == next.fallbackPageId
				&& previous.pagePayloadByteOffset == next.pagePayloadByteOffset
				&& previous.pagePayloadByteSize == next.pagePayloadByteSize
				&& previous.payloadBufferIndex == next.payloadBufferIndex
				&& previous.payloadDescriptorIndex == next.payloadDescriptorIndex);
		return compatibleOwner
			&& compatiblePayload
			&& previous.meshletOffset + previous.meshletCount == next.meshletOffset;
	};

	auto startBatch = [&](std::size_t packetIndex) {
		const GpuVirtualGeometryPacketRecord& packet = packets[packetIndex];
		GpuVirtualGeometryBatchDescriptor batch;
		batch.batchId = batchIdBase + static_cast<std::uint32_t>(batches.size());
		batch.viewId = packet.viewId;
		batch.viewType = packet.viewType;
		batch.frameId = frameId;
		batch.packetOffset = static_cast<std::uint32_t>(packetIndex);
		batch.packetCount = 1u;
		batch.meshId = packet.meshId;
		batch.materialId = packet.materialId;
		batch.selectedMeshlets = packet.meshletCount;
		batch.submittedMeshlets = packet.meshletCount;
		batch.debugMode = NormalizeVirtualGeometryDebugVisualizationMode(debugMode);
		batch.flags = packet.flags;
		return batch;
	};

	batches.push_back(startBatch(0u));
	for (std::size_t packetIndex = 1u; packetIndex < packets.size(); ++packetIndex)
	{
		GpuVirtualGeometryBatchDescriptor& currentBatch = batches.back();
		if (canMergePacket(packets[packetIndex - 1u], packets[packetIndex]))
		{
			++currentBatch.packetCount;
			currentBatch.selectedMeshlets += packets[packetIndex].meshletCount;
			currentBatch.submittedMeshlets += packets[packetIndex].meshletCount;
			continue;
		}
		batches.push_back(startBatch(packetIndex));
	}

	for (GpuVirtualGeometryBatchDescriptor& batch : batches)
	{
		if (batch.packetCount == 1u)
		{
			batch.fallbackMode = VirtualGeometryBatchFallbackMode::SinglePacketDraw;
		}
	}
	return batches;
}

GpuVirtualGeometryPacketFrameSet RenderSystem::BuildGpuVirtualGeometryPacketFrameSet(
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint64_t frameId,
	const glm::uvec2& targetSize,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::uint32_t packetOffset,
	std::uint32_t packetCount,
	std::uint32_t batchOffset,
	std::uint32_t batchCount)
{
	GpuVirtualGeometryPacketFrameSet frameSet;
	frameSet.viewType = viewType;
	frameSet.viewId = viewId;
	frameSet.frameId = frameId;
	frameSet.targetSize = targetSize;
	frameSet.debugMode = NormalizeVirtualGeometryDebugVisualizationMode(debugMode);
	frameSet.packetOffset = packetOffset;
	frameSet.packetCount = packetCount;
	frameSet.batchOffset = batchOffset;
	frameSet.batchCount = batchCount;
	frameSet.compactedPacketOffset = packetOffset;
	frameSet.compactedPacketCount = packetCount;
	frameSet.generatedSubmissionOffset = batchOffset;
	frameSet.generatedSubmissionCount = batchCount;
	frameSet.validity = packetCount == 0u
		? VirtualGeometryGpuFrameSetValidity::Delayed
		: VirtualGeometryGpuFrameSetValidity::Valid;
	frameSet.fallbackReason = packetCount == 0u ? "No virtual geometry packets were available for this view." : std::string();
	return frameSet;
}

bool RenderSystem::ValidateGpuVirtualGeometryPacketRecords(
	const std::vector<VirtualGeometryClusterPacket>& cpuPackets,
	const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters,
	const std::vector<GpuVirtualGeometryPacketRecord>& gpuPackets,
	std::uint64_t frameId,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::string* outReason)
{
	auto reject = [outReason](std::string reason) {
		if (outReason != nullptr)
		{
			*outReason = std::move(reason);
		}
		return false;
	};

	if (cpuPackets.size() != gpuPackets.size())
	{
		return reject("GPU packet mirror count does not match the CPU-selected cluster packet count.");
	}
	const std::uint32_t expectedDebugBits =
		static_cast<std::uint32_t>(NormalizeVirtualGeometryDebugVisualizationMode(debugMode)) << 8u;
	for (std::size_t index = 0u; index < cpuPackets.size(); ++index)
	{
		const VirtualGeometryClusterPacket& cpu = cpuPackets[index];
		const GpuVirtualGeometryPacketRecord& gpu = gpuPackets[index];
		if (gpu.frameId != frameId)
		{
			return reject("GPU packet frame ownership does not match the frame being published.");
		}
		if (gpu.packetId != cpu.packetId)
		{
			return reject("GPU packet id does not match the CPU packet id used by page-local descriptors.");
		}
		if (gpu.viewId != cpu.viewId || gpu.viewType != cpu.viewType)
		{
			return reject("GPU packet view ownership does not match the CPU cluster packet.");
		}
		if (gpu.objectId != cpu.objectId || gpu.meshId != cpu.meshId || gpu.materialId != cpu.materialId)
		{
			return reject("GPU packet object, mesh, or material ownership does not match the CPU packet.");
		}
		if (gpu.selectedLod != cpu.selectedLod || gpu.clusterId != cpu.clusterId)
		{
			return reject("GPU packet selected LOD or cluster identity does not match the CPU packet.");
		}
		if (gpu.meshletOffset != cpu.meshletOffset || gpu.meshletCount != cpu.meshletCount)
		{
			return reject("GPU packet meshlet range does not match the CPU packet range.");
		}
		if (gpu.pageId != cpu.pageId || gpu.fallbackPageId != cpu.fallbackPageId)
		{
			return reject("GPU packet page or fallback-page identity does not match the CPU packet.");
		}
		if (gpu.payloadSource != cpu.payloadSource || gpu.emittedSource != cpu.emittedSource)
		{
			return reject("GPU packet page source labels do not match the CPU packet.");
		}
		if (gpu.pagePayloadByteOffset != cpu.pagePayloadByteOffset || gpu.pagePayloadByteSize != cpu.pagePayloadByteSize)
		{
			return reject("GPU packet page-local payload byte range does not match the CPU packet.");
		}
		if (gpu.payloadBufferIndex != cpu.payloadBufferIndex || gpu.payloadDescriptorIndex != cpu.payloadDescriptorIndex)
		{
			return reject("GPU packet physical page payload binding does not match the CPU packet.");
		}
		const bool expectedFallback = cpu.fallbackPacket;
		const bool gpuFallback = (gpu.flags & 1u) != 0u;
		if (gpuFallback != expectedFallback)
		{
			return reject("GPU packet fallback flag does not match the CPU packet fallback state.");
		}
		if ((gpu.flags & (kVirtualGeometryDebugModeMask << kVirtualGeometryDebugModeShift)) != expectedDebugBits)
		{
			return reject("GPU packet debug-mode ownership is stale for the current view.");
		}
		if (!selectedClusters.empty())
		{
			const auto selectedIt = std::find_if(
				selectedClusters.begin(),
				selectedClusters.end(),
				[&cpu](const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& selected) {
					return selected.objectId == cpu.objectId
						&& selected.meshId == cpu.meshId
						&& selected.clusterId == cpu.clusterId;
				});
			if (selectedIt == selectedClusters.end())
			{
				return reject("GPU packet does not preserve the CPU-selected cluster cut identity.");
			}
			if (selectedIt->lodIndex != cpu.selectedLod || selectedIt->pageId != cpu.pageId)
			{
				return reject("GPU packet does not preserve selected cluster LOD or page ownership.");
			}
			if (selectedIt->usingFallback != expectedFallback)
			{
				return reject("GPU packet fallback state does not match selected cluster fallback coverage.");
			}
		}
	}

	if (outReason != nullptr)
	{
		outReason->clear();
	}
	return true;
}

std::vector<GpuPacketCompactionDispatch> RenderSystem::BuildGpuPacketCompactionDispatches(
	const std::vector<GpuVirtualGeometryPacketFrameSet>& frameSets,
	const VirtualGeometryHiZValidationState& hiZState,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::uint32_t dispatchIdBase)
{
	std::vector<GpuPacketCompactionDispatch> dispatches;
	dispatches.reserve(frameSets.size());
	for (const GpuVirtualGeometryPacketFrameSet& frameSet : frameSets)
	{
		GpuPacketCompactionDispatch dispatch;
		dispatch.dispatchId = dispatchIdBase + static_cast<std::uint32_t>(dispatches.size());
		dispatch.viewId = frameSet.viewId;
		dispatch.viewType = frameSet.viewType;
		dispatch.frameId = frameSet.frameId;
		dispatch.candidatePacketCount = frameSet.packetCount;
		dispatch.candidateBatchCount = frameSet.batchCount;
		dispatch.outputCapacity = frameSet.packetCount;
		dispatch.hiZBaselineState = hiZState.state;
		dispatch.debugMode = NormalizeVirtualGeometryDebugVisualizationMode(debugMode);
		dispatch.failOpenOnInvalid = true;
		if (frameSet.validity != VirtualGeometryGpuFrameSetValidity::Valid)
		{
			dispatch.fallbackReason = frameSet.fallbackReason.empty()
				? "GPU packet frame set is not valid, so compaction must fail open to CPU-selected coverage."
				: frameSet.fallbackReason;
		}
		dispatches.push_back(std::move(dispatch));
	}
	return dispatches;
}

std::vector<GeneratedVirtualGeometrySubmissionRecord> RenderSystem::BuildGeneratedVirtualGeometrySubmissionRecords(
	const std::vector<GpuVirtualGeometryBatchDescriptor>& batches,
	const std::vector<GpuVirtualGeometryPacketRecord>& packets,
	std::uint64_t frameId,
	std::uint32_t recordIdBase)
{
	std::vector<GeneratedVirtualGeometrySubmissionRecord> submissions;
	submissions.reserve(batches.size());
	for (const GpuVirtualGeometryBatchDescriptor& batch : batches)
	{
		GeneratedVirtualGeometrySubmissionRecord submission;
		submission.recordId = recordIdBase + static_cast<std::uint32_t>(submissions.size());
		submission.viewId = batch.viewId;
		submission.viewType = batch.viewType;
		submission.frameId = frameId;
		submission.batchId = batch.batchId;
		submission.packetOffset = batch.packetOffset;
		submission.packetCount = batch.packetCount;
		submission.indirectTaskCount = batch.submittedMeshlets == 0u ? 0u : std::max(1u, (batch.submittedMeshlets + 127u) / 128u);
		submission.materialId = batch.materialId;
		submission.meshId = batch.meshId;
		submission.counterOffset = submission.recordId;
		submission.source = VirtualGeometryCounterSource::Prepared;

		const bool rangeValid = batch.packetCount > 0u
			&& batch.packetOffset <= packets.size()
			&& static_cast<std::size_t>(batch.packetOffset) + static_cast<std::size_t>(batch.packetCount) <= packets.size();
		if (!rangeValid)
		{
			submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::FailOpen;
			submission.fallbackReason = "Generated submission batch references packets outside the compacted packet table.";
		}
		submissions.push_back(std::move(submission));
	}
	return submissions;
}

bool RenderSystem::ValidateGeneratedVirtualGeometrySubmissionRecords(
	const std::vector<GeneratedVirtualGeometrySubmissionRecord>& submissions,
	const std::vector<GpuVirtualGeometryBatchDescriptor>& batches,
	std::uint64_t frameId,
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint32_t packetCapacity,
	std::string* outReason)
{
	auto reject = [outReason](std::string reason) {
		if (outReason != nullptr)
		{
			*outReason = std::move(reason);
		}
		return false;
	};

	if (submissions.empty())
	{
		if (packetCapacity == 0u)
		{
			if (outReason != nullptr)
			{
				outReason->clear();
			}
			return true;
		}
		return reject("No generated submissions were available for non-empty packet work.");
	}
	if (submissions.size() > batches.size())
	{
		return reject("Generated submissions outnumber compatible batch descriptors.");
	}

	for (const GeneratedVirtualGeometrySubmissionRecord& submission : submissions)
	{
		if (submission.frameId != frameId || submission.viewType != viewType || submission.viewId != viewId)
		{
			return reject("Generated submission frame or view ownership is stale.");
		}
		if (submission.validationState != VirtualGeometryGeneratedSubmissionValidationState::Valid)
		{
			return reject(submission.fallbackReason.empty()
				? "Generated submission was already marked invalid."
				: submission.fallbackReason);
		}
		if (submission.packetCount == 0u || submission.indirectTaskCount == 0u)
		{
			return reject("Generated submission contains no packet or task work.");
		}
		if (submission.packetOffset > packetCapacity
			|| submission.packetOffset + submission.packetCount > packetCapacity)
		{
			return reject("Generated submission packet range exceeds compacted packet capacity.");
		}
		const auto batchIt = std::find_if(
			batches.begin(),
			batches.end(),
			[&submission](const GpuVirtualGeometryBatchDescriptor& batch) {
				return batch.batchId == submission.batchId
					&& batch.packetOffset == submission.packetOffset
					&& batch.packetCount == submission.packetCount
					&& batch.meshId == submission.meshId
					&& batch.materialId == submission.materialId;
			});
		if (batchIt == batches.end())
		{
			return reject("Generated submission no longer matches a compatible GPU batch descriptor.");
		}
	}

	if (outReason != nullptr)
	{
		outReason->clear();
	}
	return true;
}

VirtualGeometryCounterReadbackRecord RenderSystem::BuildVirtualGeometryCounterReadbackRecord(
	const CompactedVisibilitySet& compaction,
	std::uint64_t frameId,
	VirtualGeometryCounterSource source,
	std::uint32_t latencyFrames,
	std::string unavailableReason)
{
	VirtualGeometryCounterReadbackRecord readback;
	readback.viewId = compaction.viewId;
	readback.viewType = compaction.viewType;
	readback.frameId = frameId;
	readback.source = source;
	readback.latencyFrames = latencyFrames;
	readback.candidatePackets = compaction.candidatePackets;
	readback.compactedPackets = compaction.visiblePackets;
	readback.submittedBatches = compaction.submittedBatches;
	readback.rejectedPackets = compaction.rejectedByFrustum
		+ compaction.rejectedByHiZ
		+ compaction.rejectedByResidency
		+ compaction.rejectedByCone;
	readback.failOpenPackets = compaction.failOpenPackets;
	readback.unavailableReason = std::move(unavailableReason);
	return readback;
}

VirtualGeometryGpuFallbackRecord RenderSystem::BuildVirtualGeometryGpuFallbackRecord(
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint64_t frameId,
	std::string reason,
	std::uint32_t affectedPackets,
	std::uint32_t affectedBatches,
	std::string diagnostic)
{
	VirtualGeometryGpuFallbackRecord fallback;
	fallback.viewType = viewType;
	fallback.viewId = viewId;
	fallback.frameId = frameId;
	fallback.reason = std::move(reason);
	fallback.affectedPackets = affectedPackets;
	fallback.affectedBatches = affectedBatches;
	fallback.coveragePolicy = "FailOpenCoverage";
	fallback.diagnostic = std::move(diagnostic);
	return fallback;
}

bool RenderSystem::AreVirtualGeometryClustersConeCullingEligible(
	const VirtualGeometryAssetRuntime& asset,
	const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters,
	std::string* outDisabledReason)
{
	if (outDisabledReason != nullptr)
	{
		outDisabledReason->clear();
	}
	if (selectedClusters.empty())
	{
		if (outDisabledReason != nullptr)
		{
			*outDisabledReason = "No selected virtual-geometry clusters were available for cone safety validation.";
		}
		return false;
	}

	for (const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& selectedCluster : selectedClusters)
	{
		const auto metadata = std::find_if(
			asset.coneSafety.begin(),
			asset.coneSafety.end(),
			[&selectedCluster](const ConeSafetyMetadata& candidate) {
				return candidate.clusterId == selectedCluster.clusterId;
			});
		if (metadata == asset.coneSafety.end())
		{
			if (outDisabledReason != nullptr)
			{
				*outDisabledReason = "Selected cluster is missing cone safety metadata.";
			}
			return false;
		}

		const ConeSafetyMetadata normalized = NormalizeVirtualGeometryConeSafetyMetadata(*metadata);
		if (!normalized.eligible)
		{
			if (outDisabledReason != nullptr)
			{
				*outDisabledReason = normalized.disabledReason.empty()
					? "Selected cluster is not eligible for cone culling."
					: normalized.disabledReason;
			}
			return false;
		}
	}
	return true;
}

CompactedVisibilitySet RenderSystem::CompactVirtualGeometryClusterPackets(
	const std::vector<VirtualGeometryClusterPacket>& candidatePackets,
	std::vector<VirtualGeometryClusterPacket>& compactedPackets,
	RenderViewType viewType,
	std::uint32_t viewId,
	bool failOpenForMissingResourcePackets)
{
	compactedPackets.clear();
	compactedPackets.reserve(candidatePackets.size());
	CompactedVisibilitySet result;
	result.viewId = viewId;
	result.viewType = viewType;
	result.candidatePackets = static_cast<std::uint32_t>(candidatePackets.size());
	result.source = VirtualGeometryCounterSource::Prepared;

	auto projectedBoundsProvided = [](const VirtualGeometryClusterPacket& packet) {
		return packet.projectedBounds != glm::vec4(0.0f);
	};
	auto projectedBoundsOutsideView = [](const VirtualGeometryClusterPacket& packet) {
		const float minX = std::min(packet.projectedBounds.x, packet.projectedBounds.z);
		const float maxX = std::max(packet.projectedBounds.x, packet.projectedBounds.z);
		const float minY = std::min(packet.projectedBounds.y, packet.projectedBounds.w);
		const float maxY = std::max(packet.projectedBounds.y, packet.projectedBounds.w);
		return maxX < 0.0f || maxY < 0.0f || minX > 1.0f || minY > 1.0f;
	};

	for (const VirtualGeometryClusterPacket& packet : candidatePackets)
	{
		if (packet.meshletCount == 0u)
		{
			++result.rejectedByResidency;
			continue;
		}
		if (projectedBoundsProvided(packet) && projectedBoundsOutsideView(packet))
		{
			++result.rejectedByFrustum;
			continue;
		}

		// Hi-Z remains shader-owned in this milestone. CPU compaction only
		// carries the current packet set forward so the established task shader
		// policy can cull or fail open exactly as before.
		if (packet.fallbackPacket)
		{
			++result.failOpenPackets;
		}
		compactedPackets.push_back(packet);
	}

	if (compactedPackets.empty()
		&& failOpenForMissingResourcePackets
		&& result.rejectedByResidency == result.candidatePackets
		&& result.candidatePackets > 0u)
	{
		compactedPackets = candidatePackets;
		result.failOpenPackets = result.candidatePackets;
		result.rejectedByResidency = 0u;
	}

	result.visiblePackets = static_cast<std::uint32_t>(compactedPackets.size());
	return result;
}

const std::unordered_map<entt::entity, TerraEngine::Scene::WorldPose>& TerraEngine::Renderer::RenderWorldTransformCache::Refresh(
	ObjectManager& objects,
	TerraEngine::Renderer::RenderTransformCacheDiagnostics& diagnostics)
{
	// The render system owns this copied snapshot at its protected boundary. It
	// must never retain pointers into live component storage because worker and
	// editor mutations can replace that storage after the snapshot is prepared.
	diagnostics = {};
	const std::uint64_t observedSceneGeneration = objects.GetSceneGeneration();
	const std::uint64_t observedHierarchyRevision = objects.GetHierarchyRevision();
	const std::uint64_t observedSpatialRevision = objects.GetSpatialRevision();
	if (!initialized || sceneGeneration != observedSceneGeneration
		|| hierarchyRevision != observedHierarchyRevision)
	{
		graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects, false);
		graph.revision = observedSpatialRevision;
		flatHierarchy = std::all_of(graph.objects.begin(), graph.objects.end(), [](const auto& entry) {
			const TerraEngine::Scene::SceneGraphObject& object = entry.second;
			return object.parentId == TerraEngine::EMPTY_UUID && object.children.empty();
		});
		latestPropagation = TerraEngine::Scene::PropagateWorldTransforms(
			graph,
			TerraEngine::Scene::BuildDirtyTransformState(graph));

		worldTransforms.clear();
		worldTransforms.reserve(latestPropagation.worldTransformsByEntity.size());
		for (const auto& [entity, transform] : latestPropagation.worldTransformsByEntity)
		{
			worldTransforms.emplace(entity, transform);
		}

		initialized = true;
		sceneGeneration = observedSceneGeneration;
		hierarchyRevision = observedHierarchyRevision;
		spatialRevision = observedSpatialRevision;
		hierarchyLevelCount = latestPropagation.hierarchyLevelCount;
		diagnostics.rebuiltForHierarchy = true;
		diagnostics.scannedObjectCount = static_cast<std::uint32_t>(graph.objects.size());
		diagnostics.changedObjectCount = static_cast<std::uint32_t>(graph.objects.size());
		diagnostics.propagatedObjectCount = latestPropagation.processedObjectCount;
		diagnostics.hierarchyLevelCount = hierarchyLevelCount;
		diagnostics.hierarchyRevision = hierarchyRevision;
		return worldTransforms;
	}
	if (spatialRevision == observedSpatialRevision)
	{
		// No spatial mutation can have changed a local Transform revision, so
		// avoid even the compact revision scan on a completely unchanged frame.
		latestPropagation = {};
		latestPropagation.graphRevision = graph.revision;
		latestPropagation.hierarchyLevelCount = hierarchyLevelCount;
		latestPropagation.skippedCleanObjectCount = static_cast<std::uint32_t>(graph.objects.size());
		diagnostics.reusedWithoutPropagation = true;
		diagnostics.hierarchyLevelCount = hierarchyLevelCount;
		diagnostics.hierarchyRevision = hierarchyRevision;
		return worldTransforms;
	}

	// A stable hierarchy lets the protected render path inspect only compact
	// per-object transform revisions instead of rebuilding every parent/child
	// edge. Children of a changed object are expanded by the shared propagator.
	std::vector<TerraEngine::Scene::SceneObjectId> changedObjectIds;
	changedObjectIds.reserve(graph.objects.size());
	for (auto& [objectId, graphObject] : graph.objects)
	{
		const std::optional<Object> object = objects.getObject(graphObject.entity);
		if (!object)
		{
			// A removed entity without a hierarchy revision is inconsistent state.
			// Rebuild defensively so the renderer fails closed to a fresh snapshot.
			initialized = false;
			return Refresh(objects, diagnostics);
		}

		const std::uint64_t transformRevision = objects.GetObjectTransformRevision(*object);
		if (transformRevision == graphObject.transformRevision)
		{
			continue;
		}

		// Copy the current authored local transform into the immutable graph before
		// scheduling the dependency-aware world transform propagation.
		const Transform* localTransform = objects.GetComponent<Transform>(*object);
		const Transform local = localTransform != nullptr ? *localTransform : Transform{};
		graphObject.worldPose = local.worldPose;
		graphObject.parentRelativePose = local.parentRelativePose;
		graphObject.transformRevision = transformRevision;
		graphObject.spatialRevision = objects.GetObjectSpatialRevision(*object);
		changedObjectIds.push_back(objectId);
	}

	graph.revision = observedSpatialRevision;
	spatialRevision = observedSpatialRevision;
	diagnostics.scannedObjectCount = static_cast<std::uint32_t>(graph.objects.size());
	diagnostics.changedObjectCount = static_cast<std::uint32_t>(changedObjectIds.size());
	diagnostics.hierarchyLevelCount = hierarchyLevelCount;
	diagnostics.hierarchyRevision = hierarchyRevision;
	if (changedObjectIds.empty())
	{
		// Preserve explicit zero-work diagnostics so profiler and editor tools can
		// distinguish an intentional cache hit from a skipped render update.
		latestPropagation = {};
		latestPropagation.graphRevision = graph.revision;
		latestPropagation.hierarchyLevelCount = hierarchyLevelCount;
		latestPropagation.skippedCleanObjectCount = static_cast<std::uint32_t>(graph.objects.size());
		diagnostics.reusedWithoutPropagation = true;
		return worldTransforms;
	}
	if (flatHierarchy)
	{
		// Physics stress fixtures commonly contain independent root bodies. Their
		// world transform is exactly the authored local transform, so rebuilding
		// generic traversal plans, UUID sort keys, and worker graphs per body adds
		// latency without adding correctness. Cache-map writes stay on this
		// protected coordinator thread to keep the published snapshot coherent.
		latestPropagation = {};
		latestPropagation.graphRevision = graph.revision;
		latestPropagation.hierarchyLevelCount = graph.objects.empty() ? 0U : 1U;
		latestPropagation.dirtyRootCount = static_cast<std::uint32_t>(changedObjectIds.size());
		latestPropagation.expandedDirtyObjectCount = static_cast<std::uint32_t>(changedObjectIds.size());
		latestPropagation.processedObjectCount = static_cast<std::uint32_t>(changedObjectIds.size());
		latestPropagation.skippedCleanObjectCount = static_cast<std::uint32_t>(graph.objects.size() - changedObjectIds.size());
		for (const TerraEngine::Scene::SceneObjectId& objectId : changedObjectIds)
		{
			const auto graphObject = graph.objects.find(objectId);
			if (graphObject == graph.objects.end())
			{
				continue;
			}

			TerraEngine::Scene::SceneGraphObject& object = graphObject->second;
			worldTransforms[object.entity] = object.worldPose;
		}

		hierarchyLevelCount = latestPropagation.hierarchyLevelCount;
		diagnostics.usedFlatHierarchyFastPath = true;
		diagnostics.propagatedObjectCount = latestPropagation.processedObjectCount;
		diagnostics.hierarchyLevelCount = hierarchyLevelCount;
		return worldTransforms;
	}

	latestPropagation = TerraEngine::Scene::PropagateWorldTransforms(
		graph,
		TerraEngine::Scene::BuildDirtyTransformState(
			graph,
			changedObjectIds,
			TerraEngine::Scene::SceneTransformDirtyReason::LocalTransform));
	for (const auto& [entity, transform] : latestPropagation.worldTransformsByEntity)
	{
		worldTransforms[entity] = transform;
	}

	diagnostics.propagatedObjectCount = latestPropagation.processedObjectCount;
	diagnostics.hierarchyLevelCount = latestPropagation.hierarchyLevelCount;
	hierarchyLevelCount = latestPropagation.hierarchyLevelCount;
	return worldTransforms;
}

const TerraEngine::Scene::SceneTransformPropagationResult& TerraEngine::Renderer::RenderWorldTransformCache::LatestPropagation() const
{
	return latestPropagation;
}

void TerraEngine::Renderer::RenderWorldTransformCache::Clear()
{
	// Scene loads and play-mode restoration can replace every entity at once, so
	// discard both the copied graph and transforms before the next protected frame.
	graph = {};
	worldTransforms.clear();
	latestPropagation = {};
	initialized = false;
	sceneGeneration = 0U;
	hierarchyRevision = 0U;
	spatialRevision = 0U;
	hierarchyLevelCount = 0U;
	flatHierarchy = false;
}

TerraEngine::Renderer::AnimationRenderBinding RenderSystem::ResolveAnimationRenderBinding(
	const TerraEngine::Renderer::AnimationRenderBindingRequest& request) noexcept
{
	using namespace TerraEngine::Renderer;
	AnimationRenderBinding result;
	result.meshId = request.meshId;
	result.animatorObjectId = request.animatorObjectId;
	result.skeletonAssetId = request.skeletonAssetId;
	result.animatorObjectGeneration = request.animatorObjectGeneration;
	result.animationAuthorityGeneration = request.animationAuthorityGeneration;
	result.animatorSlotIndex = request.animatorSlotIndex;
	result.animatorGeneration = request.animatorGeneration;
	result.deformablePayloadGeneration = request.deformablePayloadGeneration;
	result.componentGeneration = request.componentGeneration;
	result.skeletonGeneration = request.skeletonGeneration;
	result.skeletonCompatibilityFingerprint = request.skeletonCompatibilityFingerprint;
	result.pageLocalDeformationAvailable =
		request.pageLocalDeformationAvailable;
	result.invalidation = request.invalidation;

	// A current safe/error publication is an explicit dependency result even if
	// its missing skeleton prevented the producer from filling every identity.
	// Report that result before generic identity validation and never consult held.
	if(request.authorityRow != nullptr
		&& (request.authorityRow->safeFallback
			|| request.authorityRow->status != TerraEngine::Animation::AnimationStatus::Ok)) {
		result.status = AnimationRenderBindingStatus::SafePose;
		return result;
	}

	// A Visual GPU animator publishes its packet independently from the CPU pose
	// stream. Accept it only after the packet's complete immutable identity has
	// matched this draw's copied mesh/resource/scene values; Hybrid deliberately
	// falls through and still requires the CPU-authoritative pose path below.
	if(request.authorityRow == nullptr && request.gpuVisualIdentity.has_value()
		&& GpuVisualIdentityMatchesRequest(request, *request.gpuVisualIdentity)) {
		const AnimationGpuRenderBinding& identity = *request.gpuVisualIdentity;
		result.animatorSlotIndex = identity.instanceSlot;
		result.animatorGeneration = identity.instanceGeneration;
		result.componentGeneration = identity.componentGeneration;
		result.skeletonGeneration = identity.skeletonGeneration;
		result.source = AnimationRenderPoseSource::GpuVisual;
		result.status = AnimationRenderBindingStatus::Ready;
		result.posePublicationGeneration = identity.workGeneration;
		result.invalidation.pose = identity.workGeneration;
		result.invalidation.morph = identity.workGeneration;
		result.safePose = false;
		result.gpu = identity;
		return result;
	}

	// Missing identity fields cannot name one compatible mesh/component/skeleton
	// bundle. Preserve the default bind-pose result without copying any pointer.
	if(request.meshId == TerraEngine::EMPTY_UUID
		|| request.animatorObjectId == TerraEngine::EMPTY_UUID
		|| request.skeletonAssetId == TerraEngine::EMPTY_UUID
		|| request.animatorObjectGeneration == 0U
		|| request.animationAuthorityGeneration == 0U
		|| request.animatorSlotIndex == TerraEngine::Animation::kInvalidAnimatorInstanceSlot
		|| request.animatorGeneration == 0U
		|| request.deformablePayloadGeneration == 0U
		|| request.componentGeneration == 0U
		|| request.skeletonGeneration == 0U
		|| request.skeletonCompatibilityFingerprint == 0U) {
		result.status = AnimationRenderBindingStatus::InvalidPose;
		return result;
	}

	if(request.authorityRow != nullptr && request.presentationFrame != nullptr) {
		result.status = ValidateAnimatorPosePublication(request, *request.authorityRow);
		if(result.status != AnimationRenderBindingStatus::Ready) {
			return result;
		}

		result.source = AnimationRenderPoseSource::CpuPose;
		result.posePublicationGeneration = request.authorityRow->snapshot->publicationGeneration;
		result.invalidation.pose = result.posePublicationGeneration;
		result.invalidation.morph = result.posePublicationGeneration;
		result.safePose = false;
		result.presentationFrame = request.presentationFrame;
		result.authorityRow = request.authorityRow;
		return result;
	}

	if(request.fallbackPolicy == AnimationRenderFallbackPolicy::HoldLastCompatiblePose
		&& HeldAnimationBindingMatchesRequest(request)) {
		result.source = AnimationRenderPoseSource::HeldCpuPose;
		result.status = AnimationRenderBindingStatus::Ready;
		result.posePublicationGeneration = request.heldBinding.posePublicationGeneration;
		result.invalidation.pose = request.heldBinding.invalidation.pose;
		result.invalidation.morph = request.heldBinding.invalidation.morph;
		result.safePose = false;
		result.presentationFrame = request.heldBinding.presentationFrame;
		result.authorityRow = request.heldBinding.authorityRow;
		return result;
	}

	result.status = AnimationRenderBindingStatus::MissingPose;
	return result;
}

std::shared_ptr<const TerraEngine::Renderer::CpuAnimationDeformationData>
RenderSystem::BuildCpuAnimationDeformation(
	const TerraEngine::Animation::AnimationAuthorityRow& authorityRow,
	std::span<const std::uint32_t> paletteJoints,
	std::span<const std::uint64_t> morphTargetIds) noexcept
{
	using namespace TerraEngine::Animation;
	using TerraEngine::Renderer::CpuAnimationDeformationData;
	if(authorityRow.safeFallback || authorityRow.status != AnimationStatus::Ok
		|| authorityRow.snapshot == nullptr || authorityRow.snapshot->pose == nullptr
		|| authorityRow.skeletonAsset == nullptr
		|| authorityRow.skeletonCompatibilityFingerprint == 0U
		|| authorityRow.skeletonAsset->compatibilityFingerprint
			!= authorityRow.skeletonCompatibilityFingerprint
		|| paletteJoints.empty()
		|| paletteJoints.size() > kGpuAnimationPaletteEntriesPerPartition
		|| morphTargetIds.size() > kGpuAnimationMaximumMorphTargetsV1) {
		return {};
	}

	const PoseBuffer& pose = *authorityRow.snapshot->pose;
	const SkeletonAsset& skeleton = *authorityRow.skeletonAsset;
	const std::span<const AnimationTransform> modelTransforms =
		pose.ModelTransforms();
	if(modelTransforms.empty()
		|| modelTransforms.size() != authorityRow.jointStableIds.size()
		|| skeleton.joints.size() != skeleton.inverseBindMatrices.size()) {
		return {};
	}

	enum class InverseBindStorage
	{
		Unknown,
		RowMajor,
		ColumnMajor
	};
	std::string sourceExtension =
		std::filesystem::path(skeleton.source.sourcePackagePath)
			.extension().string();
	std::transform(sourceExtension.begin(), sourceExtension.end(),
		sourceExtension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
	const InverseBindStorage inverseBindStorage = sourceExtension == ".fbx"
		? InverseBindStorage::RowMajor
		: (sourceExtension == ".gltf" || sourceExtension == ".glb"
			? InverseBindStorage::ColumnMajor
			: InverseBindStorage::Unknown);

	const auto isIdentityMatrix = [](const AnimationMatrix4x4& matrix) noexcept {
		for(std::size_t row = 0U; row < 4U; ++row) {
			for(std::size_t column = 0U; column < 4U; ++column) {
				const float expected = row == column ? 1.0F : 0.0F;
				if(std::abs(matrix.values[row * 4U + column] - expected)
					> 1.0e-6F) {
					return false;
				}
			}
		}
		return true;
	};
	const auto convertInverseBind = [inverseBindStorage, &isIdentityMatrix](
		const AnimationMatrix4x4& source,
		std::array<float, 16U>& output) noexcept {
		// Version-one skeletons predate an explicit matrix-major field. Match the
		// importer's frozen FBX/glTF convention and accept unknown provenance only
		// for identity, whose transpose is unambiguous.
		if(inverseBindStorage == InverseBindStorage::Unknown
			&& !isIdentityMatrix(source)) {
			return false;
		}
		for(std::size_t row = 0U; row < 4U; ++row) {
			for(std::size_t column = 0U; column < 4U; ++column) {
				const std::size_t sourceIndex =
					inverseBindStorage == InverseBindStorage::ColumnMajor
						? column * 4U + row : row * 4U + column;
				const float value = source.values[sourceIndex];
				if(!std::isfinite(value)) return false;
				output[row * 4U + column] = value;
			}
		}
		return std::abs(output[12U]) <= 1.0e-4F
			&& std::abs(output[13U]) <= 1.0e-4F
			&& std::abs(output[14U]) <= 1.0e-4F
			&& std::abs(output[15U] - 1.0F) <= 1.0e-4F;
	};
	const auto transformMatrix = [](const AnimationTransform& transform,
		std::array<float, 16U>& output) noexcept {
		if(!IsFinite(transform)) return false;
		const float lengthSquared =
			transform.rotation.x * transform.rotation.x
			+ transform.rotation.y * transform.rotation.y
			+ transform.rotation.z * transform.rotation.z
			+ transform.rotation.w * transform.rotation.w;
		if(!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F) {
			return false;
		}
		const float inverseLength = 1.0F / std::sqrt(lengthSquared);
		const float x = transform.rotation.x * inverseLength;
		const float y = transform.rotation.y * inverseLength;
		const float z = transform.rotation.z * inverseLength;
		const float w = transform.rotation.w * inverseLength;
		const float xx = x * x;
		const float yy = y * y;
		const float zz = z * z;
		const float xy = x * y;
		const float xz = x * z;
		const float yz = y * z;
		const float wx = w * x;
		const float wy = w * y;
		const float wz = w * z;
		// T * R * S scales the rotation columns. The shader consumes row-major
		// affine rows and applies dot(row.xyz, position) + row.w.
		output = {
			(1.0F - 2.0F * (yy + zz)) * transform.scale.x,
			(2.0F * (xy - wz)) * transform.scale.y,
			(2.0F * (xz + wy)) * transform.scale.z,
			transform.translation.x,
			(2.0F * (xy + wz)) * transform.scale.x,
			(1.0F - 2.0F * (xx + zz)) * transform.scale.y,
			(2.0F * (yz - wx)) * transform.scale.z,
			transform.translation.y,
			(2.0F * (xz - wy)) * transform.scale.x,
			(2.0F * (yz + wx)) * transform.scale.y,
			(1.0F - 2.0F * (xx + yy)) * transform.scale.z,
			transform.translation.z,
			0.0F, 0.0F, 0.0F, 1.0F};
		return true;
	};
	const auto multiply = [](const std::array<float, 16U>& left,
		const std::array<float, 16U>& right,
		std::array<float, 16U>& output) noexcept {
		for(std::size_t row = 0U; row < 4U; ++row) {
			for(std::size_t column = 0U; column < 4U; ++column) {
				float value = 0.0F;
				for(std::size_t lane = 0U; lane < 4U; ++lane) {
					value += left[row * 4U + lane]
						* right[lane * 4U + column];
				}
				if(!std::isfinite(value)) return false;
				output[row * 4U + column] = value;
			}
		}
		return true;
	};

	try {
		auto deformation = std::make_shared<CpuAnimationDeformationData>();
		deformation->jointCount = static_cast<std::uint32_t>(
			std::min<std::size_t>(paletteJoints.size(),
				kGpuAnimationPaletteEntriesPerPartition));
		deformation->paletteMatrices.reserve(paletteJoints.size());
		for(const std::uint32_t skeletonJointIndex : paletteJoints) {
			if(skeletonJointIndex >= skeleton.joints.size()) return {};
			const std::uint64_t stableJointId =
				skeleton.joints[skeletonJointIndex].stableId;
			const auto poseJoint = std::find(authorityRow.jointStableIds.begin(),
				authorityRow.jointStableIds.end(), stableJointId);
			if(poseJoint == authorityRow.jointStableIds.end()) return {};
			const std::size_t poseJointIndex = static_cast<std::size_t>(
				std::distance(authorityRow.jointStableIds.begin(), poseJoint));
			if(poseJointIndex >= modelTransforms.size()) return {};

			std::array<float, 16U> model{};
			std::array<float, 16U> inverseBind{};
			std::array<float, 16U> skinning{};
			if(!transformMatrix(modelTransforms[poseJointIndex], model)
				|| !convertInverseBind(
					skeleton.inverseBindMatrices[skeletonJointIndex],
					inverseBind)
				|| !multiply(model, inverseBind, skinning)) {
				return {};
			}
			GpuAnimationPaletteRecord matrix{};
			std::copy_n(skinning.begin(), 4U, matrix.row0.begin());
			std::copy_n(skinning.begin() + 4U, 4U, matrix.row1.begin());
			std::copy_n(skinning.begin() + 8U, 4U, matrix.row2.begin());
			deformation->paletteMatrices.push_back(matrix);
		}

		const std::span<const AnimationNamedFloatValue> publishedMorphs =
			pose.MorphValues();
		deformation->morphWeights.reserve(morphTargetIds.size());
		for(const std::uint64_t targetId : morphTargetIds) {
			const auto value = std::find_if(publishedMorphs.begin(),
				publishedMorphs.end(), [targetId](
					const AnimationNamedFloatValue& candidate) {
					return candidate.stableId == targetId;
				});
			const float weight =
				value == publishedMorphs.end() ? 0.0F : value->value;
			if(!std::isfinite(weight)) return {};
			deformation->morphWeights.push_back(weight);
		}
		BuildCpuDeformationContentIdentity(*deformation);
		if(!deformation->IsComplete()) return {};
		return deformation;
	} catch(const std::bad_alloc&) {
		return {};
	}
}

namespace
{
	using BackendStage = TerraEngine::Renderer::VirtualGeometryBackendStage;
	using CapabilityStage =
		TerraEngine::Renderer::VirtualGeometryCapabilityStage;

	CapabilityStage ToCapabilityStage(BackendStage stage)
	{
		switch(stage)
		{
		case BackendStage::Traversal: return CapabilityStage::Traversal;
		case BackendStage::Compaction: return CapabilityStage::Compaction;
		case BackendStage::IndirectGeneration: return CapabilityStage::Indirect;
		case BackendStage::PageDecode: return CapabilityStage::Decode;
		case BackendStage::HardwareRaster: return CapabilityStage::HardwareRaster;
		case BackendStage::TriangleSetup:
		case BackendStage::ComputeRaster: return CapabilityStage::ComputeRaster;
		case BackendStage::VisibilityResolve: return CapabilityStage::Resolve;
		case BackendStage::Shading: return CapabilityStage::Shading;
		case BackendStage::StreamingUpdate: return CapabilityStage::Streaming;
		case BackendStage::Deformation: return CapabilityStage::Deformation;
		case BackendStage::Shadow: return CapabilityStage::Shadow;
		case BackendStage::RayTracing: return CapabilityStage::RayTracing;
		default: return CapabilityStage::Capture;
		}
	}

	TerraEngine::Renderer::VirtualGeometryCapabilityRoute ToCapabilityRoute(
		TerraEngine::Renderer::VirtualGeometryBackendRoute route)
	{
		using BackendRoute =
			TerraEngine::Renderer::VirtualGeometryBackendRoute;
		using CapabilityRoute =
			TerraEngine::Renderer::VirtualGeometryCapabilityRoute;
		switch(route)
		{
		case BackendRoute::MeshShader: return CapabilityRoute::MeshShader;
		case BackendRoute::Compute: return CapabilityRoute::ComputeRaster;
		case BackendRoute::ProcessorOracle:
		case BackendRoute::Indexed: return CapabilityRoute::IndexedFallback;
		default: return CapabilityRoute::Unknown;
		}
	}

	TerraEngine::Renderer::VirtualGeometryMeasurementAvailability
		ToMeasurementAvailability(
			TerraEngine::Renderer::BackendMeasurementAvailability availability)
	{
		using BackendAvailability =
			TerraEngine::Renderer::BackendMeasurementAvailability;
		using Availability =
			TerraEngine::Renderer::VirtualGeometryMeasurementAvailability;
		switch(availability)
		{
		case BackendAvailability::Pending: return Availability::Pending;
		case BackendAvailability::Unavailable: return Availability::Unavailable;
		case BackendAvailability::Invalid: return Availability::Invalid;
		case BackendAvailability::Measured: return Availability::Measured;
		default: return Availability::Invalid;
		}
	}

	bool BackendSupportsStage(
		CapabilityStage stage,
		const TerraEngine::Renderer::BackendCapabilityProfile& profile,
		bool meshShaderRoute)
	{
		switch(stage)
		{
		case CapabilityStage::Compaction:
			return profile.supportsVirtualGeometryGpuCompaction;
		case CapabilityStage::Indirect:
			return profile.supportsVirtualGeometryGeneratedSubmissions;
		case CapabilityStage::ComputeRaster:
			return profile.supportsVirtualGeometryComputeRaster;
		case CapabilityStage::Resolve:
			return profile.supportsVirtualGeometryVisibilityResolve;
		case CapabilityStage::Shading:
			return profile.supportsVirtualGeometryShadingBins;
		case CapabilityStage::Streaming:
		case CapabilityStage::RayTracing:
			return false;
		case CapabilityStage::Capture:
			return profile.supportsImageReadback;
		default:
			return meshShaderRoute;
		}
	}

	TerraEngine::Renderer::VirtualGeometryCapabilityDiagnosticsSnapshot
		CollectVirtualGeometryCapabilityDiagnostics(
			RHIContext& context,
			const TerraEngine::Renderer::BackendCapabilityProfile& profile,
			const TerraEngine::Renderer::VirtualGeometryFrameViewIdentity&
				currentPublication)
	{
		using namespace TerraEngine::Renderer;
		std::vector<VirtualGeometryBackendExecutionReceipt> receipts =
			context.PollVirtualGeometryExecutionReceipts();
		std::vector<VirtualGeometryBackendMeasurement> backendMeasurements =
			context.PollVirtualGeometryMeasurements();

		VirtualGeometryCapabilityDiagnosticsSnapshot diagnostics{};
		diagnostics.publicationIdentity = currentPublication;

		// Delayed queries keep the identity of the frame that owned the commands.
		// Publish the newest completed identity, then reject older/mismatched rows
		// through the shared generation-safe diagnostics applicator.
		std::optional<VirtualGeometryFrameViewIdentity> newestCompletedIdentity;
		const auto considerCompletedIdentity =
			[&newestCompletedIdentity](
				VirtualGeometryFrameViewIdentity candidate)
			{
				if(!newestCompletedIdentity.has_value()
					|| candidate.frameId > newestCompletedIdentity->frameId
					|| (candidate.frameId == newestCompletedIdentity->frameId
						&& candidate.viewId
							> newestCompletedIdentity->viewId))
				{
					newestCompletedIdentity = candidate;
				}
			};
		for(const VirtualGeometryBackendExecutionReceipt& receipt : receipts)
		{
			if(receipt.IsExecuted())
			{
				considerCompletedIdentity({
					receipt.frameNumber,
					receipt.viewId,
					receipt.rendererGeneration
				});
			}
		}
		for(const VirtualGeometryBackendMeasurement& measurement
			: backendMeasurements)
		{
			if(measurement.frameNumber != 0u
				&& measurement.rendererGeneration != 0u)
			{
				considerCompletedIdentity({
					measurement.frameNumber,
					measurement.viewId,
					measurement.rendererGeneration
				});
			}
		}
		if(newestCompletedIdentity.has_value())
		{
			diagnostics.publicationIdentity = *newestCompletedIdentity;
		}

		const bool meshShaderRoute =
			context.SupportsVirtualGeometryMeshShaderRoute();
		constexpr std::array<BackendStage, 13u> backendStages = {
			BackendStage::Traversal,
			BackendStage::Compaction,
			BackendStage::IndirectGeneration,
			BackendStage::PageDecode,
			BackendStage::HardwareRaster,
			BackendStage::ComputeRaster,
			BackendStage::VisibilityResolve,
			BackendStage::Shading,
			BackendStage::StreamingUpdate,
			BackendStage::Deformation,
			BackendStage::Shadow,
			BackendStage::Capture,
			BackendStage::RayTracing
		};
		diagnostics.capabilities.reserve(backendStages.size());
		for(const BackendStage backendStage : backendStages)
		{
			const CapabilityStage stage = ToCapabilityStage(backendStage);
			VirtualGeometryCapabilityKey key{};
			key.stage = stage;
			key.backend = profile.backendId == "vulkan"
				? VirtualGeometryCapabilityBackend::Vulkan
				: VirtualGeometryCapabilityBackend::Unknown;
			key.route = stage == CapabilityStage::Compaction
					|| stage == CapabilityStage::ComputeRaster
				? VirtualGeometryCapabilityRoute::ComputeRaster
				: VirtualGeometryCapabilityRoute::MeshShader;
			key.deviceIdentity = profile.backendId;
			VirtualGeometryCapabilityEvidence capability =
				MakePlannedVirtualGeometryCapability(std::move(key));
			const bool supported =
				BackendSupportsStage(stage, profile, meshShaderRoute);
			EvaluateVirtualGeometryCapabilitySupport(
				capability,
				supported,
				supported
					? "The active backend exposes the required live route."
					: "The active backend has not exposed a complete live route.",
				"profile-derived limits");
			capability.source = "backend-profile";
			if(!supported)
			{
				capability.fallbackRoute =
					VirtualGeometryCapabilityRoute::IndexedFallback;
				capability.fallbackReason =
					"The processor-expanded/indexed route remains authoritative.";
			}

			const auto completed = std::find_if(
				receipts.begin(),
				receipts.end(),
				[&](const VirtualGeometryBackendExecutionReceipt& receipt)
				{
					return receipt.stage == backendStage
						&& receipt.IsExecuted()
						&& VirtualGeometryFrameViewIdentity{
							receipt.frameNumber,
							receipt.viewId,
							receipt.rendererGeneration
						} == diagnostics.publicationIdentity;
				});
			if(completed != receipts.end())
			{
				VirtualGeometryCapabilityExecution execution{};
				execution.identity = diagnostics.publicationIdentity;
				execution.evidenceId = completed->evidenceId;
				execution.completedLiveCommand = true;
				execution.preparedOnly = false;
				(void)AttachVirtualGeometryExecutionEvidence(
					capability,
					std::move(execution));
				capability.source = "backend-completion-receipt";
			}
			diagnostics.capabilities.push_back(std::move(capability));
		}

		diagnostics.measurements.reserve(backendMeasurements.size());
		for(const VirtualGeometryBackendMeasurement& backendMeasurement
			: backendMeasurements)
		{
			VirtualGeometryMeasurementRecord measurement{};
			measurement.stage =
				ToCapabilityStage(backendMeasurement.stage);
			measurement.identity = {
				backendMeasurement.frameNumber,
				backendMeasurement.viewId,
				backendMeasurement.rendererGeneration
			};
			measurement.availability =
				ToMeasurementAvailability(backendMeasurement.availability);
			measurement.rejectionReason = backendMeasurement.reason;
			if(backendMeasurement.queryPoolGeneration != 0u
				&& backendMeasurement.queryCount != 0u)
			{
				measurement.queryIdentity =
					"pool-"
					+ std::to_string(
						backendMeasurement.queryPoolGeneration)
					+ "/"
					+ std::to_string(backendMeasurement.firstQuery)
					+ "+"
					+ std::to_string(backendMeasurement.queryCount);
			}

			const auto completed = std::find_if(
				receipts.begin(),
				receipts.end(),
				[&](const VirtualGeometryBackendExecutionReceipt& receipt)
				{
					return receipt.stage == backendMeasurement.stage
						&& receipt.frameNumber
							== backendMeasurement.frameNumber
						&& receipt.viewId == backendMeasurement.viewId
						&& receipt.rendererGeneration
							== backendMeasurement.rendererGeneration
						&& receipt.IsExecuted();
				});
			if(completed != receipts.end())
			{
				measurement.executionEvidenceId = completed->evidenceId;
			}
			if(backendMeasurement.availability
				== BackendMeasurementAvailability::Measured)
			{
				if(completed == receipts.end()
					|| !backendMeasurement.HasAuthoritativeValue())
				{
					measurement.availability =
						VirtualGeometryMeasurementAvailability::Invalid;
					measurement.rejectionReason =
						"measured backend timing lacks matching completed execution evidence";
				}
				else
				{
					measurement.rawTicks =
						backendMeasurement.rawTicks;
					measurement.durationNanoseconds =
						static_cast<std::uint64_t>(std::llround(
							backendMeasurement.durationMilliseconds
								* 1'000'000.0));
				}
			}
			diagnostics.measurements.push_back(std::move(measurement));
		}

		for(VirtualGeometryCapabilityEvidence& capability
			: diagnostics.capabilities)
		{
			if(!capability.CanReportExecuted())
			{
				continue;
			}
			const auto measured = std::find_if(
				diagnostics.measurements.begin(),
				diagnostics.measurements.end(),
				[&](const VirtualGeometryMeasurementRecord& measurement)
				{
					return measurement.stage == capability.key.stage
						&& measurement.identity
							== capability.execution.identity
						&& measurement.HasAuthoritativeDuration();
				});
			if(measured != diagnostics.measurements.end())
			{
				(void)AttachVirtualGeometryValidationEvidence(
					capability,
					{ measured->queryIdentity });
			}
		}
		return diagnostics;
	}
}

// RenderSystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Initialize()
{
	RenderExecutionBoundary boundary(frameExecutionBoundary);
	RetireFrameExecutionAtProtectedBoundary();
	// A failed initialization or a stopped backend cannot advertise readiness to
	// a later frame. Initialization runs at the existing protected lifecycle edge.
	initialized = false;
	TERRAENGINE_LOG(LogLevel::Info, "Initializing RenderSystem...");
	if (const auto startupSettings = ResourceManager::Instance().GetConfiguredRenderSettings()) {
		// Editor direct-project and export profiles are resolved before system
		// initialization. Do not rediscover settings from the process cwd here.
		SetRenderSettings(*startupSettings);
	} else {
		LoadSettings();
	}
	
	auto vulkanContext = std::make_unique<VulkanContext>();
	// Vulkan validation is controlled by project/export render settings rather
	// than by the compiler configuration. Request it before instance creation so
	// Release and packaged builds can use validation layers when developers turn
	// the profile flag on, while machines without the SDK still fall back cleanly.
	vulkanContext->SetValidationLayersRequested(ResourceManager::Instance().IsRenderValidationEnabled());
	// VRS is a render-pass attachment decision in Vulkan. Apply the startup
	// render profile before backend initialization so exported builds that
	// declare VRS disabled never create or load a shading-rate attachment.
		vulkanContext->SetVariableRateShadingEnabled(SanitizeRenderSettings(currentSettings).variableRateShading > 0u);
	rhi = std::move(vulkanContext);
	rendererRecoveryPolicy = ResourceManager::Instance().GetRendererRecoveryPolicy();
	rhi->Initialize(WindowSystem::Instance());

	WindowSystem::Instance().SetResizeCallback([](int width, int height) {
		try {
			auto& renderSystem = RenderSystem::Instance();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto* vulkanContext = dynamic_cast<VulkanContext*>(renderSystem.GetContext())) {
				vulkanContext->Resize(width, height);
			}
		} catch (const std::exception& e) {
			TERRAENGINE_LOG(LogLevel::Error, "Exception in resize callback: " << e.what());
		} catch (...) {
			TERRAENGINE_LOG(LogLevel::Error, "Unknown exception in resize callback");
		}
	});

	initialized = true;
}

// RenderSystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Update(float deltaTime)
{
	TERRAENGINE_PROFILE_SCOPE("RenderSystem::Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	RenderExecutionBoundary boundary(frameExecutionBoundary);
	UpdateAtExecutionBoundary(deltaTime);
}

void RenderSystem::RetireFrameExecutionAtProtectedBoundary()
{
	std::unique_ptr<RenderFrameExecution> retired;
	{
		std::lock_guard lock(lifetimeState->framePhaseMutex);
		auto& phase = lifetimeState->framePhase;
		retired = std::move(phase.execution);
		phase.captureReady = phase.routeReady = phase.virtualGeometryReady = false;
		phase.backendComplete = phase.publicationComplete = false;
	}
	// Cancellation destroys collection owners before the lease, outside handoff
	// and diagnostics locks. The outer admission gate proves no Resume is active.
	retired.reset();
	ObserveFrameScratchAtProtectedBoundary();
}

void RenderSystem::ObserveFrameScratchAtProtectedBoundary()
{
	const auto owner = lifetimeState->frameScratch;
	if(!owner) return;
	const auto terminal = owner->LastSnapshot();
	const auto reclamation = owner->ReclamationSnapshot();
	TerraEngine::Renderer::RenderFrameScratchDiagnostics latest;
	TerraEngine::Renderer::RenderFrameScratchDiagnostics previous;
	TerraEngine::Renderer::RenderFrameScratchReclamationDiagnostics reclaimed;
	{
		std::lock_guard lock(statsMutex);
		observedFrameScratchOwner = owner;
		if(terminal.episodeId != 0U) {
			if(latestFrameScratch.lifetimeId != terminal.lifetimeId
				|| latestFrameScratch.episodeId != terminal.episodeId) {
				previousFrameScratch = latestFrameScratch;
			}
			latestFrameScratch = terminal;
			latestFrameScratchReclamation = reclamation;
		}
		TerraEngine::Renderer::ApplyFrameScratchDiagnostics(latestRenderGraphDiagnostics,
			latestFrameScratch, previousFrameScratch, latestFrameScratchReclamation);
		latest = latestFrameScratch;
		previous = previousFrameScratch;
		reclaimed = latestFrameScratchReclamation;
	}
	TerraEngine::Renderer::PublishFrameScratchDiagnostics(latest, previous, reclaimed);
}

void RenderSystem::UpdateAtExecutionBoundary(float deltaTime)
{
	RetireFrameExecutionAtProtectedBoundary();
	try {
		RenderFrameExecution execution = BuildFrameExecution(deltaTime);
		while(execution.Resume()) {
			// Direct callers execute the same declared stages serially. All nested
			// borrowing jobs still settle before their owning stage returns.
		}
	} catch(...) {
		ObserveFrameScratchAtProtectedBoundary();
		throw;
	}
	ObserveFrameScratchAtProtectedBoundary();
}

RenderFrameExecution RenderSystem::BuildFrameExecution(float deltaTime)
{
	if (!initialized) {
		co_return;
	}
	if (!editorModeEnabled)
	{
		// A standalone runtime derives Game View state from authored scene
		// cameras every frame. Clear the previous descriptor before collection so
		// a scene switch to a camera-less scene cannot leave capture requests
		// pointing at a stale camera from the prior scene.
		hasLatestGameViewCamera = false;
		latestGameViewCamera = {};
	}
	// Runtime C# settings become visible only at this protected render boundary.
	// Applying before snapshot construction gives every view one coherent profile.
	ApplyQueuedRuntimeRenderSettings();
	DrainCaptureReadbacks();

	// Standalone runtime UI should track the real framebuffer size. In the
	// editor, however, the Game View is letterboxed inside an ImGui window and
	// owns its own viewport size, so forcing the full GLFW framebuffer here
	// would make UI layout and hit testing react to the wrong rectangle.
	if (!editorModeEnabled)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Sync Runtime Viewport", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto* windowHandle = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle()))
		{
			int framebufferWidth = 0;
			int framebufferHeight = 0;
			glfwGetFramebufferSize(windowHandle, &framebufferWidth, &framebufferHeight);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (framebufferWidth > 0 && framebufferHeight > 0)
			{
				UISystem::Instance().SetGameViewportSize(glm::vec2(static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight)));
			}
		}
	}

	// Only protected capture allocates/resets this unsynchronized resource. The
	// previous continuation was retired before entry; lease release never resets.
	if(!lifetimeState->frameScratch)
		lifetimeState->frameScratch = TerraEngine::Renderer::RenderFrameScratch::Create();
	auto scratchLease = lifetimeState->frameScratch->BeginEpisode(nextRenderFrameId);
	const auto scratchValidation = frameScratchValidationCallbacks;
	{
		std::lock_guard lock(statsMutex);
		observedFrameScratchOwner = lifetimeState->frameScratch;
	}

	// Snapshot camera components so view/projection data is sourced from
	// authored entities instead of the legacy hardcoded values.
	const std::uint64_t protectedRendererGeneration = rhi
		? rhi->GetLatestRendererRecoverySnapshot().renderSessionGeneration
		: latestRendererRecoverySnapshot.renderSessionGeneration;
	// Snapshot the backend object generation beside the renderer session. A
	// presentation recovery may replace swapchain-compatible resources without
	// changing the session, so every sealed root needs both identities.
	const std::uint64_t protectedBackendResourceGeneration = rhi
		? rhi->GetBackendResourceGeneration() : 1u;
	// The scheduler owns the one scene-change drain.  Retaining its immutable
	// packet here makes renderer preparation observe the same protected boundary
	// as Scene View and other consumers, without touching mutable scene journals.
	const std::shared_ptr<const TerraEngine::Scene::SceneChangePacket>
		protectedSceneChanges = objManager.LoadPublishedSceneChangePacketShared();
	const std::uint64_t protectedSceneGeneration =
		protectedSceneChanges != nullptr
			&& protectedSceneChanges->SceneGeneration() != 0u
			? protectedSceneChanges->SceneGeneration()
			: objManager.GetSceneGeneration();
	// Acquire one post-physics presentation root. Its strong authority reference
	// and backend-neutral GPU plan prevent this renderer operation from pairing a
	// newer pose/authority row with an older owner-to-packet lookup.
	const std::shared_ptr<const TerraEngine::Animation::AnimationPresentationFrame>
		animationPresentationFrame = TerraEngine::Animation::AnimationPresentationSystem::Instance()
			.LoadPublishedAnimationPresentationFrame();
	const std::shared_ptr<const TerraEngine::Animation::PublishedGpuAnimationPresentationPlan>
		gpuPresentation = animationPresentationFrame != nullptr
			&& animationPresentationFrame->IsValid()
			? animationPresentationFrame->gpuPlan : nullptr;
	std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutableGpuAnimationPlan;
	if(gpuPresentation != nullptr && !gpuPresentation->safeFallback
		&& gpuPresentation->loweringStatus == TerraEngine::Animation::GpuAnimationLoweringStatus::Ok
		&& gpuPresentation->planStatus == TerraEngine::Animation::GpuAnimationPlanStatus::Ok
		&& gpuPresentation->presentationGeneration != 0U
		&& gpuPresentation->sourceGeneration != 0U
		&& gpuPresentation->plan.generation != 0U
		&& !gpuPresentation->plan.WorkPackets().empty()) {
		immutableGpuAnimationPlan = std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan>(
			gpuPresentation, &gpuPresentation->plan);
	}
	const std::size_t denseAnimationOwnerCount = gpuPresentation != nullptr
		? gpuPresentation->OwnerObjectMappings().size() : 0U;
	const bool hasDenseAnimationTopologyCandidate = immutableGpuAnimationPlan != nullptr
		&& denseAnimationOwnerCount != 0U
		&& denseAnimationOwnerCount == immutableGpuAnimationPlan->WorkPackets().size()
		&& immutableGpuAnimationPlan->topologyInputSignature != 0U
		&& immutableGpuAnimationPlan->topologyRendererGeneration != 0U;
	auto& resourceManager = ResourceManager::Instance();
	const auto protectedRendererResourceOwnerRevision = resourceManager.GetRendererResourcePublicationGeneration();
	const std::uint64_t protectedTerrainProxyGeneration =
		TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
			.PublicationGeneration();
	// Capture reuse is independent of cameras and transforms. It covers only the
	// live ECS/resource work needed to rediscover render membership and animation
	// binding topology. Fully GPU-visual pose generations are intentionally absent:
	// the backend resolves them from the current immutable presentation root.
	std::uint64_t renderableCaptureSourceSignature = kRenderHashOffset;
	HashCombine(renderableCaptureSourceSignature, protectedSceneGeneration);
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetComponentMutationRevision());
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetComponentValueMutationRevision(
			MeshRenderComponent::kComponentName));
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetComponentValueMutationRevision(
			TerrainComponent::kComponentName));
	HashCombine(renderableCaptureSourceSignature, protectedTerrainProxyGeneration);
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetHierarchyRevision());
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetRuntimeActiveMembershipRevision());
	HashCombine(renderableCaptureSourceSignature,
		protectedRendererResourceOwnerRevision);
	HashCombine(renderableCaptureSourceSignature,
		resourceManager.GetAnimationBindingResourceRevision());
	HashCombine(renderableCaptureSourceSignature, protectedRendererGeneration);
	HashCombine(renderableCaptureSourceSignature,
		protectedBackendResourceGeneration);
	HashCombine(renderableCaptureSourceSignature,
		hasDenseAnimationTopologyCandidate && immutableGpuAnimationPlan != nullptr
			? immutableGpuAnimationPlan->topologyInputSignature
			: animationPresentationFrame != nullptr
				? animationPresentationFrame->presentationGeneration : 0U);
	if(renderableCaptureSourceSignature == 0U)
		renderableCaptureSourceSignature = 1U;
	// Retained authoring flags must observe the scene journal too. SetStatic
	// changes metadata/bounds without changing component or active revisions.
	// A newer metadata row (or a journal gap) requires the existing delta/full
	// capture; warm transform-only packets can retain binding topology.
	const bool renderableMetadataCaptureCurrent = protectedSceneChanges != nullptr
		&& protectedSceneChanges->Validate(protectedSceneGeneration).valid
		&& protectedSceneChanges->PublicationRevision() != 0U
		&& objManager.CanApplySceneChangePacketToLiveState(*protectedSceneChanges,
			protectedSceneChanges->PublicationRevision() - 1U)
		&& (protectedSceneChanges->PublicationRevision() == renderableMembershipPacketRevision
			|| (objManager.CanApplySceneChangePacketToLiveState(*protectedSceneChanges, renderableMembershipPacketRevision)
				&& protectedSceneChanges->MetadataChanged().empty()));
	const bool reuseProtectedRenderableCapture = renderableMetadataCaptureCurrent
		&& retainedRenderableEntriesReusable
		&& retainedRenderableCaptureSourceSignature
			== renderableCaptureSourceSignature
		&& renderableMembershipSceneGeneration == objManager.GetSceneGeneration()
		&& renderableMembershipComponentRevision
			== objManager.GetComponentMutationRevision()
		&& renderableMembershipHierarchyRevision
			== objManager.GetHierarchyRevision()
		&& renderableMembershipActiveRevision
			== objManager.GetRuntimeActiveMembershipRevision()
		&& renderableMembershipTerrainProxyGeneration
			== protectedTerrainProxyGeneration
		&& retainedRenderableEntries.size() == renderableMembershipCache.size();
	std::pmr::vector<CameraEntry> cameras(scratchLease.Resource());
	cameras.reserve(10);
	// This renderer-owned array crosses the coroutine's worker phases without
	// aliasing live ECS storage. A compatible frame retains its values; a changed
	// producer clears and reconstructs the complete array transactionally below.
	std::vector<RenderableEntry>& renderables = retainedRenderableEntries;
	if(!reuseProtectedRenderableCapture)
		renderables.clear();
	// Collection already classifies the final animation binding for every owner.
	// Preserve whether any CPU pose can need a host deformation bridge so the
	// later builder does not rescan a completely GPU-driven crowd to rediscover
	// that its output would be empty.
	bool hasCpuDeformationCandidates = false;
	// Successful CPU deformation records are normally a tiny exception inside a
	// GPU-resident crowd. Preserve their renderable indices so dynamic publication
	// can copy only the affected 256-lane pages instead of reopening every page.
	std::vector<std::uint32_t> cpuDeformationRenderableIndices;
	// Reuse the previous protected snapshot's cardinality as a conservative capacity
	// hint. Large animated crowds otherwise grow this frame-arena vector several
	// times before reaching the same stable entity count on every frame.
	const std::size_t expectedRenderableCount = std::max<std::size_t>(
		1000U, latestRenderFrameSnapshot.renderables.size());
	if(!reuseProtectedRenderableCapture)
		renderables.reserve(expectedRenderableCount);
	// Snapshot scene light components so packing can run in worker tasks without
	// touching the entity registry after this point.
	std::pmr::vector<LightEntry> lights(scratchLease.Resource());
	lights.reserve(256);
	// These protected-boundary containers are populated only for validated
	// canonical deformable meshes. Static TEMF records never touch the AnimationSystem
	// singleton; morph-only payloads participate only when their exact identity
	// palette and a compatible one-joint animation publication are both present.
	std::unordered_set<TerraEngine::UUID> aliveCompatibleAnimationObjects;
	// GPU-visual crowds do not retain CPU poses. Avoid allocating a large
	// retirement set on their common frame; an existing or newly created CPU-pose
	// cache grows this set on demand without affecting GPU-only collection.
	if(!compatibleAnimationPoseCache.empty()) {
		aliveCompatibleAnimationObjects.reserve(compatibleAnimationPoseCache.size());
	}
	std::optional<std::uint64_t> animationAuthorityGeneration;
	// Missing immutable publications keep capture open for next-frame async retries.
	bool renderableResourcePublicationsComplete = true;
	std::size_t runtimeLayoutRejectedRecordCount = 0u;
	std::vector<std::string> runtimeLayoutRejectionReasons;
	// CaptureImage enqueues a custom/camera-object request before the next render
	// boundary. Snapshot whether that request needs an independent offscreen view
	// now, because renderable collection happens before the request is drained
	// into its dedicated submission below.
	bool hasQueuedDedicatedCapture = false;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for(const TerraEngine::Renderer::RenderCaptureRequest& request
			: pendingCaptureRequests) {
			if((request.sourceKind
					!= TerraEngine::Renderer::RenderCaptureSourceKind::GameView
					|| request.dedicatedRenderTarget)
				&& request.format == "image/png") {
				hasQueuedDedicatedCapture = true;
				break;
			}
		}
	}

	// GPU-visual renderables used to linearly rescan every owner mapping and work
	// packet for every mesh. That is quadratic for crowds. These frame-local
	// immutable indexes retain all duplicate records so the fail-open ambiguity
	// checks below remain identical while ordinary lookups become constant-time.
	const auto findUniqueGpuAnimationIdentity = [&immutableGpuAnimationPlan, &gpuPresentation,
		protectedRendererGeneration, protectedSceneGeneration](
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request,
		std::size_t& ownerMappingHint)
		-> std::optional<TerraEngine::Renderer::AnimationGpuRenderBinding>
	{
		if(immutableGpuAnimationPlan == nullptr || gpuPresentation == nullptr
			|| protectedRendererGeneration == 0U
			|| protectedRendererGeneration > std::numeric_limits<std::uint32_t>::max()
			|| protectedSceneGeneration == 0U
			|| protectedSceneGeneration > std::numeric_limits<std::uint32_t>::max()
			|| immutableGpuAnimationPlan->RenderIdentityCount()
				!= immutableGpuAnimationPlan->WorkPackets().size()
			|| !request.expectedGpuRenderIdentity.IsComplete()) {
			return std::nullopt;
		}

		std::optional<TerraEngine::Animation::GpuAnimationOwnerObjectMapping> ownerMapping;
		const auto& ownerMappings = gpuPresentation->OwnerObjectMappings();
		const TerraEngine::Animation::GpuAnimationOwnerObjectMapping* ownerBegin = nullptr;
		if(ownerMappingHint < ownerMappings.size()
			&& ownerMappings[ownerMappingHint].ownerObjectId
				== request.animatorObjectId)
		{
			ownerBegin = ownerMappings.data() + ownerMappingHint;
		}
		else
		{
			ownerBegin = gpuPresentation->FindFirstOwnerObjectMapping(
				request.animatorObjectId);
			ownerMappingHint = ownerBegin != nullptr
				? static_cast<std::size_t>(ownerBegin - ownerMappings.data())
				: std::numeric_limits<std::size_t>::max();
		}
		const TerraEngine::Animation::GpuAnimationOwnerObjectMapping* ownerEnd =
			ownerMappings.data() + ownerMappings.size();
		for(const TerraEngine::Animation::GpuAnimationOwnerObjectMapping* ownerIt =
				ownerBegin;
			ownerIt != nullptr && ownerIt != ownerEnd
				&& ownerIt->ownerObjectId == request.animatorObjectId; ++ownerIt) {
			const TerraEngine::Animation::GpuAnimationOwnerObjectMapping& mapping = *ownerIt;
			if(!mapping.IsComplete() || mapping.ownerObjectId != request.animatorObjectId
				|| mapping.componentGeneration != request.componentGeneration
				|| !mapping.HasPublishedPacketIndex()) {
				continue;
			}
			if(ownerMapping.has_value()) {
				// Multiple immutable records for one component cannot prove which packet
				// belongs to this draw, so fail open rather than use a global guess.
				return std::nullopt;
			}
			ownerMapping = mapping;
		}
		if(!ownerMapping.has_value()) return std::nullopt;

		std::optional<TerraEngine::Renderer::AnimationGpuRenderBinding> match;
		const std::uint32_t rendererGeneration =
			static_cast<std::uint32_t>(protectedRendererGeneration);
		const std::uint32_t packetIndex = ownerMapping->workPacketIndex;
		if(packetIndex < immutableGpuAnimationPlan->WorkPackets().size()) {
			const TerraEngine::Animation::GpuAnimationWorkPacket& packet =
				immutableGpuAnimationPlan->WorkPackets()[packetIndex];
			const TerraEngine::Animation::GpuAnimationRenderIdentity& renderIdentity =
				immutableGpuAnimationPlan->RenderIdentity(packetIndex);
			if(packet.generationRecordIndex >= immutableGpuAnimationPlan->GenerationRecordCount()
				|| packet.stageMask == 0U
				|| !renderIdentity.IsComplete()
				|| renderIdentity != request.expectedGpuRenderIdentity) {
				return std::nullopt;
			}
			const TerraEngine::Animation::GpuAnimationGenerationRecord& generation =
				immutableGpuAnimationPlan->GenerationRecord(packet.generationRecordIndex);
			if(!TerraEngine::Animation::IsGpuAnimationGenerationRecordValid(generation)
				|| generation.instanceSlot == TerraEngine::Animation::kInvalidAnimatorInstanceSlot
				|| packet.stableInstanceId != ownerMapping->stableInstanceId
				|| generation.instanceSlot != ownerMapping->instanceSlot
				|| generation.instanceGeneration != ownerMapping->instanceGeneration
				|| generation.componentGeneration != ownerMapping->componentGeneration
				|| generation.componentGeneration != request.componentGeneration
				|| generation.meshGeneration != renderIdentity.meshHeaderVersion
				|| generation.sceneGeneration != protectedSceneGeneration
				|| generation.rendererGeneration != rendererGeneration
				|| packet.stableInstanceId != ((static_cast<std::uint64_t>(generation.instanceSlot) << 32U)
					| static_cast<std::uint64_t>(generation.instanceGeneration))) {
				return std::nullopt;
			}

			TerraEngine::Renderer::AnimationGpuRenderBinding candidate{};
			candidate.stableInstanceId = packet.stableInstanceId;
			candidate.presentationGeneration = gpuPresentation->presentationGeneration;
			candidate.sourceGeneration = gpuPresentation->sourceGeneration;
			candidate.paletteGeneration = gpuPresentation->presentationGeneration;
			candidate.planGeneration = immutableGpuAnimationPlan->generation;
			candidate.rendererGeneration = rendererGeneration;
			candidate.workPacketIndex = packetIndex;
			candidate.stableSubmissionOrdinal = packet.stableSubmissionOrdinal;
			candidate.instanceSlot = generation.instanceSlot;
			candidate.instanceGeneration = generation.instanceGeneration;
			candidate.componentGeneration = generation.componentGeneration;
			candidate.skeletonGeneration = generation.skeletonGeneration;
			candidate.meshGeneration = generation.meshGeneration;
			candidate.resourceGeneration = generation.resourceGeneration;
			candidate.sceneGeneration = generation.sceneGeneration;
			candidate.workGeneration = generation.workGeneration;
			candidate.programGeneration = generation.programGeneration;
			candidate.stageMask = packet.stageMask;
			candidate.renderIdentity = renderIdentity;
			candidate.hybridCpuAuthority = TerraEngine::Animation::HasGpuAnimationFlag(
				packet.flags,
				TerraEngine::Animation::GpuAnimationWorkFlag::HybridCpuAuthority);
			candidate.valid = true;
			if(match.has_value()) {
				// The plan ABI names one packet per animator. Without a mesh ID in the
				// packet, two candidates cannot be proven to belong to this draw even
				// when their submission ordinals differ, so reject fail-open.
				return std::nullopt;
			}
			match = candidate;
		}
		return match;
	};
	// Resource availability is stable throughout this protected render boundary.
	// Cache repeated mesh/material queries so thousands of instances sharing one
	// prefab do not repeatedly enter the resource manager for identical UUIDs.
	std::unordered_map<TerraEngine::UUID, bool> frameResourceAvailability;
	std::unordered_map<TerraEngine::UUID, const Mesh*>& frameMeshResources =
		retainedRuntimeMeshResources;
	std::unordered_map<TerraEngine::UUID, const Material*>& frameMaterialResources =
		retainedRuntimeMaterialResources;
	// Terrain material publications are copied while registry mutation is
	// protected. Later visibility work reads only this frame-owned table.
	std::unordered_map<TerraEngine::UUID,
		TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeBinding>
		frameTerrainMaterialBindings;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::SpatialAABB>&
		frameMeshLocalBounds = retainedMeshLocalBounds;
	std::optional<TerraEngine::Profiling::ProfilerFramePublicationEvidence>
		pendingProfilerPublicationEvidence;
	if(!reuseProtectedRenderableCapture)
	{
		frameMeshResources.clear();
		frameMaterialResources.clear();
		retainedRuntimeMeshOwners.clear();
		retainedRuntimeMaterialOwners.clear();
		frameMeshLocalBounds.clear();
	}
	frameResourceAvailability.reserve(32U);
	frameMeshResources.reserve(16U);
	frameMaterialResources.reserve(16U);
	frameMeshLocalBounds.reserve(16U);
	auto& registry = objManager.getRegistry();
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: ECS Collection Graph", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const bool diagnosticsEnabled = TerraEngine::Scheduling::EcsGraphFrameDiagnosticsEnabled();
		TerraEngine::Scheduling::EcsQueryTaskGraph renderCollectionGraph({ "render-system-collection", "Render System Collection", false, diagnosticsEnabled });

		renderCollectionGraph.AddQuery(MakeProtectedRenderQuery(
			"Render.CollectCameras",
			diagnosticsEnabled,
			{ "Transform", "CameraComponent" },
			{},
			{ "Render.CameraCollection" },
			[this, &cameras](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				objManager.forEachObjectWithComponents<Transform, CameraComponent>(
					[&](Object object, Transform&, CameraComponent& cameraComp)
					{
						CameraComponent reflectedCamera{};
						reflectedCamera.verticalFovDegrees = cameraComp.verticalFovDegrees;
						reflectedCamera.aspectRatio = cameraComp.aspectRatio;
						reflectedCamera.nearPlane = cameraComp.nearPlane;
						reflectedCamera.farPlane = cameraComp.farPlane;
						reflectedCamera.up = cameraComp.up;
						cameras.push_back({ object.EntityId(), reflectedCamera });
					}
				);
			}));

		renderCollectionGraph.AddQuery(MakeProtectedRenderQuery(
			"Render.CollectRenderables",
			diagnosticsEnabled,
			{ "Transform", "MeshRenderComponent", "TerrainComponent", "AnimatorComponent" },
			{ "Render.CameraCollection", TerraEngine::Scheduling::kResourceAnimatorComponents,
				TerraEngine::Scheduling::kResourceAnimationRuntimeState },
			// Protected admission records streaming needs and may accept ready resource publications.
			{ "Render.RenderableCollection", "Resources", "StreamingNeeds" },
			[&cameras, &renderables, &aliveCompatibleAnimationObjects,
				&hasCpuDeformationCandidates,
				&hasQueuedDedicatedCapture,
				&animationAuthorityGeneration,
				&animationPresentationFrame,
				&protectedSceneChanges,
				&runtimeLayoutRejectedRecordCount, &runtimeLayoutRejectionReasons,
				&renderableResourcePublicationsComplete,
				&findUniqueGpuAnimationIdentity, &frameResourceAvailability,
				&frameMeshResources, &frameMaterialResources,
				&frameMeshLocalBounds, &resourceManager, &registry,
				protectedRendererGeneration,
				protectedSceneGeneration, protectedTerrainProxyGeneration,
				reuseProtectedRenderableCapture,
				renderableMetadataCaptureCurrent,
				this](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Renderables", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				if (cameras.empty() && !hasSceneViewCamera
					&& !hasQueuedDedicatedCapture)
				{
					// No viewport or dedicated capture can consume this snapshot.
					// Preserve the old no-work fast path for truly headless frames.
					return;
				}
				if(reuseProtectedRenderableCapture)
				{
					// Resource, component, hierarchy, renderer, and animation-topology
					// generations match the prior complete capture. Current GPU pose data
					// is resolved later from the new presentation root, so no ECS or
					// ResourceManager lookup is required for this crowd.
					if(protectedSceneChanges != nullptr
						&& protectedSceneChanges->Validate(protectedSceneGeneration).valid)
					{
						renderableMembershipPacketRevision =
							protectedSceneChanges->PublicationRevision();
					}
					return;
				}
				const std::uint64_t membershipSceneGeneration =
					objManager.GetSceneGeneration();
				const std::uint64_t membershipComponentRevision =
					objManager.GetComponentMutationRevision();
				const std::uint64_t membershipHierarchyRevision =
					objManager.GetHierarchyRevision();
				const std::uint64_t membershipActiveRevision =
					objManager.GetRuntimeActiveMembershipRevision();
				const auto resolveRenderBinding = [&](entt::entity objectEntity)
					-> std::optional<MeshRenderComponent> {
					// Terrain owns a specialized derived binding. When TerrainComponent is
					// present it is authoritative, so a stale legacy mesh component cannot
					// accidentally render a second copy of the same surface.
					if(const TerrainComponent* terrain = registry.try_get<TerrainComponent>(objectEntity))
					{
						if(!IsRuntimeComponentActive(*terrain) || !terrain->visible
							|| terrain->terrainAssetId == TerraEngine::EMPTY_UUID)
						{
							return std::nullopt;
						}
						const auto proxy = TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
							.Resolve(terrain->terrainAssetId);
						if(!proxy || !proxy->IsComplete()) return std::nullopt;
						MeshRenderComponent binding;
						binding.meshId = proxy->meshId;
						binding.materialId = proxy->materialId;
						binding.spatialObjectId = TerraEngine::EMPTY_UUID;
						// Direct terrain meshes use complete per-area bounds and therefore do
						// not rely on generic task-shader culling metadata.
						binding.taskShaderCullingEnabled = false;
						return binding;
					}
					if(const MeshRenderComponent* mesh = registry.try_get<MeshRenderComponent>(objectEntity);
						mesh != nullptr && IsRuntimeComponentActive(*mesh))
					{
						return *mesh;
					}
					return std::nullopt;
				};
				const auto captureMembership = [&](Object object)
					-> std::optional<RenderableMembershipCacheRecord> {
					const entt::entity objectEntity = object.EntityId();
					if(!registry.valid(objectEntity)
						|| !registry.all_of<Transform>(objectEntity)
						|| !objManager.isObjectActiveInHierarchy(object)) {
						return std::nullopt;
					}
					const Transform& transform = registry.get<Transform>(objectEntity);
					const std::optional<MeshRenderComponent> resolvedBinding =
						resolveRenderBinding(objectEntity);
					if(!IsRuntimeComponentActive(transform) || !resolvedBinding) {
						return std::nullopt;
					}
					const MeshRenderComponent& meshComp = *resolvedBinding;
					RenderableMembershipCacheRecord cached;
					cached.object = object;
					cached.objectId = meshComp.spatialObjectId != TerraEngine::EMPTY_UUID
						? meshComp.spatialObjectId : objManager.getUUID(object);
					if(const TerrainComponent* terrain = registry.try_get<TerrainComponent>(objectEntity);
						terrain != nullptr && IsRuntimeComponentActive(*terrain) && terrain->visible)
					{
						cached.terrainAssetId = terrain->terrainAssetId;
					}
					cached.mesh = meshComp;
					cached.objectIdentity = static_cast<std::uint64_t>(
						std::hash<TerraEngine::UUID>{}(cached.objectId));
					cached.meshIdentity = static_cast<std::uint64_t>(
						std::hash<TerraEngine::UUID>{}(meshComp.meshId));
					cached.materialIdentity = std::max<std::uint64_t>(1U,
						static_cast<std::uint64_t>(
							std::hash<TerraEngine::UUID>{}(meshComp.materialId)));
					cached.objectStatic = objManager.isObjectStatic(object);
					std::optional<Object> hierarchyObject = object;
					for(std::size_t depth = 0U;
						hierarchyObject.has_value() && depth < 1024U; ++depth) {
						const entt::entity entity = hierarchyObject->EntityId();
						if(registry.valid(entity)
							&& registry.all_of<AnimatorComponent>(entity)) {
							const AnimatorComponent& animator =
								registry.get<AnimatorComponent>(entity);
							if(IsRuntimeComponentActive(animator)) {
								cached.nearestAnimatorObject = hierarchyObject;
								cached.animatorObjectId = objManager.getUUID(*hierarchyObject);
								cached.animatorObjectLifetime =
									objManager.getObjectLifetimeGeneration(*hierarchyObject);
								const std::uint64_t componentGeneration =
									objManager.getObjectComponentGeneration(*hierarchyObject);
								if(componentGeneration <=
									std::numeric_limits<std::uint32_t>::max()) {
									cached.animatorComponentGeneration =
										static_cast<std::uint32_t>(componentGeneration);
								}
								cached.skeletonOverrideAssetId =
									animator.skeletonOverrideAssetId;
							}
							break;
						}
						hierarchyObject = objManager.getParent(*hierarchyObject);
					}
					return cached;
				};
				const bool membershipChanged =
					!renderableMetadataCaptureCurrent
					|| renderableMembershipSceneGeneration != membershipSceneGeneration
					|| renderableMembershipComponentRevision != membershipComponentRevision
					|| renderableMembershipHierarchyRevision != membershipHierarchyRevision
					|| renderableMembershipActiveRevision != membershipActiveRevision
					|| renderableMembershipTerrainProxyGeneration
						!= protectedTerrainProxyGeneration;
				if(membershipChanged) {
					const bool terrainProxyChanged =
						renderableMembershipTerrainProxyGeneration
							!= protectedTerrainProxyGeneration;
					const bool sameSceneAndHierarchy =
						renderableMembershipSceneGeneration == membershipSceneGeneration
						&& renderableMembershipHierarchyRevision == membershipHierarchyRevision;
					// A consecutive packet can still predate a same-frame Inspector
					// edit. Patch only if its journal covers the live state being
					// captured; otherwise rebuild once at this protected boundary.
					const bool packetIsNext = protectedSceneChanges != nullptr
						&& objManager.CanApplySceneChangePacketToLiveState(
							*protectedSceneChanges, renderableMembershipPacketRevision);
					// A proxy publication can replace the mesh or material for every
					// instance of one terrain asset without producing an ECS change packet.
					// Rebuild the protected membership snapshot in that case instead of
					// retaining stale derived bindings through the object-delta fast path.
					const bool applyDelta = !terrainProxyChanged
						&& sameSceneAndHierarchy && packetIsNext
						&& !renderableMembershipCache.empty();
					if(applyDelta) {
						std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot>
							changedObjects;
						const auto appendChanged = [&changedObjects](const auto& changes) {
							changedObjects.insert(changedObjects.end(), changes.begin(), changes.end());
						};
						appendChanged(protectedSceneChanges->Added());
						appendChanged(protectedSceneChanges->Removed());
						appendChanged(protectedSceneChanges->MetadataChanged());
						appendChanged(protectedSceneChanges->ComponentChanged());
						std::sort(changedObjects.begin(), changedObjects.end(),
							[](const auto& left, const auto& right) {
								return left.objectId < right.objectId;
							});
						for(std::size_t changeIndex = 0U;
							changeIndex < changedObjects.size();) {
							const TerraEngine::UUID changedId =
								changedObjects[changeIndex].objectId;
							Object candidate = changedObjects[changeIndex].object;
							std::size_t nextChange = changeIndex + 1U;
							while(nextChange < changedObjects.size()
								&& changedObjects[nextChange].objectId == changedId) {
								if(registry.valid(changedObjects[nextChange].object.EntityId())) {
									candidate = changedObjects[nextChange].object;
								}
								++nextChange;
							}
							const auto existing = std::lower_bound(
								renderableMembershipCache.begin(), renderableMembershipCache.end(),
								changedId, [](const RenderableMembershipCacheRecord& record,
									const TerraEngine::UUID& id) { return record.objectId < id; });
							if(existing != renderableMembershipCache.end()
								&& existing->objectId == changedId) {
								renderableMembershipCache.erase(existing);
							}
							if(const auto refreshed = captureMembership(candidate);
								refreshed.has_value()) {
								const auto insertion = std::lower_bound(
									renderableMembershipCache.begin(), renderableMembershipCache.end(),
									refreshed->objectId,
									[](const RenderableMembershipCacheRecord& record,
										const TerraEngine::UUID& id) { return record.objectId < id; });
								renderableMembershipCache.insert(insertion, *refreshed);
							}
							changeIndex = nextChange;
						}
					}
					else {
						// A hierarchy mutation can affect every descendant's animator owner.
						// A missed packet can hide intermediate removals. Both cases require
						// one complete protected rebuild rather than speculative patching.
						renderableMembershipCache.clear();
						objManager.forEachObjectWithComponents<Transform, MeshRenderComponent>(
							[&](Object object, Transform&, MeshRenderComponent&) {
								if(registry.any_of<TerrainComponent>(object.EntityId())) return;
								if(const auto captured = captureMembership(object);
									captured.has_value()) {
									renderableMembershipCache.push_back(*captured);
								}
							});
						objManager.forEachObjectWithComponents<Transform, TerrainComponent>(
							[&](Object object, Transform&, TerrainComponent&) {
								if(const auto captured = captureMembership(object);
									captured.has_value()) {
									renderableMembershipCache.push_back(*captured);
								}
							});
					}
					// The immutable snapshot and every index-aligned render working set use
					// object-id order. Canonicalize membership only when authored structure
					// changes so stable crowds do not sort thousands of records every frame.
					std::sort(renderableMembershipCache.begin(),
						renderableMembershipCache.end(),
						[](const RenderableMembershipCacheRecord& left,
							const RenderableMembershipCacheRecord& right) {
							return left.objectId < right.objectId;
						});
					renderableMembershipSceneGeneration = membershipSceneGeneration;
					renderableMembershipComponentRevision = membershipComponentRevision;
					renderableMembershipHierarchyRevision = membershipHierarchyRevision;
					renderableMembershipActiveRevision = membershipActiveRevision;
					renderableMembershipTerrainProxyGeneration =
						protectedTerrainProxyGeneration;
					renderableMembershipPacketRevision = protectedSceneChanges != nullptr
						? protectedSceneChanges->PublicationRevision() : 0U;
				}
				const auto resourceAvailable = [&frameResourceAvailability, &resourceManager](
					const TerraEngine::UUID& resourceId)
				{
					const auto cachedResource = frameResourceAvailability.find(resourceId);
					if(cachedResource != frameResourceAvailability.end()) {
						return cachedResource->second;
					}
					const bool available = resourceManager.HasResource(resourceId);
					frameResourceAvailability.emplace(resourceId, available);
					return available;
				};
				TerraEngine::UUID lastAvailabilityMeshId = TerraEngine::EMPTY_UUID;
				TerraEngine::UUID lastAvailabilityMaterialId = TerraEngine::EMPTY_UUID;
				bool lastMeshAvailable = false;
				bool lastMaterialAvailable = false;
				const Mesh* lastRuntimeMesh = nullptr;
				bool lastRuntimeMeshDeformable = false;
				TerraEngine::Animation::AnimationCompatibilityFingerprint
					lastRuntimeMeshSkeletonFingerprint = 0U;
				for(RenderableMembershipCacheRecord& cached
					: renderableMembershipCache) {
						const Object& object = cached.object;
						const MeshRenderComponent& meshComp = cached.mesh;
						const TerraEngine::UUID meshId = meshComp.meshId;
						const TerraEngine::UUID materialId = meshComp.materialId;
						if (meshId == TerraEngine::EMPTY_UUID || materialId == TerraEngine::EMPTY_UUID)
						{
							++runtimeLayoutRejectedRecordCount;
							runtimeLayoutRejectionReasons.push_back("missing-render-resource-id");
							continue;
						}

						if(lastAvailabilityMeshId != meshId) {
							lastAvailabilityMeshId = meshId;
							lastMeshAvailable = resourceAvailable(meshId);
							const auto cachedMesh = frameMeshResources.find(meshId);
							if(cachedMesh != frameMeshResources.end()) {
								lastRuntimeMesh = cachedMesh->second;
							} else {
								const auto owner = resourceManager.AcquireMeshSnapshot(meshId);
								if(!owner || owner->publicationGeneration == 0U)
								{
									// Demand every active membership before camera/light culling, including
									// off-camera shadow casters. The frame map deduplicates this ID; native
									// admission owns budgeting and job coalescing without blocking capture.
									renderableResourcePublicationsComplete = false;
									(void)resourceManager.RequestStreamableLoad(meshId, AssetType::Mesh,
										TerraEngine::Resource::StreamingNeedType::Visible, nextRenderFrameId,
										"renderer-missing-mesh-publication", cached.objectId);
								}
								retainedRuntimeMeshOwners[meshId] = owner;
								lastRuntimeMesh = owner.get();
								frameMeshResources.emplace(meshId, lastRuntimeMesh);
							}
							if(!frameMeshLocalBounds.contains(meshId))
							{
								frameMeshLocalBounds.emplace(meshId,
									resourceManager.GetMeshLocalBounds(meshId));
							}
							lastRuntimeMeshDeformable = lastRuntimeMesh != nullptr
								&& IsValidatedAnimationDeformableMesh(*lastRuntimeMesh);
							lastRuntimeMeshSkeletonFingerprint = lastRuntimeMesh != nullptr
								? ResolveDeformableSkeletonFingerprint(*lastRuntimeMesh, nullptr)
								: 0U;
						}
						if(lastAvailabilityMaterialId != materialId) {
							lastAvailabilityMaterialId = materialId;
							lastMaterialAvailable = resourceAvailable(materialId);
							if(!frameMaterialResources.contains(materialId))
							{
								const auto owner = resourceManager.AcquireMaterialSnapshot(materialId);
								if(!owner || owner->publicationGeneration == 0U)
								{
									// Retain the missing result honestly for this immutable frame. A later
									// accepted publication, rather than the request itself, enables reuse.
									renderableResourcePublicationsComplete = false;
									(void)resourceManager.RequestStreamableLoad(materialId, AssetType::Material,
										TerraEngine::Resource::StreamingNeedType::Visible, nextRenderFrameId,
										"renderer-missing-material-publication", cached.objectId);
								}
								retainedRuntimeMaterialOwners[materialId] = owner;
								frameMaterialResources.emplace(materialId, owner.get());
							}
						}
						if (!lastMeshAvailable || !lastMaterialAvailable)
						{
							++runtimeLayoutRejectedRecordCount;
							runtimeLayoutRejectionReasons.push_back("resource-not-loaded");
							continue;
						}

						const TerraEngine::UUID objectId = cached.objectId;

						TerraEngine::Renderer::AnimationRenderBinding animationBinding;
						const Mesh* runtimeMesh = lastRuntimeMesh;
						if(runtimeMesh != nullptr && lastRuntimeMeshDeformable) {
							animationBinding.meshId = meshId;
							animationBinding.deformablePayloadGeneration =
								runtimeMesh->deformablePayloadGeneration;
							animationBinding.skeletonCompatibilityFingerprint =
								runtimeMesh->deformableMetadata.skeletonCompatibilityFingerprint;
							animationBinding.pageLocalDeformationAvailable =
								runtimeMesh->hasDeformableDemandedPages
								&& runtimeMesh->virtualGeometryPageStreamingReady;

							const std::optional<Object>& nearestAnimatorObject =
								cached.nearestAnimatorObject;
							const TerraEngine::UUID skeletonOverrideAssetId =
								cached.skeletonOverrideAssetId;

							if(nearestAnimatorObject.has_value()) {
								const TerraEngine::UUID animatorObjectId = cached.animatorObjectId;
								const std::uint64_t animatorObjectLifetime =
									cached.animatorObjectLifetime;
								const std::uint64_t componentGeneration64 =
									cached.animatorComponentGeneration;
								if(objectId != TerraEngine::EMPTY_UUID
									&& animatorObjectId != TerraEngine::EMPTY_UUID
									&& animatorObjectLifetime != 0U
									&& componentGeneration64 != 0U
									&& componentGeneration64 <= std::numeric_limits<std::uint32_t>::max()) {
									const std::uint32_t componentGeneration =
										static_cast<std::uint32_t>(componentGeneration64);
									if(!animationAuthorityGeneration.has_value()
										&& animationPresentationFrame != nullptr
										&& animationPresentationFrame->IsValid()) {
										// The presentation root owns the exact authority it represents.
										// Do not reopen AnimationSystem and risk a mixed generation.
										animationAuthorityGeneration =
											animationPresentationFrame->authority->authorityGeneration;
									}
									// GPU-visual crowds do not publish CPU poses. Prove their complete
									// packet identity before consulting either CPU-pose hash table; the
									// protected cache-retirement pass below still removes a stale held
									// pose if an animator changed from CPU authority to GPU visual.
									TerraEngine::Renderer::AnimationRenderBindingRequest gpuVisualRequest;
									gpuVisualRequest.meshId = meshId;
									gpuVisualRequest.animatorObjectId = animatorObjectId;
									gpuVisualRequest.animatorObjectGeneration = animatorObjectLifetime;
									gpuVisualRequest.animationAuthorityGeneration =
										animationAuthorityGeneration.value_or(0U);
									gpuVisualRequest.deformablePayloadGeneration =
										runtimeMesh->deformablePayloadGeneration;
									gpuVisualRequest.componentGeneration = componentGeneration;
									gpuVisualRequest.skeletonCompatibilityFingerprint =
										lastRuntimeMeshSkeletonFingerprint;
									gpuVisualRequest.pageLocalDeformationAvailable =
										runtimeMesh->hasDeformableDemandedPages
										&& runtimeMesh->virtualGeometryPageStreamingReady;
									gpuVisualRequest.expectedGpuRenderIdentity.meshId = meshId;
									gpuVisualRequest.expectedGpuRenderIdentity.meshHeaderVersion =
										runtimeMesh->runtimeLayout.version;
									gpuVisualRequest.expectedGpuRenderIdentity.deformablePayloadGeneration =
										runtimeMesh->deformablePayloadGeneration;
									gpuVisualRequest.expectedGpuRenderIdentity.rendererGeneration =
										protectedRendererGeneration;
									gpuVisualRequest.expectedGpuRenderIdentity.sceneGeneration =
										protectedSceneGeneration;
									gpuVisualRequest.expectedGpuRenderIdentity.skeletonCompatibilityFingerprint =
										gpuVisualRequest.skeletonCompatibilityFingerprint;
									gpuVisualRequest.invalidation.asset =
										runtimeMesh->deformablePayloadGeneration
										^ gpuVisualRequest.skeletonCompatibilityFingerprint;
									gpuVisualRequest.invalidation.page =
										runtimeMesh->deformablePayloadGeneration;
									gpuVisualRequest.invalidation.view = nextRenderFrameId;
									gpuVisualRequest.invalidation.material =
										cached.materialIdentity;
									gpuVisualRequest.invalidation.renderer = protectedRendererGeneration;
									gpuVisualRequest.invalidation.residency =
										runtimeMesh->deformablePayloadGeneration;
									gpuVisualRequest.gpuVisualIdentity =
										findUniqueGpuAnimationIdentity(
											gpuVisualRequest, cached.gpuOwnerMappingHint);
									const bool hasDirectGpuVisualBinding =
										gpuVisualRequest.gpuVisualIdentity.has_value()
										&& !gpuVisualRequest.gpuVisualIdentity->hybridCpuAuthority;
					if(hasDirectGpuVisualBinding) {
						animationBinding = ResolveAnimationRenderBinding(
							gpuVisualRequest);
									} else {

									const TerraEngine::Animation::AnimationAuthorityRow*
										authorityRow = nullptr;
									if(animationPresentationFrame != nullptr
										&& animationPresentationFrame->IsValid()) {
										authorityRow = animationPresentationFrame
											->FindAuthorityRowForOwner(animatorObjectId);
									}

									auto held = compatibleAnimationPoseCache.find(objectId);
									const TerraEngine::Animation::AnimationAuthorityRow*
										compatibilityAuthorityRow = authorityRow;
									if(compatibilityAuthorityRow == nullptr
										&& held != compatibleAnimationPoseCache.end()) {
										compatibilityAuthorityRow =
											held->second.binding.authorityRow;
									}
									const TerraEngine::Animation::AnimationCompatibilityFingerprint
										effectiveSkeletonFingerprint =
											ResolveDeformableSkeletonFingerprint(
												*runtimeMesh,
											compatibilityAuthorityRow);

									TerraEngine::Renderer::AnimationRenderBindingRequest request;
									request.meshId = meshId;
									request.animatorObjectId = animatorObjectId;
									request.animatorObjectGeneration = animatorObjectLifetime;
									request.animationAuthorityGeneration =
										animationAuthorityGeneration.value_or(0U);
									request.deformablePayloadGeneration = runtimeMesh->deformablePayloadGeneration;
									request.componentGeneration = componentGeneration;
									request.skeletonCompatibilityFingerprint =
										effectiveSkeletonFingerprint;
									request.pageLocalDeformationAvailable =
										runtimeMesh->hasDeformableDemandedPages
										&& runtimeMesh->virtualGeometryPageStreamingReady;
							request.expectedGpuRenderIdentity.meshId = meshId;
							request.expectedGpuRenderIdentity.meshHeaderVersion =
								// Bindings compare the already decoded runtime layout, not an
								// obsolete serialized-header alias. This value changes whenever
								// the renderer-visible meshlet contract changes.
								runtimeMesh->runtimeLayout.version;
									request.expectedGpuRenderIdentity.deformablePayloadGeneration =
										runtimeMesh->deformablePayloadGeneration;
									request.expectedGpuRenderIdentity.rendererGeneration =
										protectedRendererGeneration;
									request.expectedGpuRenderIdentity.sceneGeneration =
										protectedSceneGeneration;
									request.expectedGpuRenderIdentity.skeletonCompatibilityFingerprint =
										effectiveSkeletonFingerprint;
									request.invalidation.asset = runtimeMesh->deformablePayloadGeneration
										^ effectiveSkeletonFingerprint;
									request.invalidation.page = runtimeMesh->deformablePayloadGeneration;
									request.invalidation.view = nextRenderFrameId;
									request.invalidation.material = cached.materialIdentity;
									request.invalidation.renderer = protectedRendererGeneration;
									request.invalidation.residency = runtimeMesh->deformablePayloadGeneration;
									request.presentationFrame = animationPresentationFrame;
									request.authorityRow = authorityRow;

									if(authorityRow != nullptr) {
										const TerraEngine::Animation::AnimatorInstanceHandle animator =
											authorityRow->key.Handle();
										request.animatorSlotIndex = animator.slotIndex;
										request.animatorGeneration = animator.generation;
										request.skeletonAssetId = skeletonOverrideAssetId != TerraEngine::EMPTY_UUID
											? skeletonOverrideAssetId
											: authorityRow->skeleton.assetId.value;
										request.skeletonGeneration = authorityRow->skeleton.generation;
									} else if(held != compatibleAnimationPoseCache.end()) {
										const TerraEngine::Renderer::AnimationRenderBinding& prior = held->second.binding;
										const bool liveIdentityMatches = prior.meshId == request.meshId
											&& prior.animatorObjectId == request.animatorObjectId
											&& prior.animatorObjectGeneration == request.animatorObjectGeneration
											&& prior.animationAuthorityGeneration == request.animationAuthorityGeneration
											&& prior.deformablePayloadGeneration == request.deformablePayloadGeneration
											&& prior.componentGeneration == request.componentGeneration
											&& prior.skeletonCompatibilityFingerprint
												== request.skeletonCompatibilityFingerprint
											&& (skeletonOverrideAssetId == TerraEngine::EMPTY_UUID
												|| prior.skeletonAssetId == skeletonOverrideAssetId);
										if(liveIdentityMatches) {
											request.animatorSlotIndex = prior.animatorSlotIndex;
											request.animatorGeneration = prior.animatorGeneration;
											request.skeletonAssetId = prior.skeletonAssetId;
											request.skeletonGeneration = prior.skeletonGeneration;
											request.fallbackPolicy = TerraEngine::Renderer::AnimationRenderFallbackPolicy::HoldLastCompatiblePose;
											request.heldBinding = prior;
										} else {
											compatibleAnimationPoseCache.erase(held);
											held = compatibleAnimationPoseCache.end();
										}
									}
									// The published owner mapping names one exact packet for both
									// Visual GPU and Hybrid. Resolve it while all mesh identity is
									// already hot instead of rescanning every binding in a later pass.
									request.gpuVisualIdentity =
										findUniqueGpuAnimationIdentity(
											request, cached.gpuOwnerMappingHint);

									animationBinding = ResolveAnimationRenderBinding(request);
									if(authorityRow != nullptr
										&& animationBinding.status == TerraEngine::Renderer::AnimationRenderBindingStatus::Ready
										&& animationBinding.source == TerraEngine::Renderer::AnimationRenderPoseSource::CpuPose
										&& request.gpuVisualIdentity.has_value()
										&& request.gpuVisualIdentity->hybridCpuAuthority) {
										// Hybrid keeps CPU authority while the exact GPU packet supplies
										// deformation. The helper above already validated every generation.
										animationBinding.gpu = *request.gpuVisualIdentity;
									}
									if(animationBinding.status == TerraEngine::Renderer::AnimationRenderBindingStatus::Ready
										&& animationBinding.source == TerraEngine::Renderer::AnimationRenderPoseSource::CpuPose) {
										compatibleAnimationPoseCache[objectId] = {
											animationBinding, nextRenderFrameId };
										aliveCompatibleAnimationObjects.insert(objectId);
									} else if(animationBinding.status == TerraEngine::Renderer::AnimationRenderBindingStatus::Ready
										&& animationBinding.source == TerraEngine::Renderer::AnimationRenderPoseSource::HeldCpuPose
										&& held != compatibleAnimationPoseCache.end()) {
										held->second.lastTouchedFrameId = nextRenderFrameId;
										aliveCompatibleAnimationObjects.insert(objectId);
									} else if(held != compatibleAnimationPoseCache.end()) {
										// The lookup above already proved whether a held CPU pose exists.
										// Erase by iterator so a non-CPU binding does not hash the same
										// object identity twice on every frame.
										compatibleAnimationPoseCache.erase(held);
									}
									}
								}
							}
						}

						hasCpuDeformationCandidates =
							hasCpuDeformationCandidates
							|| (!animationBinding.gpu.IsComplete()
								&& animationBinding.HasCpuPose()
								&& animationBinding.authorityRow != nullptr);
					renderables.push_back({
							object.EntityId(), objectId, cached.terrainAssetId,
							cached.objectIdentity,
							cached.meshIdentity, cached.materialIdentity,
							std::max<std::uint64_t>(1U,
								objManager.GetObjectSpatialRevision(object)),
							cached.objectStatic, &cached.mesh, runtimeMesh,
							std::move(animationBinding) });
					}
			}));

		renderCollectionGraph.AddQuery(MakeProtectedRenderQuery(
			"Render.CollectLights",
			diagnosticsEnabled,
			{ "Transform", "LightComponent" },
			{},
			{ "Render.LightCollection" },
			[this, &lights](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Lights", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				objManager.forEachObjectWithComponents<Transform, LightComponent>(
					[&](Object object, Transform&, LightComponent& lightComp)
					{
					LightComponent reflectedLight{};
					const std::uint32_t encodedType = static_cast<std::uint32_t>(lightComp.type);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (encodedType > static_cast<std::uint32_t>(LightType::Area))
					{
						// Unsupported legacy light types are intentionally skipped so they
						// are not silently remapped to another runtime behavior.
						return;
					}
					reflectedLight.type = static_cast<LightType>(encodedType);

					const std::uint32_t encodedUnit = static_cast<std::uint32_t>(lightComp.unit);
					reflectedLight.unit = static_cast<LightUnit>(encodedUnit);

					const std::uint32_t encodedAppearance = static_cast<std::uint32_t>(lightComp.appearance);
					reflectedLight.appearance = static_cast<LightAppearance>(encodedAppearance);

					reflectedLight.color = lightComp.color;
					reflectedLight.temperature = lightComp.temperature;
					reflectedLight.intensity = lightComp.intensity;
					reflectedLight.range = lightComp.range;
					reflectedLight.innerConeAngle = lightComp.innerConeAngle;
					reflectedLight.outerConeAngle = lightComp.outerConeAngle;
					reflectedLight.width = lightComp.width;
					reflectedLight.height = lightComp.height;
					reflectedLight.castsShadows = lightComp.castsShadows;
					reflectedLight.shadowStrength = lightComp.shadowStrength;
					reflectedLight.shadowBias = lightComp.shadowBias;
					reflectedLight.indirectMultiplier = lightComp.indirectMultiplier;
					reflectedLight.authoredPriority = lightComp.authoredPriority;
					reflectedLight.stabilityPreference = lightComp.stabilityPreference;

					lights.push_back({ object.EntityId(), objManager.getUUID(object), reflectedLight });
					}
				);
			}));

		const TerraEngine::Scheduling::EcsSystemGraphFrameSummary& graphSummary = renderCollectionGraph.Execute();
		if (diagnosticsEnabled)
		{
			TerraEngine::Scheduling::PublishEcsGraphFrameSummary(graphSummary);
		}
		if (!graphSummary.valid)
		{
			TERRAENGINE_LOG(LogLevel::Error, TerraEngine::Scheduling::FormatEcsSystemGraphFrameSummary(graphSummary));
		}
	}
	// Validation copies actual captured values before any worker can borrow the
	// PMR arrays. Disabled native hooks add no copy or digest to ordinary frames.
	TerraEngine::Renderer::RenderFrameScratchDiagnostics capturedScratchDiagnostics;
	if(scratchValidation && scratchValidation->onEvent) {
		auto inputs = std::make_shared<TerraEngine::Renderer::RenderFrameScratchCapturedInputs>();
		inputs->cameras.reserve(cameras.size());
		for(const auto& entry : cameras) {
			const auto object = objManager.getObject(entry.entity);
			if(!object) throw std::logic_error("Captured camera identity retired before protected validation");
			inputs->cameras.push_back({objManager.getUUID(*object), entry.camera});
		}
		inputs->lights.reserve(lights.size());
		for(const auto& entry : lights) inputs->lights.push_back({entry.lightId, entry.light});
		capturedScratchDiagnostics = lifetimeState->frameScratch->Snapshot();
		TerraEngine::Renderer::RenderFrameScratchValidationEvent event;
		event.checkpoint = TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint::Captured;
		event.episode = capturedScratchDiagnostics;
		event.capturedInputs = std::move(inputs);
		scratchValidation->onEvent(event);
	}
	frameTerrainMaterialBindings.reserve(renderables.size());
	for(const RenderableEntry& renderable : renderables)
	{
		if(renderable.terrainAssetId == TerraEngine::EMPTY_UUID || renderable.mesh == nullptr
			|| frameTerrainMaterialBindings.contains(renderable.mesh->materialId))
		{
			continue;
		}
		if(const auto material = TerraEngine::Renderer::Terrain::
			TerrainMaterialRuntimeRegistry::Instance().Resolve(renderable.mesh->materialId);
			material && material->IsComplete())
		{
			frameTerrainMaterialBindings.emplace(renderable.mesh->materialId, *material);
		}
	}
	// Command preparation retains object identity, so this compact value map is
	// enough to join visible commands back to the already copied terrain binding.
	// No worker callback receives an ECS entity or registry pointer.
	std::unordered_map<TerraEngine::UUID,
		std::pair<TerraEngine::UUID, TerraEngine::UUID>> frameTerrainObjects;
	frameTerrainObjects.reserve(frameTerrainMaterialBindings.size());
	for(const RenderableEntry& renderable : renderables)
	{
		if(renderable.terrainAssetId != TerraEngine::EMPTY_UUID && renderable.mesh != nullptr)
		{
			frameTerrainObjects.emplace(renderable.objectId,
				std::pair{renderable.terrainAssetId, renderable.mesh->materialId});
		}
	}

	// Scene/object/component retirement removes held ownership immediately. The
	// hard cap is a second guard for extremely large scenes and editor churn.
	for(auto cache = compatibleAnimationPoseCache.begin();
		cache != compatibleAnimationPoseCache.end();) {
		if(!aliveCompatibleAnimationObjects.contains(cache->first)) {
			cache = compatibleAnimationPoseCache.erase(cache);
		} else {
			++cache;
		}
	}
	while(compatibleAnimationPoseCache.size() > kCompatibleAnimationPoseCacheCapacity) {
		auto victim = std::min_element(compatibleAnimationPoseCache.begin(),
			compatibleAnimationPoseCache.end(), [](const auto& lhs, const auto& rhs) {
				if(lhs.second.lastTouchedFrameId != rhs.second.lastTouchedFrameId) {
					return lhs.second.lastTouchedFrameId < rhs.second.lastTouchedFrameId;
				}
				return TerraEngine::ToHexString(lhs.first) < TerraEngine::ToHexString(rhs.first);
			});
		if(victim == compatibleAnimationPoseCache.end()) {
			break;
		}
		compatibleAnimationPoseCache.erase(victim);
	}

	// Throttle warning spam so missing cameras do not flood the console every frame.
	static bool loggedMissingCamera = false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cameras.empty() && !hasSceneViewCamera)
	{
		if (!loggedMissingCamera)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "RenderSystem skipped frame because no CameraComponent was found in the scene.");
			loggedMissingCamera = true;
		}
	}
	else
	{
		loggedMissingCamera = false;
	}

	const std::unordered_map<entt::entity, TerraEngine::Scene::WorldPose>* worldTransforms = nullptr;
	const TerraEngine::Scene::SceneTransformPropagationResult* transformPropagation = nullptr;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Resolve World Transforms", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		worldTransforms = &worldTransformCache.Refresh(objManager, latestTransformCacheDiagnostics);
		transformPropagation = &worldTransformCache.LatestPropagation();
		if (!transformPropagation->valid)
		{
			TERRAENGINE_LOG(LogLevel::Error, "RenderSystem received an invalid scene transform propagation result.");
		}
	}

	auto makeModelMatrix = [](const TerraEngine::Scene::WorldPose& pose,
		const TerraEngine::Scene::WorldPosition& referenceOrigin) {
		glm::mat4 model = glm::mat4(1.0f);
		const TerraEngine::Scene::WorldLocalizationResult localized =
			TerraEngine::Scene::LocalizeWorldPosition(
				pose.position,
				referenceOrigin,
				TerraEngine::Scene::kTransformSpaceMaximumRelativeRadiusMetres);
		if (!localized.Succeeded())
		{
			return model;
		}
		model = glm::translate(model, glm::vec3(localized.offsetMetres));
		model = model * glm::mat4_cast(pose.rotation);
		model = glm::scale(model, pose.scale);
		return model;
	};
	const TerraEngine::Scene::WorldPose fallbackWorldTransform{};
	auto getWorldTransform = [&](entt::entity entity) -> const TerraEngine::Scene::WorldPose& {
		if (const auto transform = worldTransforms->find(entity); transform != worldTransforms->end())
		{
			return transform->second;
		}
		return fallbackWorldTransform;
	};

	activeRenderFrameId = nextRenderFrameId++;
	// Mesh payloads and streaming publications are immutable for this protected
	// render update. Build their expensive runtime mirrors once per unique mesh,
	// then let every instance and view reuse the same frame-local value.
	std::unordered_map<TerraEngine::UUID, VirtualGeometryAssetRuntime>
		virtualGeometryRuntimeByMesh;
	std::unordered_map<TerraEngine::UUID, CanonicalGeometryRouteFacts>
		geometryRouteFactsByMesh;
	// Scenes commonly contain thousands of instances but only a small set of
	// unique meshes. Keep these frame-local tables proportional to resource
	// diversity instead of allocating one bucket per instance every frame.
	const std::size_t expectedUniqueMeshCount = std::min<std::size_t>(
		64U, std::max<std::size_t>(1U, frameMeshResources.size()));
	virtualGeometryRuntimeByMesh.reserve(expectedUniqueMeshCount);
	geometryRouteFactsByMesh.reserve(expectedUniqueMeshCount);
	const auto getVirtualGeometryRuntime = [&virtualGeometryRuntimeByMesh](
		const TerraEngine::UUID& meshId, const Mesh& mesh)
		-> const VirtualGeometryAssetRuntime&
	{
		const auto existing = virtualGeometryRuntimeByMesh.find(meshId);
		if(existing != virtualGeometryRuntimeByMesh.end()) {
			return existing->second;
		}
		auto [inserted, wasInserted] = virtualGeometryRuntimeByMesh.emplace(
			meshId, BuildVirtualGeometryRuntimeFromMesh(meshId, mesh));
		(void)wasInserted;
		return inserted->second;
	};
	const auto getGeometryRouteFacts = [&geometryRouteFactsByMesh](
		const TerraEngine::UUID& meshId, const Mesh& mesh)
		-> const CanonicalGeometryRouteFacts&
	{
		const auto existing = geometryRouteFactsByMesh.find(meshId);
		if(existing != geometryRouteFactsByMesh.end()) {
			return existing->second;
		}
		auto [inserted, wasInserted] = geometryRouteFactsByMesh.emplace(
			meshId, BuildCanonicalGeometryRouteFacts(mesh));
		(void)wasInserted;
		return inserted->second;
	};
	std::shared_ptr<TerraEngine::Renderer::CpuAnimationDeformationPlan>
		cpuDeformationPlan;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build CPU Animation Deformation Plan",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	if(hasCpuDeformationCandidates)
	{
		cpuDeformationPlan =
			std::make_shared<TerraEngine::Renderer::CpuAnimationDeformationPlan>();
		cpuDeformationPlan->frameId = activeRenderFrameId;
		cpuDeformationPlan->rendererGeneration = protectedRendererGeneration;
	}
	struct CpuDeformationReuseKey
	{
		std::uint64_t visualPoseContentIdLow = 0U;
		std::uint64_t visualPoseContentIdHigh = 0U;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint64_t deformablePayloadGeneration = 0U;
		std::uint64_t skeletonCompatibilityFingerprint = 0U;

		bool operator==(const CpuDeformationReuseKey&) const = default;
	};
	struct CpuDeformationReuseKeyHash
	{
		std::size_t operator()(const CpuDeformationReuseKey& key) const noexcept
		{
			std::uint64_t hash = kRenderHashOffset;
			HashCombine(hash, key.visualPoseContentIdLow);
			HashCombine(hash, key.visualPoseContentIdHigh);
			HashCombine(hash, static_cast<std::uint64_t>(
				std::hash<TerraEngine::UUID>{}(key.meshId)));
			HashCombine(hash, key.deformablePayloadGeneration);
			HashCombine(hash, key.skeletonCompatibilityFingerprint);
			return static_cast<std::size_t>(hash);
		}
	};
	// This cache lasts only for the immutable frame. Synchronized visual clones
	// keep instance-specific pose publications but avoid repeating palette matrix
	// construction for the same model-space pose and exact mesh generation.
	std::unordered_map<CpuDeformationReuseKey,
		std::shared_ptr<const TerraEngine::Renderer::CpuAnimationDeformationData>,
		CpuDeformationReuseKeyHash> cpuDeformationByVisualPose;
	if(hasCpuDeformationCandidates)
	{
		cpuDeformationByVisualPose.reserve(renderables.size());
	}
	const std::span<RenderableEntry> cpuDeformationCandidates =
		hasCpuDeformationCandidates
			? std::span<RenderableEntry>(renderables.data(), renderables.size())
			: std::span<RenderableEntry>{};
	for(RenderableEntry& entry : cpuDeformationCandidates) {
		TerraEngine::Renderer::AnimationRenderBinding& binding =
			entry.animationBinding;
		// Hybrid/GPU bindings already own a compute-produced palette. Only the
		// CPU deterministic/fallback path needs the host-upload bridge.
		if(binding.gpu.IsComplete() || !binding.HasCpuPose()
			|| binding.authorityRow == nullptr || protectedRendererGeneration == 0U) {
			continue;
		}
		// Collection already resolved this exact resource under the same protected
		// boundary. Reuse it instead of reopening ResourceManager for each of the
		// few CPU-authoritative members of an otherwise GPU-visual crowd.
		const Mesh* mesh = entry.runtimeMesh;
		if(mesh == nullptr
			|| mesh->deformablePayloadGeneration
				!= binding.deformablePayloadGeneration
			|| ResolveDeformableSkeletonFingerprint(
					*mesh, binding.authorityRow)
				!= binding.skeletonCompatibilityFingerprint) {
			continue;
		}
		std::vector<std::uint64_t> morphTargetIds;
		try {
			morphTargetIds.reserve(mesh->deformableMorphs.size());
			for(const TemfDeformableMorphRecord& morph : mesh->deformableMorphs) {
				morphTargetIds.push_back(morph.stableTargetId);
			}
		} catch(const std::bad_alloc&) {
			continue;
		}
		std::shared_ptr<const TerraEngine::Renderer::CpuAnimationDeformationData>
			deformation;
		const CpuDeformationReuseKey reuseKey{
			binding.authorityRow->visualPoseContentIdLow,
			binding.authorityRow->visualPoseContentIdHigh,
			binding.meshId,
			binding.deformablePayloadGeneration,
			binding.skeletonCompatibilityFingerprint};
		if(reuseKey.visualPoseContentIdLow != 0U
			&& reuseKey.visualPoseContentIdHigh != 0U) {
			const auto reused = cpuDeformationByVisualPose.find(reuseKey);
			if(reused != cpuDeformationByVisualPose.end()) {
				deformation = reused->second;
			}
		}
		if(deformation == nullptr) {
			deformation = BuildCpuAnimationDeformation(
				*binding.authorityRow, mesh->deformablePaletteJoints, morphTargetIds);
			if(deformation != nullptr && deformation->IsComplete()
				&& reuseKey.visualPoseContentIdLow != 0U
				&& reuseKey.visualPoseContentIdHigh != 0U) {
				cpuDeformationByVisualPose.emplace(reuseKey, deformation);
			}
		}
		if(deformation == nullptr || !deformation->IsComplete()
			|| cpuDeformationPlan->records.size()
				>= std::numeric_limits<std::uint32_t>::max()) {
			continue;
		}
		binding.cpuDeformation = deformation;
		binding.deformationContentIdLow = deformation->contentIdLow;
		binding.deformationContentIdHigh = deformation->contentIdHigh;
		binding.cpuDeformationRecordIndex = static_cast<std::uint32_t>(
			cpuDeformationPlan->records.size());
		const std::size_t renderableIndex = static_cast<std::size_t>(
			std::addressof(entry) - renderables.data());
		if(renderableIndex < renderables.size()
			&& renderableIndex <= std::numeric_limits<std::uint32_t>::max()) {
			cpuDeformationRenderableIndices.push_back(
				static_cast<std::uint32_t>(renderableIndex));
		}
		cpuDeformationPlan->records.push_back({
			BuildStableAnimationInstanceId(binding, entry.objectId),
			binding.meshId,
			binding.posePublicationGeneration,
			binding.deformablePayloadGeneration,
			protectedRendererGeneration,
			binding.skeletonCompatibilityFingerprint,
			deformation});
	}
	if(cpuDeformationPlan != nullptr && cpuDeformationPlan->records.empty()) {
		cpuDeformationPlan.reset();
	}
	}
	const TerraEngine::Renderer::BackendCapabilityProfile backendCapabilities = rhi
		? rhi->GetBackendCapabilityProfile()
		: TerraEngine::Renderer::BackendCapabilityProfile{};
	// Route admission uses immutable device/bootstrap capability, while the
	// stronger supportsVirtualGeometry* stage flags remain fence-delayed proof
	// that work actually completed. Using completion evidence for admission
	// would deadlock a fresh editor: no route could run the first measured frame.
	const bool hardwareVirtualGeometryRouteAvailable =
		rhi != nullptr && rhi->SupportsVirtualGeometryMeshShaderRoute();
	const bool computeVirtualGeometryRouteAvailable =
		backendCapabilities.supportsVirtualGeometryGpuPacketBuffers;
	const bool deviceAuthoredVirtualGeometryAvailable =
		backendCapabilities.supportsVirtualGeometryGpuCompaction
		&& backendCapabilities.supportsVirtualGeometryGeneratedSubmissions;
	// Snapshot the mutually exclusive validation controls once before worker-safe
	// preparation. Packet values, rather than mutable editor state, cross the
	// protected Vulkan recording boundary later in this frame.
	std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute>
		validationRouteOverride;
	bool hybridValidationSceneRequested = false;
	{
		std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
		validationRouteOverride = virtualGeometryValidationRouteOverride;
		hybridValidationSceneRequested = virtualGeometryHybridValidationSceneRequested;
	}
	bool hasPendingDedicatedCaptureRender = false;
	TerraEngine::Renderer::RenderCaptureViewIdentity pendingDedicatedCaptureIdentity =
		TerraEngine::Renderer::ResolveDedicatedCaptureViewIdentity(
			RenderViewType::Game);
	TerraEngine::Scene::WorldPosition pendingDedicatedCaptureCanonicalOrigin{};
	glm::uvec2 pendingDedicatedCaptureTargetSize = captureLimits.defaultWidth > 0u && captureLimits.defaultHeight > 0u
		? glm::uvec2(captureLimits.defaultWidth, captureLimits.defaultHeight)
		: glm::uvec2(1280u, 720u);
	if (backendCapabilities.supportsImageReadback)
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for (const TerraEngine::Renderer::RenderCaptureRequest& captureRequest : pendingCaptureRequests)
		{
			if ((captureRequest.sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView
					|| captureRequest.dedicatedRenderTarget)
				&& captureRequest.format == "image/png")
			{
				hasPendingDedicatedCaptureRender = true;
				pendingDedicatedCaptureIdentity =
					TerraEngine::Renderer::ResolveDedicatedCaptureViewIdentity(
						captureRequest.viewType);
				pendingDedicatedCaptureTargetSize = captureRequest.targetSize;
				pendingDedicatedCaptureCanonicalOrigin =
					captureRequest.cameraCanonicalPosition;
				break;
			}
		}
	}
	// Capture monotonic producer values once. These scalars are also the worker
	// reuse proof below, so no worker needs to query mutable scene ownership.
	const std::uint64_t capturedSceneGeneration = objManager.GetSceneGeneration();
	const std::uint64_t capturedSpatialRevision = objManager.GetSpatialRevision();
	const std::uint64_t capturedComponentMutationRevision =
		objManager.GetComponentMutationRevision();
	const std::uint64_t capturedMeshComponentRevision =
		objManager.GetComponentValueMutationRevision(
			MeshRenderComponent::kComponentName);
	const std::uint64_t capturedTerrainComponentRevision =
		objManager.GetComponentValueMutationRevision(
			TerrainComponent::kComponentName);
	const std::uint64_t capturedTerrainProxyGeneration =
		TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
			.PublicationGeneration();
	// Existing cache records call this the mesh-component revision. Fold every
	// source of a resolved render binding into one token so TerrainComponent and
	// proxy changes invalidate the same topology, spatial, and draw caches.
	std::uint64_t capturedRenderBindingRevision = kRenderHashOffset;
	HashCombine(capturedRenderBindingRevision, capturedMeshComponentRevision);
	HashCombine(capturedRenderBindingRevision, capturedTerrainComponentRevision);
	HashCombine(capturedRenderBindingRevision, capturedTerrainProxyGeneration);
	const std::uint64_t capturedHierarchyRevision = objManager.GetHierarchyRevision();
	const std::uint64_t capturedActiveMembershipRevision =
		objManager.GetRuntimeActiveMembershipRevision();
	const std::uint64_t capturedAnimatorComponentRevision =
		objManager.GetComponentValueMutationRevision(
			AnimatorComponent::kComponentName);
	// Close the same protected loader cohort after retaining shader, mesh and
	// material owners. A concurrent publication disables snapshot reuse rather
	// than mixing a newer shader owner with an older reusable capture signature.
	const TerraEngine::UUID capturedDefaultShaderCollectionId =
		resourceManager.GetDefaultShaderCollectionID();
	const TerraEngine::UUID capturedDefaultTaskShaderCollectionId =
		resourceManager.GetDefaultTaskShaderCollectionID();
	const auto capturedShadowMeshCollection = resourceManager.AcquireShaderCollectionSnapshot(capturedDefaultShaderCollectionId);
	const auto capturedShadowTaskCollection = capturedDefaultTaskShaderCollectionId == capturedDefaultShaderCollectionId
		? capturedShadowMeshCollection : resourceManager.AcquireShaderCollectionSnapshot(capturedDefaultTaskShaderCollectionId);
	const std::uint64_t capturedRendererResourceRevision =
		resourceManager.GetRendererResourcePublicationGeneration();
	const bool rendererResourceOwnerCohortStable = protectedRendererResourceOwnerRevision == capturedRendererResourceRevision;
	const std::uint64_t capturedAnimationBindingResourceRevision =
		resourceManager.GetAnimationBindingResourceRevision();
	const std::uint64_t capturedVirtualGeometryResidencyGeneration =
		TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance()
			.ResidencySnapshot().publicationGeneration;
	const RenderSettings capturedRenderSettings =
		SanitizeRenderSettings(currentSettings);
	const auto capturedEfficiencyOptions = GetRendererEfficiencyOptionsSnapshot();
	std::uint64_t capturedRenderableSourceSignature = kRenderHashOffset;
	HashCombine(capturedRenderableSourceSignature, capturedSceneGeneration);
	HashCombine(capturedRenderableSourceSignature, capturedSpatialRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedComponentMutationRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedMeshComponentRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedTerrainComponentRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedTerrainProxyGeneration);
	HashCombine(capturedRenderableSourceSignature, capturedHierarchyRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedRendererResourceRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedVirtualGeometryResidencyGeneration);
	HashCombine(capturedRenderableSourceSignature,
		BuildRenderSettingsSignature(capturedRenderSettings, currentPreset,
			taskShaderCullingEnabled));
	HashCombine(capturedRenderableSourceSignature,
		hardwareVirtualGeometryRouteAvailable ? 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		computeVirtualGeometryRouteAvailable ? 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		deviceAuthoredVirtualGeometryAvailable ? 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		validationRouteOverride.has_value()
			? static_cast<std::uint64_t>(*validationRouteOverride) + 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		hybridValidationSceneRequested ? 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		hasDenseAnimationTopologyCandidate && immutableGpuAnimationPlan != nullptr
			? immutableGpuAnimationPlan->topologyInputSignature
			: animationPresentationFrame != nullptr
				? animationPresentationFrame->presentationGeneration : 0u);
	HashCombine(capturedRenderableSourceSignature,
		static_cast<std::uint64_t>(renderables.size()));
	if(capturedRenderableSourceSignature == 0u)
		capturedRenderableSourceSignature = 1u;

	// GPU-visual owners advance through the immutable animation publication, not
	// through geometry records. Static owners have no binding. Any CPU/hybrid or
	// incomplete binding disables reuse so its exact pose/fallback owner is rebuilt.
	bool renderableBindingsReusable = true;
	for(const RenderableEntry& entry : renderables)
	{
		const TerraEngine::Renderer::AnimationRenderBinding& binding =
			entry.animationBinding;
		if(binding.meshId == TerraEngine::EMPTY_UUID)
			continue;
		if(!hasDenseAnimationTopologyCandidate || !binding.gpu.IsComplete()
			|| binding.gpu.hybridCpuAuthority || binding.HasCpuPose())
		{
			renderableBindingsReusable = false;
			break;
		}
	}
	if(transformPropagation != nullptr && transformPropagation->valid)
	{
		for(const TerraEngine::Scene::SceneWorldTransformRecord& changedTransform :
			transformPropagation->records)
		{
			const auto changedRenderable = std::lower_bound(
				renderables.begin(), renderables.end(), changedTransform.objectId,
				[](const RenderableEntry& entry, const TerraEngine::UUID& objectId)
				{
					return entry.objectId < objectId;
				});
			if(changedRenderable != renderables.end()
				&& changedRenderable->objectId == changedTransform.objectId)
			{
				changedRenderable->transformGeneration = std::max<std::uint64_t>(
					1U, changedTransform.sourceRevision);
			}
		}
	}
	const bool capturedRenderableConsumer = !cameras.empty()
		|| hasSceneViewCamera || hasQueuedDedicatedCapture;
	retainedRenderableEntriesReusable = renderableBindingsReusable
		&& renderableResourcePublicationsComplete
		&& capturedRenderableConsumer && rendererResourceOwnerCohortStable;
	retainedRenderableCaptureSourceSignature =
		retainedRenderableEntriesReusable
		? renderableCaptureSourceSignature : 0U;
	const bool detailedRenderableEvidenceRequested =
		TerraEngine::Renderer::RenderDiagnosticsRequested();
	TerraEngine::Renderer::RenderFrameSnapshot renderFrameSnapshot =
		std::move(recycledRenderFrameSnapshot);
	// View requests are tiny compared with renderable records. Retain the prior
	// qualified origins so motion matrices can be rebuilt without treating a
	// canonical pose as a global float.
	const std::vector<TerraEngine::Renderer::RenderFrameViewRequest>
		previousViewRequests = renderFrameSnapshot.viewRequests;
	const bool reuseRenderableRecords = renderableBindingsReusable
		&& renderableResourcePublicationsComplete
		&& rendererResourceOwnerCohortStable
		&& !detailedRenderableEvidenceRequested
		&& renderFrameSnapshot.reusableRenderableRecords
		&& renderFrameSnapshot.renderableSourceSignature
			== capturedRenderableSourceSignature
		&& renderFrameSnapshot.renderables.size() == renderables.size();
	// Reuse top-level vector capacity from the prior published frame. Clearing the
	// non-visible owner here avoids allocating and later destroying another large
	// crowd snapshot while the concurrently readable latest snapshot stays intact.
	renderFrameSnapshot.viewRequests.clear();
	if(!reuseRenderableRecords)
	{
		renderFrameSnapshot.renderables.clear();
		renderFrameSnapshot.materials.clear();
		renderFrameSnapshot.meshes.clear();
		// Direct-batch contents are rebuilt only with persistent renderable records.
		// Their instance arrays stay intact on a warm reusable frame.
		for(TerraEngine::Renderer::DirectMeshletBatch& batch :
			renderFrameSnapshot.directMeshletBatches)
		{
			batch.instanceIdentities.clear();
		}
	}
	renderFrameSnapshot.conventionalVisibilityIdentityTable.clear();
	renderFrameSnapshot.lights.clear();
	renderFrameSnapshot.textures.clear();
	renderFrameSnapshot.virtualGeometryViewSelections.clear();
	renderFrameSnapshot.virtualGeometryVisibilitySnapshots.clear();
	renderFrameSnapshot.geometryRouteDecisions.clear();
	renderFrameSnapshot.animatedGeometryPageDemands.clear();
	renderFrameSnapshot.animatedGeometryPageDemandCount = 0u;
	renderFrameSnapshot.resourceRevision = 0u;
	renderFrameSnapshot.sceneRevision = 0u;
	renderFrameSnapshot.valid = true;
	renderFrameSnapshot.containsLiveMutableState = false;
	renderFrameSnapshot.frameId = activeRenderFrameId;
	renderFrameSnapshot.renderableSourceSignature =
		capturedRenderableSourceSignature;
	renderFrameSnapshot.reusableRenderableRecords = renderableBindingsReusable
		&& renderableResourcePublicationsComplete
		&& !detailedRenderableEvidenceRequested && rendererResourceOwnerCohortStable;
	renderFrameSnapshot.renderSettings = capturedRenderSettings;
	renderFrameSnapshot.efficiencyOptions = capturedEfficiencyOptions;
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		renderFrameSnapshot.efficiencyFixtureIntent = capturedRendererEfficiencyFixtureIntent;
	}
	// Renderer asset publication is monotonic and advances only after a complete
	// mesh, material, or texture cache replacement. Backend preparation caches use
	// it to reject plans built from older immutable payloads.
	renderFrameSnapshot.resourceRevision = capturedRendererResourceRevision;
	renderFrameSnapshot.sceneRevision = capturedSpatialRevision;
	std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureTargetProof> fixtureTargets;
	std::vector<TerraEngine::Renderer::RendererEfficiencyResolvedFixtureCamera> sealedFixtureCameras;
	if (!renderFrameSnapshot.efficiencyFixtureIntent.correlation.operationId.empty())
	{
		// Validation alone requests owned cooked range copies. Ordinary frames
		// avoid this allocation; no host can retain live loader pointers.
		fixtureTargets.reserve(renderables.size());
		for (const auto& entry : renderables)
		{
			const auto found = frameMeshResources.find(entry.mesh->meshId);
			const Mesh* mesh = found != frameMeshResources.end() ? found->second : nullptr;
			if (mesh == nullptr) continue;
			TerraEngine::Renderer::RendererEfficiencyFixtureTargetProof proof;
			proof.objectId = entry.objectId; proof.meshId = entry.mesh->meshId;
			if (const auto object = objManager.getObject(entry.entity); object.has_value()) {
				proof.lifetimeGeneration = objManager.getObjectLifetimeGeneration(*object);
				proof.isStatic = objManager.isObjectStatic(*object);
				proof.metadataGeneration = objManager.getObjectMetadataGeneration(*object);
				proof.spatialRevision = objManager.GetObjectSpatialRevision(*object);
			}
			proof.resourceGeneration = rendererResourceOwnerCohortStable ? renderFrameSnapshot.resourceRevision : 0;
			proof.meshletCapacity = mesh->runtimeLayout.meshletCount;
			proof.ownerGeometrySupported = !mesh->hasVirtualGeometry &&
				getGeometryRouteFacts(entry.mesh->meshId, *mesh).directUsable;
			for (const auto& detail : mesh->virtualGeometryDetailLevels)
				proof.detailLevels.push_back({detail.lodIndex, detail.meshletOffset, detail.meshletCount});
			fixtureTargets.push_back(std::move(proof));
		}
	}
	{
		auto& intent = renderFrameSnapshot.efficiencyFixtureIntent;
		intent.controlsApplied = !intent.correlation.operationId.empty(); intent.controlReason.clear();
		if(!TerraEngine::Renderer::ResolveRendererEfficiencyFixtureCameras(intent, sealedFixtureCameras, intent.controlReason)) intent.controlsApplied = false;
		if(intent.controlsApplied && !ValidateDepthFixtureIntent(intent,intent.controlReason)) intent.controlsApplied = false;
		if(intent.controlsApplied && !TerraEngine::Renderer::ValidateRendererEfficiencyFixtureCasterSelections(intent,fixtureTargets,intent.controlReason)) intent.controlsApplied = false;
		if (intent.controlsApplied && intent.lightingControls)
		{
			// Capture the complete active input, before camera queries or budgets.
			// Inactive scene lights cannot be introduced by a validation ordering.
			std::vector<TerraEngine::UUID> activeLightIds; activeLightIds.reserve(lights.size());
			for (const auto& light : lights) activeLightIds.push_back(light.lightId);
			if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureLightingControls(intent, activeLightIds, intent.controlReason)) intent.controlsApplied = false;
		}
		if (intent.shadowSelection)
		{
			const auto target = std::find_if(fixtureTargets.begin(), fixtureTargets.end(), [&](const auto& row) { return row.objectId == intent.shadowSelection->objectId; });
			TerraEngine::Renderer::RendererEfficiencyFixtureDetailRange range;
			if (target == fixtureTargets.end()) { intent.controlsApplied = false; intent.controlReason = "Requested fixture object is absent from current protected capture."; }
			else if (!TerraEngine::Renderer::ResolveRendererEfficiencyFixtureSelection(*intent.shadowSelection, *target, range, intent.controlReason)) intent.controlsApplied = false;
		}
		std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureViewOrderEntry> actualViews;
		if(!sealedFixtureCameras.empty() && intent.controlsApplied) {for(const auto& camera:sealedFixtureCameras) actualViews.push_back({camera.viewType,camera.outputId});}
		else {
			if (hasSceneViewCamera && (!editorModeEnabled || (sceneViewTargetSize.x && sceneViewTargetSize.y))) actualViews.push_back({RenderViewType::Scene, 1});
			if (!gameViewSuppressed && (!editorModeEnabled || (gameViewTargetSize.x && gameViewTargetSize.y)) && (!gameViewCameras.empty() || !cameras.empty())) actualViews.push_back({RenderViewType::Game, 2});
		}
		std::vector<std::size_t> viewIndices; std::string viewReason;
		if (intent.controlsApplied && intent.lightingControls && intent.lightingControls->snapshotSlot
			&& std::none_of(actualViews.begin(), actualViews.end(), [&](const auto& view) {
				return view.viewType == intent.lightingControls->snapshotViewType && view.outputId == intent.lightingControls->snapshotOutputId;
			}))
		{ intent.controlsApplied = false; intent.controlReason = "Lighting snapshot target is absent from the captured output membership."; }
		if (!TerraEngine::Renderer::ResolveRendererEfficiencyFixtureViewOrder(intent, actualViews, viewIndices, viewReason))
		{ intent.controlsApplied = false; intent.controlReason = viewReason; }
		if (sealedFixtureCameras.empty() && (!intent.viewOrder.empty() || !intent.viewOrigins.empty())
			&& (gameViewCameras.size() > 1 || (gameViewCameras.empty() && cameras.size() > 1)))
		{ intent.controlsApplied = false; intent.controlReason = "Controlled view identity is ambiguous across multiple Game cameras."; }
		if (sealedFixtureCameras.empty() && (intent.referenceOriginDelta || !intent.viewOrigins.empty()))
		{
			// Validate every affected view before changing any reference frame.
			const auto validCamera = [&](const TerraEngine::Scene::WorldPosition& camera, RenderViewType type, std::uint32_t output) {
				const auto delta = TerraEngine::Renderer::RendererEfficiencyFixtureOriginDelta(intent, type, output);
				if (!delta) return true;
				TerraEngine::Renderer::RenderReferenceFrame frame; frame.canonicalOrigin = camera;
				glm::mat4 view(1); glm::vec3 local(0);
				return TerraEngine::Renderer::RebaseRendererEfficiencyFixtureView(camera, *delta, frame, view, local);
			};
			bool valid = !hasSceneViewCamera || validCamera(sceneViewCamera.canonicalPosition, RenderViewType::Scene, 1);
			for (const auto& camera : gameViewCameras) valid = valid && validCamera(camera.canonicalPosition, RenderViewType::Game, 2);
			for (const auto& camera : cameras)
			{
				const auto pose = worldTransforms->find(camera.entity);
				valid = valid && pose != worldTransforms->end() && validCamera(pose->second.position, RenderViewType::Game, 2);
			}
			if (!valid) { intent.controlsApplied = false; intent.controlReason = "Reference-origin delta cannot be safely localized for both views."; }
		}
		// A user's pending dedicated capture retains its ordinary completion path.
		// Reject the fixture controls instead of acknowledging an unexpected view.
		if(!intent.correlation.operationId.empty() && hasPendingDedicatedCaptureRender)
		{ intent.controlsApplied = false; intent.controlReason = "Owned fixture controls rejected because a dedicated capture is pending."; }
	}
	// Sealed descriptors are coroutine-owned immutable copies. Window visibility,
	// orbit controls and panel dimensions cannot overwrite validation view inputs.
	const auto sealedCamera = [&](RenderViewType type, std::uint32_t output) -> const TerraEngine::Renderer::RendererEfficiencyResolvedFixtureCamera* {
		if(!renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied) return nullptr;
		const auto found = std::find_if(sealedFixtureCameras.begin(),sealedFixtureCameras.end(),[&](const auto& camera) {return camera.viewType==type && camera.outputId==output;});
		return found==sealedFixtureCameras.end() ? nullptr : &*found;
	};
	const auto* fixedSceneCamera = sealedCamera(RenderViewType::Scene,1);
	const auto* fixedGameCamera = sealedCamera(RenderViewType::Game,2);
	const bool sealedCameraRecipeActive = renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied && !sealedFixtureCameras.empty();
	const glm::uvec2 effectiveSceneTargetSize = fixedSceneCamera ? fixedSceneCamera->nativeExtent : sceneViewTargetSize;
	const glm::uvec2 effectiveGameTargetSize = fixedGameCamera ? fixedGameCamera->nativeExtent : gameViewTargetSize;
	// Capture the renderer lifetime once. Every animated record in this immutable
	// frame must agree with it; querying recovery state per renderable could mix a
	// pre-loss and post-loss generation in one snapshot.
	const std::uint64_t snapshotRendererGeneration = protectedRendererGeneration;
	const auto makeViewRequest = [&](RenderOutputKind outputKind,
		RenderViewType viewType,
		const glm::uvec2& targetSize,
		std::uint32_t outputId,
		const TerraEngine::Scene::WorldPosition& canonicalOrigin)
	{
		TerraEngine::Renderer::RenderFrameViewRequest request{};
		request.outputKind = outputKind;
		request.viewType = viewType;
		request.targetSize = targetSize;
		request.outputId = outputId;
		request.viewGeneration = activeRenderFrameId;
		request.referenceFrame.frameId = activeRenderFrameId;
		request.referenceFrame.viewId = static_cast<std::uint64_t>(outputId);
		request.referenceFrame.outputId = outputId;
		request.referenceFrame.canonicalOrigin = canonicalOrigin;
		const auto* fixedCamera = sealedCamera(viewType,outputId);
		if(fixedCamera) request.referenceFrame.canonicalOrigin = fixedCamera->referenceOrigin;
		const auto& fixtureIntent = renderFrameSnapshot.efficiencyFixtureIntent;
		const auto originDelta = TerraEngine::Renderer::RendererEfficiencyFixtureOriginDelta(fixtureIntent, viewType, outputId);
		if (!fixedCamera && fixtureIntent.controlsApplied && originDelta
			&& (outputKind == RenderOutputKind::SceneView || outputKind == RenderOutputKind::GameView || outputKind == RenderOutputKind::MainRuntimeView))
		{
			const auto changed = TerraEngine::Scene::ApplyWorldDelta(canonicalOrigin, *originDelta);
			if (changed.Succeeded()) request.referenceFrame.canonicalOrigin = changed.position;
		}
		request.referenceFrame.originGeneration = activeRenderFrameId;
		request.referenceFrame.spatialGeneration =
			std::max<std::uint64_t>(1U, capturedSpatialRevision);
		request.referenceFrame.residencyGeneration = std::max<std::uint64_t>(
			1U, capturedVirtualGeometryResidencyGeneration);
		return request;
	};
	renderFrameSnapshot.viewRequests.reserve(3u + gameViewCameras.size() + cameras.size());
	if (fixedSceneCamera || (!sealedCameraRecipeActive && hasSceneViewCamera && (!editorModeEnabled || (sceneViewTargetSize.x > 0u && sceneViewTargetSize.y > 0u))))
	{
		renderFrameSnapshot.viewRequests.push_back(makeViewRequest(
			RenderOutputKind::SceneView, RenderViewType::Scene,
			effectiveSceneTargetSize, 1U, fixedSceneCamera ? fixedSceneCamera->cameraPosition : sceneViewCamera.canonicalPosition));
	}
	if (fixedGameCamera || (!sealedCameraRecipeActive && !gameViewSuppressed && (!editorModeEnabled || (gameViewTargetSize.x > 0u && gameViewTargetSize.y > 0u))))
	{
		if (fixedGameCamera || !gameViewCameras.empty())
		{
			renderFrameSnapshot.viewRequests.push_back(makeViewRequest(
				RenderOutputKind::GameView, RenderViewType::Game,
				effectiveGameTargetSize, 2U, fixedGameCamera ? fixedGameCamera->cameraPosition : gameViewCameras.front().canonicalPosition));
		}
		else if (!cameras.empty())
		{
			TerraEngine::Scene::WorldPosition runtimeCameraPosition{};
			const auto runtimeCamera = worldTransforms->find(cameras.front().entity);
			if (runtimeCamera != worldTransforms->end())
			{
				runtimeCameraPosition = runtimeCamera->second.position;
			}
			renderFrameSnapshot.viewRequests.push_back(makeViewRequest(
				RenderOutputKind::MainRuntimeView, RenderViewType::Game,
				effectiveGameTargetSize, 2U, runtimeCameraPosition));
		}
	}
	if (hasPendingDedicatedCaptureRender)
	{
		renderFrameSnapshot.viewRequests.push_back(makeViewRequest(
			RenderOutputKind::OffscreenTexture,
			pendingDedicatedCaptureIdentity.viewType,
			pendingDedicatedCaptureTargetSize,
			pendingDedicatedCaptureIdentity.outputId,
			pendingDedicatedCaptureCanonicalOrigin));
	}
	if (renderFrameSnapshot.viewRequests.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Skip No Render Outputs", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		TerraEngine::Renderer::RenderDiagnosticsSummary noViewDiagnostics{};
		noViewDiagnostics.frameId = activeRenderFrameId;
		const TerraEngine::Renderer::RendererRecoverySnapshot recoverySnapshot = rhi
			? rhi->GetLatestRendererRecoverySnapshot()
			: latestRendererRecoverySnapshot;
		TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(noViewDiagnostics, recoverySnapshot);
		if (!TerraEngine::Renderer::CanSubmitNormalGpuWork(recoverySnapshot))
		{
			ReportFatalRendererRecoveryFailure(recoverySnapshot);
		}
		const TerraEngine::Renderer::RendererRecoverySnapshot finalRecoverySnapshot = rhi
			? rhi->GetLatestRendererRecoverySnapshot()
			: latestRendererRecoverySnapshot;
		TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(noViewDiagnostics, finalRecoverySnapshot);
		if (TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			noViewDiagnostics.delayedWork.push_back("Render frame skipped because no Scene, Game, or runtime output target was requested.");
		}
		if(rhi)
		{
			TerraEngine::Renderer::ApplyVirtualGeometryCapabilityDiagnostics(
				noViewDiagnostics,
				CollectVirtualGeometryCapabilityDiagnostics(
					*rhi,
					backendCapabilities,
					{
						activeRenderFrameId,
						0u,
						snapshotRendererGeneration
					}));
		}
		TerraEngine::Renderer::PublishRenderDiagnostics(noViewDiagnostics);
		// A valid empty publication lets visual-only animation sleep while every
		// output is closed. Scene generation still guards against stale project data.
		TerraEngine::Renderer::AnimatedObjectVisibilitySnapshot emptyVisibility;
		emptyVisibility.frameId = activeRenderFrameId;
		emptyVisibility.sceneGeneration = protectedSceneGeneration;
		emptyVisibility.hasActiveView = false;
		emptyVisibility.valid = true;
		emptyVisibility.canonicalUnique = true;
		TerraEngine::Renderer::PublishAnimatedObjectVisibilitySnapshot(
			std::move(emptyVisibility));
		{
			std::lock_guard<std::mutex> lock(statsMutex);
			std::swap(latestRenderFrameSnapshot, renderFrameSnapshot);
			recycledRenderFrameSnapshot = std::move(renderFrameSnapshot);
			latestRendererRecoverySnapshot = finalRecoverySnapshot;
			latestRenderGraphDiagnostics = noViewDiagnostics;
			latestGameVirtualGeometryVisibilityRecords.clear();
			latestSceneVirtualGeometryVisibilityRecords.clear();
			latestGameCaptureVisibilityRecords.clear();
			latestSceneCaptureVisibilityRecords.clear();
			latestGameCaptureIdentitySnapshot = {};
			latestSceneCaptureIdentitySnapshot = {};
			pendingGameIdentitySnapshot = {};
			pendingSceneIdentitySnapshot = {};
			pendingGameIdentityVisibilityRecords.clear();
			pendingSceneIdentityVisibilityRecords.clear();
			pendingGameAuthoritativeIdentityPixel.reset();
			pendingSceneAuthoritativeIdentityPixel.reset();
			virtualGeometryDebugModes.clear();
		}
		// Closing every output retires all view-local history immediately. A later
		// view may reuse the same numeric output ID, but it must begin with a fresh
		// Hi-Z policy and geometric-transition history.
		virtualGeometryViewPolicies.clear();
		virtualGeometryTransitionCache.clear();
		// Even when no view submits GPU work, streaming visibility needs still
		// have to age out. Without this cleanup pass, assets that were visible
		// on a previous Scene/Game View frame stay marked as active-view after
		// the user hides those views or disables every camera.
		ProcessQueuedCaptureRequests();
		DrainCaptureReadbacks();
		ResourceManager::Instance().RunResidencyCleanup(activeRenderFrameId);
		ClearOverrideCameras();
		scratchLease.MarkCompleted();
		co_return;
	}
	// Capture every live generation and completed backend counter before leaving
	// the protected scheduler boundary. The continuation below may run on a worker,
	// so it must consume only these values, the immutable scene-change packet, and
	// the frame-owned component/resource snapshots prepared above.
	const std::vector<TerraEngine::Renderer::VirtualGeometryAuthoritativeCounterReadback>
		completedVirtualGeometryCounters = rhi
		? rhi->PollVirtualGeometryReplayCounters()
		: std::vector<TerraEngine::Renderer::VirtualGeometryAuthoritativeCounterReadback>{};
	const TerraEngine::Renderer::BackendFrameBreakdown capturedBackendBreakdown = rhi
		? rhi->GetLatestBackendFrameBreakdown()
		: TerraEngine::Renderer::BackendFrameBreakdown{};
	const TerraEngine::Renderer::RendererRecoverySnapshot capturedRecoverySnapshot = rhi
		? rhi->GetLatestRendererRecoverySnapshot()
		: latestRendererRecoverySnapshot;
	const auto capturedUiBatch = UISystem::Instance().GetCombinedFrameBatch();
	const TerraEngine::Resource::VirtualGeometryStreamingPlan
		capturedVirtualGeometryStreamingPlan =
			TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance()
				.LastPreparedPlan();
	// Sky hierarchy selection is scene-owned work. Resolve and copy the selected
	// component before the worker boundary so sky preparation cannot traverse live
	// ECS storage or resolve an asset while another system is publishing changes.
	std::optional<SkyComponent> capturedSkyComponent;
	TerraEngine::UUID capturedSkyObjectId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID capturedSkyTextureId = TerraEngine::EMPTY_UUID;
	{
		const std::uint64_t skyHierarchyRevision = capturedHierarchyRevision;
		std::uint64_t skyCandidateSignature = 0U;
		const bool observeSkyCandidates = skyStateDirty
			|| !hasCachedSkyObjectResolution
			|| cachedSkyHierarchyRevision != skyHierarchyRevision;
		if(observeSkyCandidates)
		{
			const auto skyCandidates = objManager.getRegistry().view<SkyComponent>();
			for(const entt::entity entity : skyCandidates)
			{
				const std::optional<Object> object = objManager.getObject(entity);
				const SkyComponent* component = object
					? objManager.GetComponent<SkyComponent>(*object) : nullptr;
				if(component == nullptr) continue;
				const bool objectActive = objManager.isObjectActiveInHierarchy(*object);
				const std::uint64_t entityValue = static_cast<std::uint32_t>(entity);
				const std::uint64_t candidateBits = entityValue
					^ (component->active ? 0x9e3779b97f4a7c15ULL : 0ULL)
					^ (objectActive ? 0xc2b2ae3d27d4eb4fULL : 0ULL);
				skyCandidateSignature ^= candidateBits + 0x9e3779b97f4a7c15ULL
					+ (skyCandidateSignature << 6U) + (skyCandidateSignature >> 2U);
			}
		}
		const bool skyCandidatesChanged = skyStateDirty
			&& skyCandidateSignature != cachedSkyCandidateSignature;
		if(!hasCachedSkyObjectResolution
			|| cachedSkyHierarchyRevision != skyHierarchyRevision
			|| skyCandidatesChanged)
		{
			cachedSkyObject = SceneManager::Instance().GetActiveSkyObject();
			hasCachedSkyObjectResolution = true;
			cachedSkyHierarchyRevision = skyHierarchyRevision;
			cachedSkyCandidateSignature = skyCandidateSignature;
		}
		const std::optional<Object> activeSkyObject = cachedSkyObject.has_value()
			? objManager.getObject(*cachedSkyObject) : std::nullopt;
		if(activeSkyObject.has_value())
		{
			if(const SkyComponent* component =
				objManager.GetComponent<SkyComponent>(*activeSkyObject))
			{
				capturedSkyComponent = *component;
				capturedSkyObjectId = objManager.getUUID(*activeSkyObject);
				if(!component->sourceAsset.empty())
				{
					capturedSkyTextureId = resourceManager.ResolveAssetReference(
						component->sourceAsset, AssetType::Texture);
				}
			}
		}
	}
	// Only protected capture can inspect live identity and loader ownership.
	// Copy lifetimes only when rebuilding rows; compatible warm snapshots retain
	// their sealed values. Shader owners stay alive across worker suspension even
	// when an asynchronous reload replaces the ResourceManager cache entry.
	std::vector<std::uint64_t> capturedShadowObjectLifetimes;
	if(!reuseRenderableRecords) {
		capturedShadowObjectLifetimes.resize(renderables.size());
		for(std::size_t index = 0U; index < renderables.size(); ++index) {
			if(const auto object = objManager.getObject(renderables[index].entity); object.has_value())
				capturedShadowObjectLifetimes[index] = objManager.getObjectLifetimeGeneration(*object);
		}
	}
	// Boundary 1 ends immediately after immutable capture. Route scoring, snapshot
	// assembly, topology transactions, and deterministic lowering are worker work;
	// keeping them out of the protected phase prevents scene mutation and editor UI
	// from waiting on a complete crowd traversal.
	co_await std::suspend_always{};
	struct GeometryRouteStatsAggregate
	{
		std::uint32_t directCount = 0U;
		std::uint32_t virtualCount = 0U;
		std::uint32_t sharedInstances = 0U;
		std::uint64_t policyIdentity = 0U;
		std::uint64_t calibrationIdentity = 0U;
		std::uint32_t hysteresisFrames = 0U;
		std::string fallbackReason;
		TerraEngine::Renderer::GeometryRouteSelectionSource source =
			TerraEngine::Renderer::GeometryRouteSelectionSource::Automatic;
	};
	GeometryRouteStatsAggregate geometryRouteStats;
	std::uint64_t geometryRouteSelectionSignature = 1469598103934665603ULL;
	if(!reuseRenderableRecords)
	{
	TERRAENGINE_PROFILE_SCOPE("RenderSystem: Assemble Render Snapshot",
		TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	std::unordered_map<TerraEngine::UUID,
		std::array<std::optional<TerraEngine::Renderer::VirtualGeometryDrawRange>, 2U>>
		virtualDrawRangeByMesh;
	virtualDrawRangeByMesh.reserve(expectedUniqueMeshCount);
	// Route scoring depends on immutable mesh/capability/calibration facts, not on
	// an instance transform. Evaluate hysteresis once per unique mesh per frame,
	// then stamp the copied decision with each object's diagnostic identity.
	struct CachedGeometryRouteDecision
	{
		// Route scoring and mesh hashing depend only on the immutable mesh facts.
		// Keeping both values together lets every repeated instance copy the result
		// without rebuilding costs or rehashing the shared mesh UUID.
		TerraEngine::Renderer::GeometryRouteDecision decision{};
		std::uint64_t meshIdentity = 0U;
	};
	std::unordered_map<TerraEngine::UUID, CachedGeometryRouteDecision>
		geometryRouteDecisionByMesh;
	geometryRouteDecisionByMesh.reserve(expectedUniqueMeshCount);
	renderFrameSnapshot.renderables.reserve(renderables.size());
	// Meshes and materials are resource tables, not draw tables. Track their exact
	// identities once per frame so instancing does not create, sort, hash, and copy
	// thousands of duplicate backend resource records.
	std::unordered_set<TerraEngine::UUID> snapshotMeshIds;
	std::unordered_set<TerraEngine::UUID> snapshotMaterialIds;
	snapshotMeshIds.reserve(expectedUniqueMeshCount);
	snapshotMaterialIds.reserve(expectedUniqueMeshCount);
	renderFrameSnapshot.meshes.reserve(expectedUniqueMeshCount);
	renderFrameSnapshot.materials.reserve(expectedUniqueMeshCount);
	const bool retainDetailedRouteCandidates =
		TerraEngine::Renderer::RenderDiagnosticsRequested();
	if(retainDetailedRouteCandidates)
	{
		// Full route decisions contain candidate vectors and diagnostic strings.
		// Keep them only while an explicit diagnostics consumer is active; ordinary
		// rendering stores the compact per-instance selection record below.
		renderFrameSnapshot.geometryRouteDecisions.reserve(renderables.size());
	}
	const auto copyRouteDecisionForFrame = [retainDetailedRouteCandidates](
		const TerraEngine::Renderer::GeometryRouteDecision& source) {
		TerraEngine::Renderer::GeometryRouteDecision destination;
		destination.decisionIdentity = source.decisionIdentity;
		destination.frameId = source.frameId;
		destination.viewFamilyId = source.viewFamilyId;
		destination.objectIdentity = source.objectIdentity;
		destination.meshIdentity = source.meshIdentity;
		destination.assetGeneration = source.assetGeneration;
		destination.selected = source.selected;
		if(retainDetailedRouteCandidates) {
			// Candidate rejection strings are valuable only to an active detailed
			// consumer. Avoid two nested-vector copies per instance in ordinary play.
			destination.candidates = source.candidates;
		}
		destination.policyIdentity = source.policyIdentity;
		destination.calibrationIdentity = source.calibrationIdentity;
		destination.source = source.source;
		destination.estimatedCost = source.estimatedCost;
		destination.measuredCost = source.measuredCost;
		destination.confidenceMargin = source.confidenceMargin;
		destination.stableFrameCount = source.stableFrameCount;
		destination.fallbackReason = source.fallbackReason;
		destination.fallbackDiagnostic = source.fallbackDiagnostic;
		destination.routeGeneration = source.routeGeneration;
		destination.deterministicHash = source.deterministicHash;
		destination.nextState = source.nextState;
		return destination;
	};
	const CanonicalGeometryRouteFacts emptyRouteFacts{};
	TerraEngine::UUID preparedMeshId = TerraEngine::EMPTY_UUID;
	const Mesh* preparedMesh = nullptr;
	const CanonicalGeometryRouteFacts* preparedRouteFacts = &emptyRouteFacts;
	std::array<std::optional<TerraEngine::Renderer::VirtualGeometryDrawRange>, 2U>*
		preparedVirtualRanges = nullptr;
	CachedGeometryRouteDecision* preparedRouteDecision = nullptr;
	TerraEngine::UUID lastSnapshotMeshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID lastSnapshotMaterialId = TerraEngine::EMPTY_UUID;
	const std::uint64_t modelMatrixSceneGeneration = capturedSceneGeneration;
	const std::uint64_t modelMatrixSpatialRevision = capturedSpatialRevision;
	const std::uint64_t modelMatrixComponentRevision =
		capturedComponentMutationRevision;
	const std::uint64_t modelMatrixHierarchyRevision = capturedHierarchyRevision;
	const bool reuseModelMatrixShape =
		renderableWorldPoseCache.size() == renderables.size()
		&& renderableModelMatrixSceneGeneration == modelMatrixSceneGeneration
		&& renderableModelMatrixComponentRevision == modelMatrixComponentRevision
		&& renderableModelMatrixHierarchyRevision == modelMatrixHierarchyRevision;
	const bool reuseModelMatrices = reuseModelMatrixShape
		&& renderableModelMatrixSpatialRevision == modelMatrixSpatialRevision;
	if(!reuseModelMatrices)
	{
		TERRAENGINE_PROFILE_SCOPE(
			"RenderSystem: Refresh Renderable Model Matrices",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const bool sparseTransformRefresh = reuseModelMatrixShape
			&& transformPropagation != nullptr && transformPropagation->valid
			&& !transformPropagation->records.empty()
			&& transformPropagation->records.size() < renderables.size();
		if(sparseTransformRefresh)
		{
			for(const TerraEngine::Scene::SceneWorldTransformRecord& changedTransform :
				transformPropagation->records)
			{
				const auto changedRenderable = std::lower_bound(
					renderables.begin(), renderables.end(), changedTransform.objectId,
					[](const RenderableEntry& entry, const TerraEngine::UUID& objectId)
					{
						return entry.objectId < objectId;
					});
				if(changedRenderable != renderables.end()
					&& changedRenderable->objectId == changedTransform.objectId)
				{
					const std::size_t index = static_cast<std::size_t>(
						changedRenderable - renderables.begin());
					renderableWorldPoseCache[index] = changedTransform.worldPose;
				}
			}
		}
		else
		{
			renderableWorldPoseCache.resize(renderables.size());
			for(std::size_t index = 0U; index < renderables.size(); ++index)
			{
				const auto transformIt = worldTransforms->find(renderables[index].entity);
				renderableWorldPoseCache[index] = transformIt != worldTransforms->end()
					? transformIt->second : TerraEngine::Scene::WorldPose{};
			}
		}
		renderableModelMatrixSceneGeneration = modelMatrixSceneGeneration;
		renderableModelMatrixSpatialRevision = modelMatrixSpatialRevision;
		renderableModelMatrixComponentRevision = modelMatrixComponentRevision;
		renderableModelMatrixHierarchyRevision = modelMatrixHierarchyRevision;
	}
	for (std::size_t renderableIndex = 0U;
		renderableIndex < renderables.size(); ++renderableIndex)
	{
		RenderableEntry& entry = renderables[renderableIndex];
		if(preparedMeshId != entry.mesh->meshId) {
			// Most crowds contain long runs of one mesh. Retain only the immediately
			// preceding lookup results so identical adjacent instances avoid four
			// unordered-map probes while mixed scenes preserve the general cache path.
			preparedMeshId = entry.mesh->meshId;
			preparedMesh = entry.runtimeMesh;
			preparedVirtualRanges = &virtualDrawRangeByMesh[entry.mesh->meshId];
			preparedRouteFacts = preparedMesh != nullptr
				? &getGeometryRouteFacts(entry.mesh->meshId, *preparedMesh)
				: &emptyRouteFacts;
			const auto existingRoute = geometryRouteDecisionByMesh.find(
				entry.mesh->meshId);
			preparedRouteDecision = existingRoute != geometryRouteDecisionByMesh.end()
				? &existingRoute->second : nullptr;
		}
		TerraEngine::Renderer::VirtualGeometryDrawRange virtualRange{};
		const Mesh* mesh = preparedMesh;
		if (mesh != nullptr && mesh->hasVirtualGeometry)
		{
			const std::size_t cullingIndex = entry.mesh->taskShaderCullingEnabled ? 1U : 0U;
			auto& cachedRanges = *preparedVirtualRanges;
			if(cachedRanges[cullingIndex].has_value()) {
				virtualRange = *cachedRanges[cullingIndex];
			} else {
				const VirtualGeometryAssetRuntime& runtime =
					getVirtualGeometryRuntime(entry.mesh->meshId, *mesh);
				virtualRange = SelectVirtualGeometryResidentLodRange(
					runtime,
					renderFrameSnapshot.renderSettings.virtualGeometry,
					512.0f,
					entry.mesh->taskShaderCullingEnabled,
					!mesh->meshletBounds.empty());
				cachedRanges[cullingIndex] = virtualRange;
			}
		}
		renderFrameSnapshot.renderables.push_back({
			entry.objectId,
			entry.mesh->meshId,
			entry.mesh->materialId,
			glm::mat4(1.0F),
			virtualRange,
			entry.mesh->taskShaderCullingEnabled,
			false
		});
		TerraEngine::Renderer::RenderFrameRenderableRecord& snapshotRenderable =
			renderFrameSnapshot.renderables.back();
		snapshotRenderable.canonicalWorldPose =
			renderableWorldPoseCache[renderableIndex];
		snapshotRenderable.transformGeneration = entry.transformGeneration;
		// Static meshes do not carry an animation binding, but they still need a
		// complete immutable resource lifetime before persistent topology can publish
		// their direct draw selectors. Animated bindings below replace these broad
		// frame baselines with their more specific mesh/palette generations.
		snapshotRenderable.invalidation.asset = std::max<std::uint64_t>(1u,
			entry.meshIdentity);
		snapshotRenderable.invalidation.material = std::max<std::uint64_t>(1u,
			entry.materialIdentity);
		snapshotRenderable.invalidation.renderer = std::max<std::uint64_t>(1u,
			snapshotRendererGeneration);
		snapshotRenderable.invalidation.residency = std::max<std::uint64_t>(1u,
			renderFrameSnapshot.resourceRevision);
		// Copy representation facts into the frame before view-specific work. A
		// direct route clears its VG span, preventing hierarchy/page/executor work
		// while leaving conventional mesh-task submission and resource uploads intact.
		const CanonicalGeometryRouteFacts& routeFacts = *preparedRouteFacts;
		const std::uint64_t routeObjectIdentity = entry.objectIdentity;
		if(preparedRouteDecision == nullptr) {
			TerraEngine::Renderer::GeometryRoutePolicyInput routeInput{};
			// Route costs are calibrated from canonical topology, physical directory,
			// and decoded-byte facts. Optional meshlet bounds deliberately contribute
			// nothing: they are a culling acceleration structure, not geometry truth.
			// Repeated instances share these immutable facts, so calculate them once
			// for the mesh instead of once for every object in a crowd.
			const float sourceTriangleWork =
				static_cast<float>(routeFacts.sourceTriangleCount);
			const float meshletWork = static_cast<float>(routeFacts.packedMeshletCount);
			const float decodedKiB = static_cast<float>(routeFacts.decodedBytes) / 1024.0F;
			const float directCost = sourceTriangleWork + meshletWork * 8.0F
				+ decodedKiB * 0.125F;
			const float virtualDirectoryWork =
				static_cast<float>(routeFacts.physicalPageCount) * 24.0F
				+ static_cast<float>(routeFacts.physicalBlockCount) * 3.0F;
			routeInput.direct.valid = routeFacts.directUsable;
			routeInput.direct.estimatedCost = routeFacts.directUsable
				? std::max(1.0F, directCost) : 0.0F;
			routeInput.virtualGeometryHardware.valid = routeFacts.virtualGeometryUsable;
			routeInput.virtualGeometryHardware.capabilitiesAvailable =
				hardwareVirtualGeometryRouteAvailable;
			routeInput.virtualGeometryHardware.estimatedCost = routeFacts.virtualGeometryUsable
				? std::max(1.0F, sourceTriangleWork * 0.45F + meshletWork * 2.0F
					+ decodedKiB * 0.050F + virtualDirectoryWork)
				: 0.0F;
			routeInput.virtualGeometryCompute.valid = routeFacts.virtualGeometryUsable;
			routeInput.virtualGeometryCompute.capabilitiesAvailable =
				computeVirtualGeometryRouteAvailable;
			routeInput.virtualGeometryCompute.estimatedCost = routeFacts.virtualGeometryUsable
				? std::max(1.0F, sourceTriangleWork * 0.58F + meshletWork * 3.0F
					+ decodedKiB * 0.075F + virtualDirectoryWork)
				: 0.0F;
			routeInput.calibrationIdentity = routeFacts.calibrationIdentity == 0u
				? 1u : routeFacts.calibrationIdentity;
			if(validationRouteOverride.has_value()) {
				switch(*validationRouteOverride) {
				case TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed:
					routeInput.authorOverride =
						TerraEngine::Renderer::GeometryRoute::DirectMeshlet;
					break;
				case TerraEngine::Renderer::VirtualGeometryRasterRoute::Hardware:
					routeInput.authorOverride =
						TerraEngine::Renderer::GeometryRoute::VirtualGeometryHardware;
					break;
				case TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute:
					routeInput.authorOverride =
						TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute;
					break;
				case TerraEngine::Renderer::VirtualGeometryRasterRoute::Fallback:
					// The setter rejects this retired validation choice. Keep the
					// snapshot automatic if external code somehow supplies it.
					break;
				}
			}
			const std::uint64_t routeMeshIdentity = entry.meshIdentity;
			// Calibration identity distinguishes recooked topology while the mesh
			// identity keeps unrelated resources from sharing hysteresis history.
			const std::uint64_t routeStateKey = routeMeshIdentity
				^ (routeInput.calibrationIdentity << 1U);
			const auto priorRouteState = geometryRoutePolicyStates.find(routeStateKey);
			TerraEngine::Renderer::GeometryRouteDecision preparedRoute =
				priorRouteState != geometryRoutePolicyStates.end()
				? TerraEngine::Renderer::SelectGeometryRouteCompact(
					routeInput, priorRouteState->second)
				: TerraEngine::Renderer::SelectGeometryRouteCompact(routeInput);
			geometryRoutePolicyStates[routeStateKey] = preparedRoute.nextState;
			const auto cachedRoute = geometryRouteDecisionByMesh.emplace(
				entry.mesh->meshId,
				CachedGeometryRouteDecision{
					std::move(preparedRoute), routeMeshIdentity}).first;
			preparedRouteDecision = &cachedRoute->second;
		}
		const TerraEngine::Renderer::GeometryRouteDecision& preparedRoute =
			preparedRouteDecision->decision;
		const std::uint64_t routeMeshIdentity =
			preparedRouteDecision->meshIdentity;
		TerraEngine::Renderer::GeometryRouteSelectionRecord routeSelection{};
		routeSelection.objectIdentity = routeObjectIdentity;
		routeSelection.meshIdentity = routeMeshIdentity;
		routeSelection.assetGeneration = snapshotRendererGeneration;
		routeSelection.decisionIdentity = routeObjectIdentity
			^ (routeMeshIdentity << 1u) ^ activeRenderFrameId;
		routeSelection.selected = preparedRoute.selected;
		routeSelection.policyIdentity = preparedRoute.policyIdentity;
		routeSelection.calibrationIdentity = preparedRoute.calibrationIdentity;
		routeSelection.source = preparedRoute.source;
		routeSelection.stableFrameCount = preparedRoute.stableFrameCount;
		routeSelection.fallbackReason = preparedRoute.fallbackReason;
		routeSelection.routeGeneration = snapshotRendererGeneration;
		geometryRouteStats.directCount += routeSelection.selected
			== TerraEngine::Renderer::GeometryRoute::DirectMeshlet ? 1U : 0U;
		geometryRouteStats.virtualCount += (routeSelection.selected
			== TerraEngine::Renderer::GeometryRoute::VirtualGeometryHardware
			|| routeSelection.selected
				== TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute) ? 1U : 0U;
		geometryRouteStats.policyIdentity = routeSelection.policyIdentity;
		geometryRouteStats.calibrationIdentity = routeSelection.calibrationIdentity;
		geometryRouteStats.hysteresisFrames = std::max(
			geometryRouteStats.hysteresisFrames, routeSelection.stableFrameCount);
		geometryRouteStats.source = routeSelection.source;
		geometryRouteSelectionSignature ^=
			static_cast<std::uint64_t>(routeSelection.selected) + 1U;
		geometryRouteSelectionSignature *= 1099511628211ULL;
		snapshotRenderable.geometryRouteInput.sourceTriangleCount =
			routeFacts.sourceTriangleCount;
		snapshotRenderable.geometryRouteInput.packedMeshletCount =
			routeFacts.packedMeshletCount;
		snapshotRenderable.geometryRouteInput.baseStoredBytes = routeFacts.storedBytes;
		snapshotRenderable.geometryRouteInput.baseDecodedBytes = routeFacts.decodedBytes;
		snapshotRenderable.geometryRouteInput.availableRepresentationMask =
			routeFacts.representationFlags;
		snapshotRenderable.geometryRouteInput.calibrationClassIdentity =
			preparedRoute.calibrationIdentity;
		snapshotRenderable.geometryRouteInput.calibrationVersion =
			mesh != nullptr ? mesh->runtimeLayout.version : 0u;
		snapshotRenderable.geometryRouteInput.directBatchEligible =
			routeSelection.selected == TerraEngine::Renderer::GeometryRoute::DirectMeshlet;
		snapshotRenderable.geometryRouteInput.repeatedInstanceStorageCompatible = true;
		snapshotRenderable.geometryRouteInput.deterministicSummaryHash =
			routeSelection.decisionIdentity;
		snapshotRenderable.geometryRouteDecision = routeSelection;
		if(retainDetailedRouteCandidates)
		{
			TerraEngine::Renderer::GeometryRouteDecision routeDecision =
				copyRouteDecisionForFrame(preparedRoute);
			routeDecision.frameId = activeRenderFrameId;
			routeDecision.objectIdentity = routeSelection.objectIdentity;
			routeDecision.meshIdentity = routeSelection.meshIdentity;
			routeDecision.assetGeneration = routeSelection.assetGeneration;
			routeDecision.decisionIdentity = routeSelection.decisionIdentity;
			routeDecision.routeGeneration = routeSelection.routeGeneration;
			routeDecision.deterministicHash = routeSelection.decisionIdentity;
			renderFrameSnapshot.geometryRouteDecisions.push_back(
				std::move(routeDecision));
		}
		if (routeSelection.selected == TerraEngine::Renderer::GeometryRoute::DirectMeshlet)
		{
			snapshotRenderable.virtualGeometry = {};
		}
		const TerraEngine::Renderer::AnimationRenderBinding& binding = entry.animationBinding;
		// Worker selection owns the complete binding once per renderable. Protected
		// publication later strips worker-only ownership and duplicated GPU fields,
		// while every view continues to reuse this resolved source value.
		if(binding.meshId != TerraEngine::EMPTY_UUID) {
			TerraEngine::Renderer::AnimationRenderGenerationRecord& animation =
				snapshotRenderable.animation;
			animation.stableInstanceId = BuildStableAnimationInstanceId(binding, entry.objectId);
			animation.deformablePayloadGeneration = binding.deformablePayloadGeneration;
			animation.rendererGeneration = snapshotRendererGeneration;
			animation.deformationContentIdLow =
				binding.deformationContentIdLow;
			animation.deformationContentIdHigh =
				binding.deformationContentIdHigh;
			if(binding.gpu.IsComplete() && immutableGpuAnimationPlan != nullptr
				&& binding.gpu.workPacketIndex < immutableGpuAnimationPlan->WorkPackets().size()) {
				const TerraEngine::Animation::GpuAnimationWorkPacket& gpuPacket =
					immutableGpuAnimationPlan->WorkPackets()[binding.gpu.workPacketIndex];
				const TerraEngine::Animation::GpuAnimationGenerationRecord& gpuGeneration =
					immutableGpuAnimationPlan->GenerationRecord(gpuPacket.generationRecordIndex);
				animation.poseGeneration = gpuGeneration.workGeneration;
				animation.morphGeneration = gpuGeneration.workGeneration;
				animation.paletteGeneration = binding.gpu.paletteGeneration;
				animation.programGeneration = gpuGeneration.programGeneration;
				animation.animationPlanGeneration = binding.gpu.planGeneration;
				animation.firstGpuWorkPacket = binding.gpu.workPacketIndex;
				animation.gpuWorkPacketCount = 1U;
				animation.gpuStageMask = binding.gpu.stageMask;
				animation.source = binding.gpu.hybridCpuAuthority
					? TerraEngine::Renderer::AnimationRenderEvaluationSource::HybridAuthoritative
					: TerraEngine::Renderer::AnimationRenderEvaluationSource::GpuVisual;
				animation.complete = true;
			} else {
				animation.poseGeneration = binding.posePublicationGeneration;
				// CPU snapshots publish pose and morph arrays atomically. Their one
				// publication identity is shared by every render view.
				animation.morphGeneration = binding.posePublicationGeneration;
				animation.paletteGeneration = binding.posePublicationGeneration;
				if(binding.authorityRow != nullptr && binding.authorityRow->snapshot != nullptr
					&& binding.authorityRow->snapshot->pose != nullptr) {
					animation.programGeneration =
						binding.authorityRow->snapshot->pose->Generation().programGeneration;
				}
				animation.safeFallback = binding.safePose
					|| binding.status != TerraEngine::Renderer::AnimationRenderBindingStatus::Ready;
				animation.source = animation.safeFallback
					? TerraEngine::Renderer::AnimationRenderEvaluationSource::SafeFallback
					: TerraEngine::Renderer::AnimationRenderEvaluationSource::CpuPose;
				animation.complete = !animation.safeFallback && binding.HasCpuPose();
			}

			snapshotRenderable.invalidation = binding.invalidation;
			snapshotRenderable.invalidation.view = activeRenderFrameId;
		}
		// Snapshot assembly is the final owner of the complete worker binding. Move
		// its shared publication handles instead of incrementing and later releasing
		// three reference counts for every member of a large animated crowd. All
		// downstream consumers read the binding from the immutable snapshot record.
		// The renderer-owned capture cache retains GPU/static binding topology across
		// warm frames. Copy only on a rebuilt snapshot; compatible frames skip this
		// entire loop, so the shared presentation owner is not churned per frame.
		snapshotRenderable.animationBinding = entry.animationBinding;
		// Worker assembly consumes only identity values and shared shader owners
		// sealed above. It must never reopen live ECS or loader state after suspend.
		try
		{
			auto shadowInputs = std::make_shared<TerraEngine::Renderer::ShadowCasterStamp>();
			shadowInputs->objectId = entry.objectId;
			shadowInputs->lifetimeGeneration = capturedShadowObjectLifetimes[renderableIndex];
			shadowInputs->meshId = entry.mesh->meshId;
			shadowInputs->assetId = entry.terrainAssetId != TerraEngine::EMPTY_UUID
				? entry.terrainAssetId : entry.mesh->meshId;
			shadowInputs->materialId = entry.mesh->materialId;
			const auto& shadowPose = snapshotRenderable.canonicalWorldPose;
			shadowInputs->worldPosition.cell = {shadowPose.position.cell.x, shadowPose.position.cell.y, shadowPose.position.cell.z};
			shadowInputs->worldPosition.offsetMetres = {shadowPose.position.offsetMetres.x, shadowPose.position.offsetMetres.y, shadowPose.position.offsetMetres.z};
			shadowInputs->rotation = {shadowPose.rotation.w, shadowPose.rotation.x, shadowPose.rotation.y, shadowPose.rotation.z};
			shadowInputs->scale = {shadowPose.scale.x, shadowPose.scale.y, shadowPose.scale.z};
			shadowInputs->staticEligible = entry.isStatic;
			// Authoring is one input to static admission. Backend preparation must
			// independently prove the resources and shader actually used by each draw.
			shadowInputs->staticSupport.authoredStatic = entry.isStatic;
			using ShadowProof = TerraEngine::Renderer::ShadowCasterEvidence;
			const auto addShadowProof = [&](ShadowProof proof) { shadowInputs->evidenceMask |= static_cast<std::uint64_t>(proof); };
			if (shadowInputs->lifetimeGeneration != 0U) addShadowProof(ShadowProof::Identity);
			addShadowProof(ShadowProof::Transform);
			// This is the actual resource publication counter, not an identity hash.
			// One broad counter intentionally over-invalidates when an unrelated asset
			// changes; it safely covers payload, material, shader and texture commits.
			const auto shadowResourceRevision = rendererResourceOwnerCohortStable ? renderFrameSnapshot.resourceRevision : 0;
			if (mesh != nullptr && shadowResourceRevision != 0U)
			{
				shadowInputs->meshRevision = shadowResourceRevision;
				shadowInputs->assetRevision = shadowResourceRevision;
				addShadowProof(ShadowProof::Geometry);
				if (routeFacts.directUsable)
				{
					// Canonical complete owner geometry is retained even when visibility
					// chose a VG route. Current shadows read that whole owner, not streamed
					// pages; backend recording still must prove its actual buffers ready.
					// Sparse VG page packets require their own selected publication proof.
					shadowInputs->pageContentRevision = shadowResourceRevision;
					shadowInputs->payloadContentRevision = shadowResourceRevision;
					shadowInputs->residencyGeneration = shadowResourceRevision;
					addShadowProof(ShadowProof::Residency);
				}
			}
			const auto shadowMaterialEntry = frameMaterialResources.find(entry.mesh->materialId);
			const Material* shadowMaterial = shadowMaterialEntry != frameMaterialResources.end() ? shadowMaterialEntry->second : nullptr;
			if (shadowMaterial != nullptr && shadowResourceRevision != 0U)
			{
				shadowInputs->materialRevision = shadowResourceRevision;
				shadowInputs->shadowBehavior = static_cast<std::uint64_t>(shadowMaterial->shadowBehavior)
					| (static_cast<std::uint64_t>(shadowMaterial->header.alphaMode) << 32U)
					| (static_cast<std::uint64_t>(shadowMaterial->header.flags) << 40U);
				shadowInputs->materialShadowParameters = {shadowMaterial->header.alphaCutoff,
					shadowMaterial->header.baseColorFactor[3], 0.0, 0.0};
				addShadowProof(ShadowProof::Material);
				const auto authoredShader = shadowMaterial->header.shaderCollectionId;
				const bool knownDefault = authoredShader == TerraEngine::EMPTY_UUID
					|| authoredShader == capturedDefaultShaderCollectionId || authoredShader == capturedDefaultTaskShaderCollectionId;
				shadowInputs->shaderId = authoredShader == TerraEngine::EMPTY_UUID ? capturedDefaultTaskShaderCollectionId : authoredShader;
				shadowInputs->shaderRevision = shadowResourceRevision;
				// An authored surface can discard or displace with hidden/time-varying
				// inputs. Its UUID/revision alone does not prove an unchanged silhouette.
				const auto& shadowCollection = shadowInputs->shaderId == capturedDefaultTaskShaderCollectionId
					? capturedShadowTaskCollection : capturedShadowMeshCollection;
				if (knownDefault && shadowInputs->shaderId != TerraEngine::EMPTY_UUID
					&& shadowCollection != nullptr && shadowCollection->collectionGeneration != 0)
				{
					shadowInputs->shaderRevision = shadowCollection->collectionGeneration;
					addShadowProof(ShadowProof::Shader);
					shadowInputs->staticSupport.knownDefaultShadowAbi = true;
				}
				// Default opaque shadow texture samples cannot clip the depth silhouette.
				// Cutout and other texture-dependent silhouettes remain unknown until exact sampling
				// inputs are explicitly supported, forcing the correct full refresh.
				shadowInputs->textureRevision = shadowResourceRevision;
				if (knownDefault && shadowMaterial->header.alphaMode == static_cast<std::uint32_t>(AlphaMode::Opaque))
					addShadowProof(ShadowProof::Texture);
			}
			const bool knownNondeformable = mesh != nullptr && !mesh->hasDeformablePayload;
			// Bind pose on a deformable mesh remains dynamic: a later animation
			// callback can change it without changing the author's static flag.
			shadowInputs->staticSupport.deformationAbsent = knownNondeformable
				&& !binding.HasCpuDeformation() && !binding.gpu.IsComplete();
			const bool knownBindPose = mesh != nullptr && binding.meshId == entry.mesh->meshId
				&& binding.deformablePayloadGeneration == mesh->deformablePayloadGeneration
				&& binding.source == TerraEngine::Renderer::AnimationRenderPoseSource::BindPose
				&& binding.status == TerraEngine::Renderer::AnimationRenderBindingStatus::Ready;
			if (knownNondeformable || knownBindPose)
				addShadowProof(ShadowProof::Deformation); // Explicit known bind-pose zero.
			else if (!binding.gpu.IsComplete() && binding.HasCpuDeformation()
				&& binding.cpuDeformation->jointCount == binding.cpuDeformation->paletteMatrices.size())
			{
				const auto& cpu = *binding.cpuDeformation;
				shadowInputs->animationInstanceId = BuildStableAnimationInstanceId(binding, entry.objectId);
				shadowInputs->deformationGeneration = binding.deformablePayloadGeneration;
				shadowInputs->deformationContentIdLow = cpu.contentIdLow;
				shadowInputs->deformationContentIdHigh = cpu.contentIdHigh;
				shadowInputs->deformationPalette.reserve(cpu.paletteMatrices.size());
				for (const auto& palette : cpu.paletteMatrices)
				{
					std::array<double, 16> matrix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
					for (std::size_t component = 0; component < 4; ++component)
					{
						matrix[component] = palette.row0[component];
						matrix[4 + component] = palette.row1[component];
						matrix[8 + component] = palette.row2[component];
					}
					shadowInputs->deformationPalette.push_back(matrix);
				}
				shadowInputs->morphWeights.assign(cpu.morphWeights.begin(), cpu.morphWeights.end());
				addShadowProof(ShadowProof::Deformation);
			}
			// Pure GPU work/palette publication generations are transient selectors;
			// their output cannot be proven exact without a CPU-owned immutable value.
			snapshotRenderable.shadowCasterInputs = std::move(shadowInputs);
		}
		catch (const std::bad_alloc&)
		{
			// Missing copied evidence conservatively refreshes; allocation failure
			// must never publish a partially built known-content proof.
			snapshotRenderable.shadowCasterInputs.reset();
		}
		if(lastSnapshotMeshId != entry.mesh->meshId) {
			lastSnapshotMeshId = entry.mesh->meshId;
			if(snapshotMeshIds.emplace(entry.mesh->meshId).second) {
				renderFrameSnapshot.meshes.push_back({
					entry.mesh->meshId, "mesh", true, false });
			}
		}
		if(lastSnapshotMaterialId != entry.mesh->materialId) {
			lastSnapshotMaterialId = entry.mesh->materialId;
			if(snapshotMaterialIds.emplace(entry.mesh->materialId).second) {
				renderFrameSnapshot.materials.push_back({
					entry.mesh->materialId, "material", true, false });
			}
		}
	}
	}
	else
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Render Snapshot Records",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Route policy and batch membership are immutable under the source signature.
		// Rebuild only bounded scalar diagnostics; no mesh, material, resource, route,
		// transform, or animation-binding record is reconstructed on this path.
		for(const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable :
			renderFrameSnapshot.renderables)
		{
			const TerraEngine::Renderer::GeometryRouteSelectionRecord& route =
				renderable.geometryRouteDecision;
			geometryRouteStats.directCount += route.selected
				== TerraEngine::Renderer::GeometryRoute::DirectMeshlet ? 1u : 0u;
			geometryRouteStats.virtualCount += route.selected
				== TerraEngine::Renderer::GeometryRoute::VirtualGeometryHardware
				|| route.selected
					== TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute
				? 1u : 0u;
			geometryRouteStats.sharedInstances += route.selected
				== TerraEngine::Renderer::GeometryRoute::DirectMeshlet ? 1u : 0u;
			geometryRouteStats.policyIdentity = route.policyIdentity;
			geometryRouteStats.calibrationIdentity = route.calibrationIdentity;
			geometryRouteStats.hysteresisFrames = std::max(
				geometryRouteStats.hysteresisFrames, route.stableFrameCount);
			geometryRouteStats.source = route.source;
			geometryRouteSelectionSignature ^=
				static_cast<std::uint64_t>(route.selected) + 1u;
			geometryRouteSelectionSignature *= 1099511628211ULL;
		}
	}
	renderFrameSnapshot.viewTransforms.clear();
	renderFrameSnapshot.viewTransforms.reserve(
		renderFrameSnapshot.viewRequests.size()
		* renderFrameSnapshot.renderables.size());
	for (const TerraEngine::Renderer::RenderFrameViewRequest& viewRequest :
		renderFrameSnapshot.viewRequests)
	{
		const TerraEngine::Renderer::RenderReferenceFrame* previousFrame = nullptr;
		for (const TerraEngine::Renderer::RenderFrameViewRequest& previousRequest :
			previousViewRequests)
		{
			if (previousRequest.referenceFrame.viewId
					== viewRequest.referenceFrame.viewId
				&& previousRequest.referenceFrame.outputId
					== viewRequest.referenceFrame.outputId)
			{
				previousFrame = &previousRequest.referenceFrame;
				break;
			}
		}
		for (const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable :
			renderFrameSnapshot.renderables)
		{
			const TerraEngine::Renderer::RenderLocalizedPoseMatrices localized =
				TerraEngine::Renderer::BuildLocalizedRenderPoseMatrices(
					renderable.canonicalWorldPose,
					viewRequest.referenceFrame, previousFrame);
			TerraEngine::Renderer::RenderViewTransformRecord record{};
			record.objectId = renderable.objectId;
			record.viewId = viewRequest.referenceFrame.viewId;
			record.originGeneration =
				viewRequest.referenceFrame.originGeneration;
			record.currentMatrix = localized.currentMatrix;
			record.previousMatrix = localized.previousMatrix;
			record.valid = localized.valid && !localized.staleReferenceFrame;
			renderFrameSnapshot.viewTransforms.push_back(record);
		}
	}
	{
	TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Direct Meshlet Batches",
		TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	if(!reuseRenderableRecords)
	{
	std::size_t activeDirectBatchCount = 0U;
	for(const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable :
		renderFrameSnapshot.renderables)
	{
		if(renderable.geometryRouteDecision.selected
			!= TerraEngine::Renderer::GeometryRoute::DirectMeshlet)
		{
			continue;
		}
		const std::uint64_t geometryIdentity =
			renderable.geometryRouteDecision.meshIdentity;
		const std::uint64_t materialIdentity = static_cast<std::uint64_t>(
			std::hash<TerraEngine::UUID>{}(renderable.materialId));
		auto activeBatchEnd = renderFrameSnapshot.directMeshletBatches.begin()
			+ static_cast<std::ptrdiff_t>(activeDirectBatchCount);
		auto batch = std::find_if(renderFrameSnapshot.directMeshletBatches.begin(),
			activeBatchEnd,
			[geometryIdentity, materialIdentity](
				const TerraEngine::Renderer::DirectMeshletBatch& value)
			{
				return value.geometryIdentity == geometryIdentity
					&& value.materialIdentity == materialIdentity;
			});
		if(batch == activeBatchEnd)
		{
			if(activeDirectBatchCount == renderFrameSnapshot.directMeshletBatches.size())
			{
				renderFrameSnapshot.directMeshletBatches.emplace_back();
			}
			batch = renderFrameSnapshot.directMeshletBatches.begin()
				+ static_cast<std::ptrdiff_t>(activeDirectBatchCount++);
			batch->geometryIdentity = geometryIdentity;
			// Direct batches retain meshlet spans, so hot reload must give their
			// immutable snapshot identity a new resource generation as well.
			batch->geometryGeneration = renderFrameSnapshot.resourceRevision;
			batch->materialIdentity = materialIdentity;
			batch->firstMeshlet = 0U;
			batch->meshletCount = 0U;
			const auto cachedDirectMesh = frameMeshResources.find(renderable.meshId);
			const Mesh* directMesh = cachedDirectMesh != frameMeshResources.end()
				? cachedDirectMesh->second : nullptr;
			if(directMesh != nullptr
				&& !directMesh->virtualGeometryDetailLevels.empty())
			{
				const VirtualGeometryDetailLevelRecord& baseDetail =
					directMesh->virtualGeometryDetailLevels.front();
				batch->firstMeshlet = baseDetail.meshletOffset;
				batch->meshletCount = baseDetail.meshletCount;
			}
			else
			{
				batch->meshletCount =
					renderable.geometryRouteInput.packedMeshletCount;
			}
		}
		// A batch shares one immutable geometry allocation across every matching
		// object, while retaining stable identities for per-instance state.
		batch->instanceIdentities.push_back(
			renderable.geometryRouteDecision.objectIdentity);
		++geometryRouteStats.sharedInstances;
	}
	renderFrameSnapshot.directMeshletBatches.resize(activeDirectBatchCount);
	}
	}
	geometryRouteSelectionSignature ^=
		static_cast<std::uint64_t>(renderFrameSnapshot.renderables.size());
	renderFrameSnapshot.lights.reserve(lights.size());
	for (const LightEntry& light : lights)
	{
		renderFrameSnapshot.lights.push_back({
			light.lightId,
			static_cast<std::uint32_t>(light.light.type),
			light.light.castsShadows,
			false
		});
	}
	// Candidate construction is deferred until every view has resolved visibility.
	// The callable captures immutable frame records, but performs no work here;
	// this prevents offscreen crowds from allocating and scanning every cooked page.
	struct AnimatedGeometryBuildResult
	{
		std::shared_ptr<const TerraEngine::Renderer::GpuDeformationPlan> immutablePlan;
		std::size_t demandCount = 0u;
	};
	const auto buildAnimatedGeometryPlan = [&renderFrameSnapshot, &frameMeshResources,
		&snapshotRendererGeneration, &protectedSceneGeneration](
		std::span<const std::uint8_t> visibleAnimatedRenderables,
		const std::unordered_map<TerraEngine::UUID, std::size_t>&
			renderableIndicesByObjectId)
	{
		struct DeformablePageShape
		{
			std::uint32_t pageId = 0u;
			std::uint64_t contentGeneration = 0u;
			std::uint32_t vertexCount = 0u;
			std::uint32_t meshletCount = 0u;
		};
		struct DeformablePageFamily
		{
			TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
			std::vector<DeformablePageShape> pages;
		};
		struct AnimatedPageDemandSource
		{
			std::size_t renderableIndex = 0u;
			std::size_t pageFamilyIndex = 0u;
			std::size_t exactPageMaskOffset = 0u;
			std::uint64_t fallbackViewMask = 0u;
		};
		AnimatedGeometryBuildResult result;
		// A heterogeneous crowd still contains only a small number of exact mesh
		// families. Parse each immutable demanded-page directory once per frame,
		// then stamp the instance-specific generations onto the retained shape.
		std::vector<DeformablePageFamily> pageFamilies;
		pageFamilies.reserve(16u);
		thread_local std::vector<AnimatedPageDemandSource> demandSources;
		thread_local std::vector<std::size_t> sourceIndicesByRenderable;
		thread_local std::vector<std::uint64_t> resolvedExactPageMasks;
		demandSources.clear();
		demandSources.reserve(renderFrameSnapshot.renderables.size());
		sourceIndicesByRenderable.assign(renderFrameSnapshot.renderables.size(),
			std::numeric_limits<std::size_t>::max());
		resolvedExactPageMasks.clear();
		resolvedExactPageMasks.reserve(renderFrameSnapshot.renderables.size() * 3u);
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Gather Animated Page Sources",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			for(std::size_t renderableIndex = 0u;
				renderableIndex < renderFrameSnapshot.renderables.size(); ++renderableIndex)
			{
				const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable =
					renderFrameSnapshot.renderables[renderableIndex];
				if(renderableIndex >= visibleAnimatedRenderables.size()
					|| visibleAnimatedRenderables[renderableIndex] == 0u
					|| !TerraEngine::Renderer::
						CanScheduleGpuAnimatedPageDeformation(renderable.animation))
				{
					continue;
				}
				auto family = std::find_if(pageFamilies.begin(), pageFamilies.end(),
					[&renderable](const DeformablePageFamily& candidate)
					{
						return candidate.meshId == renderable.meshId;
					});
				if(family == pageFamilies.end())
				{
					DeformablePageFamily candidate;
					candidate.meshId = renderable.meshId;
					const auto capturedMesh = frameMeshResources.find(renderable.meshId);
					const Mesh* mesh = capturedMesh != frameMeshResources.end()
						? capturedMesh->second : nullptr;
					if(mesh != nullptr && mesh->hasDeformableDemandedPages
						&& mesh->deformableDemandedPageEntries.size()
							== mesh->deformableDemandedPageSections.size())
					{
						candidate.pages.reserve(mesh->deformableDemandedPageEntries.size());
						for(std::size_t pageIndex = 0u;
							pageIndex < mesh->deformableDemandedPageEntries.size(); ++pageIndex)
						{
							DeformablePageShape pageShape{};
							pageShape.pageId = mesh->deformableDemandedPageEntries[pageIndex].pageId;
							pageShape.contentGeneration =
								mesh->deformableDemandedPageEntries[pageIndex].contentGeneration;
							for(const TemfDeformablePageSection& section :
								mesh->deformableDemandedPageSections[pageIndex])
							{
								if(section.kind == TemfDeformablePageSectionKind::PackedVertices)
								{
									pageShape.vertexCount = section.elementCount;
								}
								else if(section.kind == TemfDeformablePageSectionKind::Meshlets)
								{
									pageShape.meshletCount = section.elementCount;
								}
							}
							if(pageShape.vertexCount != 0u && pageShape.meshletCount != 0u)
							{
								candidate.pages.push_back(pageShape);
							}
						}
					}
					pageFamilies.push_back(std::move(candidate));
					family = std::prev(pageFamilies.end());
				}
				const std::size_t sourceIndex = demandSources.size();
				const std::size_t exactPageMaskOffset = resolvedExactPageMasks.size();
				sourceIndicesByRenderable[renderableIndex] = sourceIndex;
				demandSources.push_back({
					renderableIndex,
					static_cast<std::size_t>(family - pageFamilies.begin()),
					exactPageMaskOffset,
					0u
				});
				resolvedExactPageMasks.resize(exactPageMaskOffset + family->pages.size(), 0u);
			}
		}
		thread_local std::vector<std::uint64_t> fallbackViewMasks;
		fallbackViewMasks.assign(renderFrameSnapshot.renderables.size(), 0U);
		const auto findExactPageMask = [&](std::size_t renderableIndex,
			std::uint32_t pageId) -> std::uint64_t*
		{
			if(renderableIndex >= sourceIndicesByRenderable.size())
			{
				return nullptr;
			}
			const std::size_t sourceIndex = sourceIndicesByRenderable[renderableIndex];
			if(sourceIndex >= demandSources.size())
			{
				return nullptr;
			}
			const AnimatedPageDemandSource& source = demandSources[sourceIndex];
			const std::vector<DeformablePageShape>& pages =
				pageFamilies[source.pageFamilyIndex].pages;
			for(std::size_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
			{
				if(pages[pageIndex].pageId == pageId)
				{
					return &resolvedExactPageMasks[source.exactPageMaskOffset + pageIndex];
				}
			}
			return nullptr;
		};
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Accumulate Deformation View Masks",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			// The canonical renderable index addresses a compact per-family mask range.
			// Exact cluster or packet pages win per view, while unmatched visible draws
			// safely retain every validated page owned by that animator.
			for(std::size_t viewIndex = 0u;
				viewIndex < renderFrameSnapshot.virtualGeometryViewSelections.size(); ++viewIndex)
			{
				const std::uint64_t viewMask = std::uint64_t{1u}
					<< std::min<std::size_t>(viewIndex, 63u);
				const TerraEngine::Renderer::VirtualGeometryViewSelectionState& view =
					renderFrameSnapshot.virtualGeometryViewSelections[viewIndex];
				for(const TerraEngine::Renderer::VirtualGeometryViewDrawRecord& draw :
					view.DrawRecords())
				{
					std::size_t renderableIndex = draw.renderableIndex;
					if(renderableIndex >= renderFrameSnapshot.renderables.size())
					{
						const auto renderable = renderableIndicesByObjectId.find(draw.objectId);
						if(renderable == renderableIndicesByObjectId.end())
						{
							continue;
						}
						renderableIndex = renderable->second;
					}
					if(renderableIndex < sourceIndicesByRenderable.size()
						&& sourceIndicesByRenderable[renderableIndex] < demandSources.size()
						&& draw.range.meshletCount != 0u
						&& (draw.range.virtualGeometryActive || draw.range.failOpen))
					{
						fallbackViewMasks[renderableIndex] |= viewMask;
					}
					const std::size_t firstCluster = std::min<std::size_t>(
						draw.range.selectedClusterOffset, view.SelectedClusters().size());
					const std::size_t clusterCount = std::min<std::size_t>(
						draw.range.selectedClusterCount,
						view.SelectedClusters().size() - firstCluster);
					for(std::size_t clusterIndex = firstCluster;
						clusterIndex < firstCluster + clusterCount; ++clusterIndex)
					{
						const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& cluster =
							view.SelectedClusters()[clusterIndex];
						if(cluster.objectId == draw.objectId)
						{
							if(std::uint64_t* exactMask = findExactPageMask(
								renderableIndex, cluster.pageId))
							{
								*exactMask |= viewMask;
							}
						}
					}
				}
				for(const GpuVirtualGeometryPacketRecord& packet : view.GpuPacketRecords())
				{
					const bool exactPage = packet.payloadSource
						== VirtualGeometryPagePayloadSource::ResidentPagePayload
						|| packet.payloadSource
						== VirtualGeometryPagePayloadSource::ResidentFallbackPayload
						|| packet.payloadSource
						== VirtualGeometryPagePayloadSource::EmbeddedMeshPayload
						|| packet.payloadSource
						== VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
					if(exactPage)
					{
						std::size_t renderableIndex = packet.renderableIndex;
						if(renderableIndex >= renderFrameSnapshot.renderables.size()
							|| renderFrameSnapshot.renderables[renderableIndex].objectId
								!= packet.objectId)
						{
							const auto renderable = renderableIndicesByObjectId.find(
								packet.objectId);
							if(renderable == renderableIndicesByObjectId.end())
							{
								continue;
							}
							renderableIndex = renderable->second;
						}
						if(std::uint64_t* exactMask = findExactPageMask(
							renderableIndex, packet.pageId))
						{
							*exactMask |= viewMask;
						}
					}
				}
			}
			for(AnimatedPageDemandSource& source : demandSources)
			{
				const std::vector<DeformablePageShape>& pages =
					pageFamilies[source.pageFamilyIndex].pages;
				std::uint64_t matchedViewMask = 0u;
				for(std::size_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
				{
					matchedViewMask |= resolvedExactPageMasks[
						source.exactPageMaskOffset + pageIndex];
				}
				source.fallbackViewMask = fallbackViewMasks[source.renderableIndex]
					& ~matchedViewMask;
			}
		}
		TerraEngine::Renderer::GpuDeformationPlanAccumulator accumulator(
			renderFrameSnapshot.frameId,
			snapshotRendererGeneration,
			std::min(visibleAnimatedRenderables.size(),
				renderFrameSnapshot.renderables.size()));
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Accumulate Animated Page Demands",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			for(const AnimatedPageDemandSource& source : demandSources)
			{
				const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable =
					renderFrameSnapshot.renderables[source.renderableIndex];
				const std::vector<DeformablePageShape>& pages =
					pageFamilies[source.pageFamilyIndex].pages;
				for(std::size_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
				{
					const DeformablePageShape& page = pages[pageIndex];
					const std::uint64_t exactViewMask = resolvedExactPageMasks[
						source.exactPageMaskOffset + pageIndex];
					const std::uint64_t requestedViewMask =
						source.fallbackViewMask | exactViewMask;
					if(requestedViewMask == 0u)
					{
						continue;
					}
					TerraEngine::Renderer::DeformationPageDemand demand;
					demand.key.page.meshId = renderable.meshId;
					demand.key.page.meshPayloadGeneration =
						renderable.animation.deformablePayloadGeneration;
					demand.key.page.pageId = page.pageId;
					demand.key.page.pageContentGeneration = page.contentGeneration;
					demand.key.page.residencyGeneration = page.contentGeneration;
					demand.key.page.layoutVersion =
						TerraEngine::Renderer::kAnimatedGeometryAbiVersion;
					// The runtime page currently uses the expanded production profile.
					// Other profiles publish distinct values and cannot alias this output.
					demand.key.page.codecProfile = 1u;
					demand.key.page.codecVersion = 1u;
					demand.key.deformation.stableInstanceId =
						renderable.animation.stableInstanceId;
					demand.key.deformation.objectGeneration =
						std::max<std::uint64_t>(1u, renderable.invalidation.asset);
					demand.key.deformation.animationPublicationGeneration =
						renderable.animation.poseGeneration;
					demand.key.deformation.skeletonGeneration =
						std::max<std::uint64_t>(1u, renderable.animation.programGeneration);
					demand.key.deformation.morphGeneration =
						renderable.animation.morphGeneration;
					demand.key.deformation.meshPayloadGeneration =
						renderable.animation.deformablePayloadGeneration;
					demand.key.deformation.rendererGeneration = snapshotRendererGeneration;
					demand.key.deformation.sceneGeneration =
						std::max<std::uint64_t>(1u, protectedSceneGeneration);
					demand.key.deformation.contentIdLow =
						renderable.animation.deformationContentIdLow;
					demand.key.deformation.contentIdHigh =
						renderable.animation.deformationContentIdHigh;
					demand.key.inputLayoutVersion =
						TerraEngine::Renderer::kAnimatedGeometryAbiVersion;
					demand.key.animationLod = 0u;
					demand.key.animationLodVersion =
						TerraEngine::Renderer::kAnimatedGeometryAbiVersion;
					demand.sourceRecordIndex = renderable.animation.firstGpuWorkPacket;
					demand.ownerStableInstanceId = renderable.animation.stableInstanceId;
					demand.sourceStableInstanceId = renderable.animation.stableInstanceId;
					demand.vertexCount = page.vertexCount;
					demand.meshletCount = page.meshletCount;
					// Each canonical demanded-page entry owns exactly one cluster.
					demand.clusterCount = 1u;
					demand.requestedViewMask = requestedViewMask;
					demand.priorHistoryCompatible =
						renderable.invalidation.pose == renderable.animation.poseGeneration
						&& renderable.invalidation.morph
							== renderable.animation.morphGeneration;
					++result.demandCount;
					(void)accumulator.Add(demand);
				}
			}
		}
		if(result.demandCount != 0u)
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Finalize Deformation Plan",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			result.immutablePlan =
				std::make_shared<const TerraEngine::Renderer::GpuDeformationPlan>(
					std::move(accumulator).Finish());
		}
		return result;
	};
	std::vector<TerraEngine::Renderer::RenderPassNode> renderGraphPasses;
	RetainedRenderTopologyState& retainedRenderTopology =
		lifetimeState->retainedRenderTopology;
	GroupedSubmissionMembershipCacheState& groupedSubmissionMembershipCache =
		lifetimeState->groupedSubmissionMembershipCache;
	if(groupedSubmissionMembershipCache.owner != this) {
		groupedSubmissionMembershipCache = {};
		groupedSubmissionMembershipCache.owner = this;
	}
	bool renderTopologyRebuilt = false;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Lower Render Snapshot",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto matchesRetainedOrder = [&]() {
			if (retainedRenderTopology.orderedObjectIds.size()
				!= renderFrameSnapshot.renderables.size())
			{
				return false;
			}
			for (std::size_t index = 0u;
				index < renderFrameSnapshot.renderables.size(); ++index)
			{
				if (retainedRenderTopology.orderedObjectIds[index]
					!= renderFrameSnapshot.renderables[index].objectId)
				{
					return false;
				}
			}
			return true;
		};
		// Collectors normally preserve their canonical object order. Verify that
		// proof first and skip an O(n log n) sort for a stable crowd; any change
		// falls back to the existing deterministic sort before publication.
		const bool retainedOrderMatches = matchesRetainedOrder();
		if (!retainedOrderMatches)
		{
			TerraEngine::Renderer::SortSnapshotDeterministically(renderFrameSnapshot);
		}
		const std::uint64_t topologySceneGeneration = capturedSceneGeneration;
		const std::uint64_t topologyComponentMutationRevision =
			capturedComponentMutationRevision;
		const std::uint64_t topologyMeshComponentRevision =
			capturedRenderBindingRevision;
		const std::uint64_t topologyHierarchyRevision = capturedHierarchyRevision;
		// Exact monotonic producers prove whether any immutable input can have
		// changed. On a warm frame, this constant-time gate avoids hashing every
		// mesh/material/group key again. Object order is still checked above because
		// it is the index contract shared with frame-owned dynamic sidecars.
		const bool topologyObservationUnchanged = retainedRenderTopology.topology.valid
			&& retainedRenderTopology.owner == this
			&& retainedOrderMatches
			&& retainedRenderTopology.sceneGeneration == topologySceneGeneration
			&& retainedRenderTopology.componentMutationRevision
				== topologyComponentMutationRevision
			&& retainedRenderTopology.meshComponentRevision == topologyMeshComponentRevision
			&& retainedRenderTopology.hierarchyRevision == topologyHierarchyRevision
			&& retainedRenderTopology.resourceRevision == renderFrameSnapshot.resourceRevision
			&& retainedRenderTopology.rendererGeneration == snapshotRendererGeneration;
		bool topologyInputsChanged = !topologyObservationUnchanged;
		std::uint64_t stableTopologySignature = retainedRenderTopology.stableSignature;
		if (topologyInputsChanged)
		{
			// Fold only membership and immutable compatibility inputs after a producer
			// revision changed. Dynamic transforms, animation samples, visibility, LOD,
			// and page selection remain frame-owned sidecars and never enter this key.
			stableTopologySignature = kRenderHashOffset;
			for (const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable
				: renderFrameSnapshot.renderables)
			{
				const SharedRenderGroupKey key =
					TerraEngine::Renderer::BuildSharedRenderGroupKey(renderable, {});
				HashCombine(stableTopologySignature,
					static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(renderable.objectId)));
				HashCombine(stableTopologySignature,
					static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(key.meshId)));
				HashCombine(stableTopologySignature,
					static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(key.materialId)));
				HashCombine(stableTopologySignature, key.meshGeneration);
				HashCombine(stableTopologySignature, key.materialGeneration);
				HashCombine(stableTopologySignature, key.pipelineGeneration);
				HashCombine(stableTopologySignature, key.residencyGeneration);
				HashCombine(stableTopologySignature, key.rendererGeneration);
				HashCombine(stableTopologySignature, key.qualityVariant);
			}
		}
		if (topologyInputsChanged)
		{
			PersistentRenderTopology denseTopology =
				TerraEngine::Renderer::BuildPersistentRenderTopology(
					renderFrameSnapshot, stableTopologySignature);
			const std::uint64_t slotSessionGeneration =
				std::max<std::uint64_t>(1u, snapshotRendererGeneration);
			if(retainedRenderTopology.slotAllocator.rendererSessionGeneration
				!= slotSessionGeneration
				|| retainedRenderTopology.slotAllocator.topology == nullptr)
			{
				retainedRenderTopology.slotAllocator =
					TerraEngine::Renderer::CreateRenderSlotAllocatorState(
						slotSessionGeneration, 1u);
				retainedRenderTopology.slotKeysByObject.clear();
			}
			const std::uint64_t nextTopologyGeneration =
				retainedRenderTopology.slotAllocator.topologyGeneration + 1u;
			auto slotTransaction =
				TerraEngine::Renderer::BeginRenderSlotAllocatorTransaction(
					retainedRenderTopology.slotAllocator, nextTopologyGeneration);
			auto candidateSlotKeys = retainedRenderTopology.slotKeysByObject;
			std::unordered_set<TerraEngine::UUID> liveObjects;
			liveObjects.reserve(denseTopology.records.size());
			for(const PersistentRenderTopologyRecord& record : denseTopology.records)
				liveObjects.insert(record.objectId);

			// Release membership first so additions in the same protected snapshot can
			// deterministically reuse the lowest free ordinal with a new lifetime.
			for(auto existing = candidateSlotKeys.begin();
				existing != candidateSlotKeys.end();)
			{
				if(liveObjects.contains(existing->first))
				{
					++existing;
					continue;
				}
				if(!slotTransaction.Release(existing->second))
					throw std::runtime_error(
						"Renderer persistent-slot release rejected the captured topology transaction.");
				existing = candidateSlotKeys.erase(existing);
			}
			const auto resolveAllocatedSlot = [](
				const TerraEngine::Renderer::RenderSlotAllocatorState& state,
				TerraEngine::Renderer::RenderSlotGenerationKey key)
				-> const TerraEngine::Renderer::RenderPersistentSlot*
			{
				if(state.topology == nullptr || !key.IsValid()) return nullptr;
				const std::size_t pageIndex = key.ordinal
					/ TerraEngine::Renderer::kRenderPublicationPageCapacity;
				const std::size_t slotIndex = key.ordinal
					% TerraEngine::Renderer::kRenderPublicationPageCapacity;
				if(pageIndex >= state.topology->pages.size()
					|| state.topology->pages[pageIndex] == nullptr
					|| slotIndex >= state.topology->pages[pageIndex]->slots.size())
					return nullptr;
				const TerraEngine::Renderer::RenderPersistentSlot& slot =
					state.topology->pages[pageIndex]->slots[slotIndex];
				return slot.IsComplete() && slot.key == key ? &slot : nullptr;
			};
			for(PersistentRenderTopologyRecord& record : denseTopology.records)
			{
				if(!record.groupKey.IsComplete())
				{
					const auto stale = candidateSlotKeys.find(record.objectId);
					if(stale != candidateSlotKeys.end())
					{
						if(!slotTransaction.Release(stale->second))
							throw std::runtime_error(
								"Renderer incomplete persistent identity could not release its prior slot.");
						candidateSlotKeys.erase(stale);
					}
					continue;
				}
				std::uint64_t packetTemplateIdentity = kRenderHashOffset;
				HashCombine(packetTemplateIdentity, static_cast<std::uint64_t>(
					std::hash<TerraEngine::UUID>{}(record.groupKey.meshId)));
				HashCombine(packetTemplateIdentity, static_cast<std::uint64_t>(
					std::hash<TerraEngine::UUID>{}(record.groupKey.materialId)));
				HashCombine(packetTemplateIdentity, record.groupKey.meshGeneration);
				HashCombine(packetTemplateIdentity, record.groupKey.materialGeneration);
				HashCombine(packetTemplateIdentity, record.groupKey.pipelineGeneration);
				HashCombine(packetTemplateIdentity, record.groupKey.residencyGeneration);
				if(packetTemplateIdentity == 0u) packetTemplateIdentity = 1u;
				TerraEngine::Renderer::RenderSlotAllocationRequest request{};
				request.objectId = record.objectId;
				request.meshId = record.groupKey.meshId;
				request.materialId = record.groupKey.materialId;
				request.meshGeneration = record.groupKey.meshGeneration;
				request.materialGeneration = record.groupKey.materialGeneration;
				request.resourceGeneration = std::max(
					record.groupKey.pipelineGeneration,
					record.groupKey.residencyGeneration);
				request.packetTemplateIdentity = packetTemplateIdentity;
				request.routeTemplate = record.groupTemplateIndex;

				auto current = candidateSlotKeys.find(record.objectId);
				const TerraEngine::Renderer::RenderPersistentSlot* currentSlot =
					current != candidateSlotKeys.end()
					? resolveAllocatedSlot(retainedRenderTopology.slotAllocator,
						current->second) : nullptr;
				const bool persistentInputsMatch = currentSlot != nullptr
					&& currentSlot->meshId == request.meshId
					&& currentSlot->materialId == request.materialId
					&& currentSlot->meshGeneration == request.meshGeneration
					&& currentSlot->materialGeneration == request.materialGeneration
					&& currentSlot->resourceGeneration == request.resourceGeneration
					&& currentSlot->packetTemplateIdentity == request.packetTemplateIdentity
					&& currentSlot->routeTemplate == request.routeTemplate;
				if(persistentInputsMatch)
					continue;
				if(current != candidateSlotKeys.end())
				{
					if(!slotTransaction.Release(current->second))
						throw std::runtime_error(
							"Renderer persistent-slot replacement could not release its old lifetime.");
					candidateSlotKeys.erase(current);
				}
				const std::optional<TerraEngine::Renderer::RenderSlotGenerationKey> allocated =
					slotTransaction.Allocate(request);
				if(!allocated.has_value())
					throw std::runtime_error(
						"Renderer persistent-slot allocation rejected the complete topology transaction.");
				candidateSlotKeys.emplace(record.objectId, *allocated);
			}
			const std::optional<TerraEngine::Renderer::RenderSlotAllocatorState>
				committedSlots =
				slotTransaction.Commit();
			if(!committedSlots.has_value() || committedSlots->topology == nullptr)
				throw std::runtime_error(
					"Renderer persistent-slot transaction did not produce an immutable root.");
			retainedRenderTopology.slotAllocator = *committedSlots;
			retainedRenderTopology.slotKeysByObject = std::move(candidateSlotKeys);

			// Keep the legacy grouping helper as a sparse ordinal-indexed sidecar until
			// its remaining callers move to the publication resolver. Empty records are
			// released slots; live records keep allocator ordinals across mutations.
			PersistentRenderTopology sparseTopology{};
			sparseTopology.generation = committedSlots->topologyGeneration;
			sparseTopology.groupTemplates = denseTopology.groupTemplates;
			sparseTopology.records.resize(committedSlots->nextOrdinal);
			for(PersistentRenderTopologyRecord& record : denseTopology.records)
			{
				const auto key = retainedRenderTopology.slotKeysByObject.find(record.objectId);
				if(key == retainedRenderTopology.slotKeysByObject.end()) continue;
				record.stableOrdinal = key->second.ordinal;
				sparseTopology.records[key->second.ordinal] = record;
			}
			sparseTopology.valid = denseTopology.valid;
			retainedRenderTopology.topology = std::move(sparseTopology);
			retainedRenderTopology.stableSignature = stableTopologySignature;
			retainedRenderTopology.orderedObjectIds.clear();
			retainedRenderTopology.orderedSlotKeys.clear();
			retainedRenderTopology.renderableIndexByOrdinal.assign(
				committedSlots->nextOrdinal,
				TerraEngine::Renderer::RenderSlotGenerationKey::kInvalidOrdinal);
			retainedRenderTopology.conventionalVisibilityIdentityTable.clear();
			retainedRenderTopology.conventionalVisibilityIdentityIndices.clear();
			retainedRenderTopology.orderedObjectIds.reserve(
				renderFrameSnapshot.renderables.size());
			retainedRenderTopology.orderedSlotKeys.reserve(
				renderFrameSnapshot.renderables.size());
			retainedRenderTopology.conventionalVisibilityIdentityTable.resize(
				committedSlots->nextOrdinal);
			retainedRenderTopology.conventionalVisibilityIdentityIndices.reserve(
				renderFrameSnapshot.renderables.size());
			for (std::size_t index = 0u;
				index < renderFrameSnapshot.renderables.size(); ++index)
			{
				// Zero remains the typed attachment's clear/no-producer sentinel.
				TerraEngine::Renderer::RenderFrameRenderableRecord& renderable =
					renderFrameSnapshot.renderables[index];
				const auto key = retainedRenderTopology.slotKeysByObject.find(
					renderable.objectId);
				if(key == retainedRenderTopology.slotKeysByObject.end()) continue;
				const std::uint32_t identityIndex = key->second.ordinal + 1u;
				renderable.conventionalVisibilityIdentityIndex = identityIndex;
				retainedRenderTopology.orderedObjectIds.push_back(renderable.objectId);
				retainedRenderTopology.orderedSlotKeys.push_back(key->second);
				retainedRenderTopology.renderableIndexByOrdinal[key->second.ordinal] =
					static_cast<std::uint32_t>(index);
				retainedRenderTopology.conventionalVisibilityIdentityTable[
					key->second.ordinal] = {
					identityIndex, renderable.objectId, renderable.meshId,
					renderable.materialId };
				retainedRenderTopology.conventionalVisibilityIdentityIndices.emplace(
					renderable.objectId, identityIndex);
			}
			retainedRenderTopology.sceneGeneration = topologySceneGeneration;
			retainedRenderTopology.componentMutationRevision =
				topologyComponentMutationRevision;
			retainedRenderTopology.meshComponentRevision = topologyMeshComponentRevision;
			retainedRenderTopology.hierarchyRevision = topologyHierarchyRevision;
			retainedRenderTopology.resourceRevision = renderFrameSnapshot.resourceRevision;
			retainedRenderTopology.rendererGeneration = snapshotRendererGeneration;
			retainedRenderTopology.owner = this;
			renderTopologyRebuilt = true;
		}
		else
		{
			// The static identity records are already canonical. Only restore the
			// one-based attachment for this independent dynamic snapshot. The retained
			// slot vector is index-aligned with the verified object order, avoiding a
			// UUID hash lookup for every object on every animated frame.
			for (std::size_t index = 0u;
				index < renderFrameSnapshot.renderables.size(); ++index)
			{
				renderFrameSnapshot.renderables[index]
					.conventionalVisibilityIdentityIndex =
					index < retainedRenderTopology.orderedSlotKeys.size()
						? retainedRenderTopology.orderedSlotKeys[index].ordinal + 1u
						: 0u;
			}
		}
		renderFrameSnapshot.conventionalVisibilityIdentityTable =
			retainedRenderTopology.conventionalVisibilityIdentityTable;
		renderGraphPasses = TerraEngine::Renderer::BuildDefaultRenderPasses(renderFrameSnapshot, backendCapabilities);
	}
	// Draw-list materialization consults the retained map instead of reconstructing
	// a UUID lookup for every stable frame. The map contains only static snapshot
	// identities and is replaced atomically at this protected coordinator point.
	const auto& conventionalVisibilityIdentityIndices =
		retainedRenderTopology.conventionalVisibilityIdentityIndices;
	// The protected capture already copied replay counters and generation values.
	// Reaching this point completes worker-owned route/topology preparation.
	TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache& rendererPreparationCache =
		lifetimeState->rendererPreparationCache;
	// This wall span deliberately covers only preparation work that turns the
	// immutable frame snapshot into backend-ready cache and submission data. It
	// excludes diagnostics and later publication, which have independent timing
	// ownership and would otherwise make the profiler buckets overlap.
	const auto backendPreparationStart = std::chrono::steady_clock::now();
	TerraEngine::Renderer::Vulkan::VulkanRendererPreparationSummary rendererPreparationSummary;
	{
		// Backend caches must observe the same immutable root that Vulkan later
		// consumes. Preparation is therefore deferred until sealing below instead
		// of receiving the worker-only render collection directly.
	}
	const auto backendPreparationEnd = std::chrono::steady_clock::now();
	// Diagnostics begin from publication-neutral frame facts. Packet/submission
	// summaries belonged to the superseded intermediate plan and could disagree with
	// the sealed ranges that Vulkan actually consumes.
	TerraEngine::Renderer::RenderDiagnosticsSummary renderGraphDiagnostics{};
	renderGraphDiagnostics.frameId = activeRenderFrameId;
	renderGraphDiagnostics.valid = true;
	renderGraphDiagnostics.detailed = TerraEngine::Renderer::RenderDiagnosticsRequested();
	if(!backendCapabilities.supportsParallelRecording) {
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Backend profile requires serial render recording.");
	}
	renderGraphDiagnostics.runtimeLayoutRenderRecordCount = cameras.size() + renderables.size() + lights.size();
	renderGraphDiagnostics.runtimeLayoutRejectedRecordCount = runtimeLayoutRejectedRecordCount;
	renderGraphDiagnostics.runtimeLayoutRejectionReasons = runtimeLayoutRejectionReasons;
	if (TerraEngine::Renderer::RenderDiagnosticsRequested() && rhi)
	{
		TerraEngine::Renderer::BackendFrameBreakdown backendBreakdown =
			capturedBackendBreakdown;
		if (backendBreakdown.frameId == 0u)
		{
			backendBreakdown = TerraEngine::Renderer::BuildUnavailableBackendFrameBreakdown(renderFrameSnapshot);
		}
		TerraEngine::Renderer::ApplyBackendBreakdownToRenderDiagnostics(renderGraphDiagnostics, backendBreakdown);
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Vulkan preparation cache hits: "
			+ std::to_string(rendererPreparationSummary.cacheHitCount)
			+ "/"
			+ std::to_string(rendererPreparationSummary.cacheLookupCount));
		if (rendererPreparationSummary.uploadPlan.queuedForUploadWorker)
		{
			renderGraphDiagnostics.backendFallbacks.push_back("Immutable upload packets are queued for backend worker preparation.");
		}
	}
	const TerraEngine::Renderer::RendererRecoverySnapshot recoverySnapshot =
		capturedRecoverySnapshot;
	TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(renderGraphDiagnostics, recoverySnapshot);
	if (!TerraEngine::Renderer::CanSubmitNormalGpuWork(recoverySnapshot))
	{
		ReportFatalRendererRecoveryFailure(recoverySnapshot);
	}
	// Serial validation uses the same immutable inputs and protected boundaries,
	// with one preparation lane so comparisons can detect concurrency defects.
	const size_t renderGraphWorkerCount = renderFrameSnapshot.efficiencyOptions.options.validationPreparation
		== TerraEngine::Renderer::RendererEfficiencyPreparation::Serial ? 1U
		: std::max<size_t>(1u, backendCapabilities.maxRecordingWorkers);
	VisibleCommandCacheState& visibleCommandCache = lifetimeState->visibleCommandCache;
	ShadowDrawListCacheState& shadowDrawListCache = lifetimeState->shadowDrawListCache;
	visibleCommandCache.hitCount = 0u;
	visibleCommandCache.missCount = 0u;
	shadowDrawListCache.hitCount = 0u;
	shadowDrawListCache.missCount = 0u;
	const std::uint64_t renderPreparationSettingsSignature = BuildRenderSettingsSignature(renderFrameSnapshot.renderSettings, currentPreset, taskShaderCullingEnabled);

	// Conservative deformable bounds cover the full animation envelope, so pose
	// publications do not invalidate spatial records. Rebuild only when object
	// spatial state, renderable identity/order, or a unique mesh bound changes.
	SpatialPreparationCacheState& spatialPreparationCache =
		lifetimeState->spatialPreparationCache;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::SpatialAABB>
		localBoundsByMesh;
	const std::uint64_t spatialSceneGeneration = capturedSceneGeneration;
	const std::uint64_t spatialRevision = capturedSpatialRevision;
	const std::uint64_t meshComponentRevision = capturedRenderBindingRevision;
	// This token changes after every successful renderer-resource reload. A mesh
	// replacement can alter bounds without mutating the MeshRenderComponent, so it
	// must invalidate the spatial observation before cached bounds are reused.
	const std::uint64_t meshResourceRevision =
		renderFrameSnapshot.resourceRevision;
	const bool spatialObservationUnchanged = spatialPreparationCache.valid
		&& spatialPreparationCache.observedSceneGeneration == spatialSceneGeneration
		&& spatialPreparationCache.observedSpatialRevision == spatialRevision
		&& spatialPreparationCache.observedMeshComponentRevision
			== meshComponentRevision
		&& spatialPreparationCache.observedMeshResourceRevision
			== meshResourceRevision
		&& spatialPreparationCache.observedRenderableCount == renderables.size();
	std::uint64_t spatialPreparationSignature = spatialPreparationCache.signature;
	if(!spatialObservationUnchanged) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Spatial Cache Signature",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		localBoundsByMesh.reserve(expectedUniqueMeshCount);
		spatialPreparationSignature = kRenderHashOffset;
		HashCombine(spatialPreparationSignature, spatialSceneGeneration);
		HashCombine(spatialPreparationSignature, spatialRevision);
		HashCombine(spatialPreparationSignature,
			static_cast<std::uint64_t>(renderables.size()));
		for(const RenderableEntry& entry : renderables)
		{
			HashCombine(spatialPreparationSignature,
				static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(entry.objectId)));
			HashCombine(spatialPreparationSignature,
				static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(entry.mesh->meshId)));
			HashCombine(spatialPreparationSignature, entry.isStatic ? 1U : 0U);
			if(localBoundsByMesh.contains(entry.mesh->meshId)) continue;

			const auto capturedBounds = frameMeshLocalBounds.find(entry.mesh->meshId);
			const TerraEngine::Scene::SpatialAABB localBounds =
				capturedBounds != frameMeshLocalBounds.end()
					? capturedBounds->second : TerraEngine::Scene::SpatialAABB{};
			localBoundsByMesh.emplace(entry.mesh->meshId, localBounds);
			const auto hashFloat = [](float value) {
				return static_cast<std::uint64_t>(std::hash<float>{}(value));
			};
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.min.x));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.min.y));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.min.z));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.max.x));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.max.y));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.max.z));
			HashCombine(spatialPreparationSignature,
				static_cast<std::uint64_t>(localBounds.state));
			HashCombine(spatialPreparationSignature,
				static_cast<std::uint64_t>(localBounds.source));
			if(const auto mesh = frameMeshResources.find(entry.mesh->meshId);
				mesh != frameMeshResources.end() && mesh->second != nullptr) {
				HashCombine(spatialPreparationSignature,
					mesh->second->deformablePayloadGeneration);
				HashCombine(spatialPreparationSignature,
					mesh->second->runtimeLayout.version);
			}
		}
	}
	bool spatialIndexRebuilt = false;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Refresh Spatial Index", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		TerraEngine::Scene::SpatialReferenceFrame spatialFrame{};
		spatialFrame.generation = std::max<std::uint64_t>(1U, capturedSpatialRevision);
		if (!renderables.empty())
		{
			spatialFrame.canonicalOrigin = getWorldTransform(renderables.front().entity).position;
		}
		const TerraEngine::Scene::SpatialReferenceFrame previousSpatialFrame =
			spatialIndex.GetReferenceFrame();
		const bool spatialFrameChanged =
			previousSpatialFrame.generation != spatialFrame.generation
			|| previousSpatialFrame.canonicalOrigin != spatialFrame.canonicalOrigin;
		const bool spatialFrameAccepted = spatialIndex.SetReferenceFrame(spatialFrame);
		if(!spatialPreparationCache.valid
			|| spatialPreparationCache.signature != spatialPreparationSignature
			|| spatialFrameChanged || !spatialFrameAccepted)
		{
			spatialPreparationCache.records.clear();
			spatialPreparationCache.renderableLookup.clear();
			spatialPreparationCache.conservativeAnimationBounds.assign(
				renderables.size(), false);
			spatialPreparationCache.records.reserve(renderables.size());
			spatialPreparationCache.renderableLookup.reserve(renderables.size());
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (std::size_t index = 0; index < renderables.size(); ++index)
			{
				const RenderableEntry& entry = renderables[index];
				spatialPreparationCache.renderableLookup.emplace(entry.entity, index);
				const TerraEngine::Scene::WorldPose& transform = getWorldTransform(entry.entity);
				const glm::mat4 model = makeModelMatrix(transform, spatialFrame.canonicalOrigin);
				const auto cachedBounds = localBoundsByMesh.find(entry.mesh->meshId);
				const TerraEngine::Scene::SpatialAABB localBounds =
					cachedBounds != localBoundsByMesh.end()
						? cachedBounds->second : TerraEngine::Scene::SpatialAABB{};
				const auto cachedSpatialMesh = frameMeshResources.find(entry.mesh->meshId);
				const Mesh* spatialMesh = cachedSpatialMesh != frameMeshResources.end()
					? cachedSpatialMesh->second : nullptr;
				// CacheLoadedMeshBounds unions validated TEMF deformation bounds with
				// bind-pose bounds. That whole-animation envelope makes ordinary spatial
				// rejection conservative even while the exact page bounds are produced
				// on the GPU later in the frame.
				spatialPreparationCache.conservativeAnimationBounds[index] = spatialMesh != nullptr
					&& spatialMesh->hasDeformablePayload && localBounds.IsUsable();
				const TerraEngine::Scene::WorldLocalizationResult localizedOwner =
					TerraEngine::Scene::LocalizeWorldPosition(
						transform.position,
						spatialFrame.canonicalOrigin,
						spatialFrame.maximumLocalDistanceMetres);
				TerraEngine::Scene::SpatialAABB localizedBounds = localBounds.IsUsable()
					? TerraEngine::Scene::TransformAABB(localBounds, model)
					: (localizedOwner.Succeeded()
						? TerraEngine::Scene::MakeFallbackBounds(
							glm::vec3(localizedOwner.offsetMetres), 1.0f)
						: TerraEngine::Scene::MakeInvalidBounds(
							TerraEngine::Scene::SpatialBoundsSource::Fallback));
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!localizedBounds.IsUsable() && localizedOwner.Succeeded())
				{
					localizedBounds = TerraEngine::Scene::MakeFallbackBounds(
						glm::vec3(localizedOwner.offsetMetres), 1.0f);
				}

				TerraEngine::Scene::SpatialObjectRecord record{};
				record.objectId = entry.objectId;
				record.entity = entry.entity;
				record.active = true;
				record.isStatic = entry.isStatic;
				record.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable | TerraEngine::Scene::SpatialCategoryEditorSelectable;
				record.canonicalOwner = transform.position;
				record.localBounds = localizedBounds;
				record.frameGeneration = spatialFrame.generation;
				record.revision = capturedSpatialRevision;
				spatialPreparationCache.records.push_back(record);
			}
			spatialIndex.Refresh(spatialPreparationCache.records);
			spatialPreparationCache.signature = spatialPreparationSignature;
			spatialPreparationCache.valid = true;
			spatialIndexRebuilt = true;
		}
	}
	spatialPreparationCache.observedSceneGeneration = spatialSceneGeneration;
	spatialPreparationCache.observedSpatialRevision = spatialRevision;
	spatialPreparationCache.observedMeshComponentRevision = meshComponentRevision;
	spatialPreparationCache.observedMeshResourceRevision = meshResourceRevision;
	spatialPreparationCache.observedRenderableCount = renderables.size();
	const std::vector<TerraEngine::Scene::SpatialObjectRecord>& spatialRecords =
		spatialPreparationCache.records;
	const std::unordered_map<entt::entity, std::size_t>& renderableLookup =
		spatialPreparationCache.renderableLookup;
	const std::vector<bool>& conservativeAnimationBounds =
		spatialPreparationCache.conservativeAnimationBounds;
	TerraEngine::Scene::SpatialDiagnosticsSnapshot latestSpatialRefreshDiagnostics = spatialIndex.GetDiagnosticsSnapshot();
	latestSpatialRefreshDiagnostics.transformDirtyRootCount = transformPropagation->dirtyRootCount;
	latestSpatialRefreshDiagnostics.transformExpandedObjectCount = transformPropagation->expandedDirtyObjectCount;
	latestSpatialRefreshDiagnostics.transformProcessedObjectCount = transformPropagation->processedObjectCount;
	latestSpatialRefreshDiagnostics.transformSkippedCleanObjectCount = transformPropagation->skippedCleanObjectCount;
	latestSpatialRefreshDiagnostics.transformHierarchyLevelCount = transformPropagation->hierarchyLevelCount;
	latestSpatialRefreshDiagnostics.boundsRecomputedObjectCount = spatialIndexRebuilt
		? static_cast<std::uint32_t>(spatialRecords.size()) : 0U;

	RenderableDrawWorkingSet& renderableDrawWorkingSet =
		lifetimeState->renderableDrawWorkingSet;
	// resize preserves the allocated backing storage. Every active element is
	// overwritten below, and base commands never own a pose publication.
	renderableDrawWorkingSet.commands.resize(renderables.size());
	renderableDrawWorkingSet.signatures.resize(renderables.size());
	std::vector<RenderItemRecord>& renderableDrawCommands =
		renderableDrawWorkingSet.commands;
	std::vector<std::uint64_t>& drawCommandSignatures =
		renderableDrawWorkingSet.signatures;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!renderables.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		DrawCommandCacheState& drawCommandCache = lifetimeState->drawCommandCache;
		drawCommandCache.hitCount = 0u;
		drawCommandCache.missCount = 0u;
		const std::uint64_t drawSceneGeneration = capturedSceneGeneration;
		const std::uint64_t drawActiveMembershipRevision =
			capturedActiveMembershipRevision;
		const std::uint64_t drawComponentRevision =
			capturedComponentMutationRevision;
		const std::uint64_t drawHierarchyRevision = capturedHierarchyRevision;
		const std::uint64_t drawSpatialRevision = capturedSpatialRevision;
		const std::uint64_t drawMeshComponentRevision =
			capturedRenderBindingRevision;
		const bool reuseOrderedBaseCommands = drawCommandCache.baseInputsValid
			&& drawCommandCache.observedSceneGeneration == drawSceneGeneration
			&& drawCommandCache.observedActiveMembershipRevision
				== drawActiveMembershipRevision
			&& drawCommandCache.observedSpatialRevision == drawSpatialRevision
			&& drawCommandCache.observedMeshComponentRevision
				== drawMeshComponentRevision
			&& drawCommandCache.recordsByObjectId.size() == renderables.size();

		if(!reuseOrderedBaseCommands) {
			// Workers compute cheap signatures from immutable reflected inputs.
			// The cache map itself is updated later on the coordinator thread so
			// frame-to-frame reuse does not add a lock to every renderable.
			RunRenderChunkedWork(
				renderables.size(),
				renderGraphWorkerCount,
				kRenderParallelThreshold,
				"RenderSystem Task: Hash Draw Command Chunk",
				[&](std::size_t start, std::size_t end)
				{
					for (std::size_t index = start; index < end; ++index)
					{
						const RenderableEntry& entry = renderables[index];
						const MeshRenderComponent& meshComp = *entry.mesh;
						drawCommandSignatures[index] = BuildDrawCommandSignature(
							meshComp.meshId,
							meshComp.materialId,
							getWorldTransform(entry.entity),
							meshComp.taskShaderCullingEnabled);
					}
				});
			for (std::size_t index = 0u; index < renderables.size(); ++index)
			{
				const RenderableEntry& entry = renderables[index];
				const MeshRenderComponent& meshComp = *entry.mesh;
				const std::uint64_t signature = drawCommandSignatures[index];
				auto cacheEntry = drawCommandCache.recordsByObjectId.find(entry.objectId);
				if (cacheEntry != drawCommandCache.recordsByObjectId.end() && cacheEntry->second.signature == signature)
				{
					cacheEntry->second.lastTouchedFrameId = activeRenderFrameId;
					renderableDrawCommands[index] = cacheEntry->second.command;
					// Geometry routing is recalibrated from the current immutable
					// TEMF/runtime facts each frame, so it must not remain cached.
					renderableDrawCommands[index].geometryRoute =
						renderFrameSnapshot.renderables[index].geometryRouteDecision.selected;
					// The visible-command cache later republishes this base value without
					// another scene lookup. Preserve its canonical one-based ordinal so a
					// static cache hit can still build a sealed selector for this draw.
					renderableDrawCommands[index].conventionalVisibilityIdentityIndex =
						renderFrameSnapshot.renderables[index].conventionalVisibilityIdentityIndex;
					// Pose publication changes are intentionally absent from the base draw
					// signature. The binding is attached only to a visible frame command.
					renderableDrawCommands[index].animationBinding = {};
					++drawCommandCache.hitCount;
					continue;
				}

				RenderItemRecord command{};
				command.meshId = meshComp.meshId;
				command.materialId = meshComp.materialId;
				command.objectId = entry.objectId;
				command.geometryRoute =
					renderFrameSnapshot.renderables[index].geometryRouteDecision.selected;
				// Store the stable identity with the reusable base command. The cached
				// visible path needs this value to address immutable publication pages.
				command.conventionalVisibilityIdentityIndex =
					renderFrameSnapshot.renderables[index].conventionalVisibilityIdentityIndex;
				command.taskShaderCullingEnabled = meshComp.taskShaderCullingEnabled;
				const TerraEngine::Scene::WorldPose& canonicalPose =
					getWorldTransform(entry.entity);
				command.canonicalWorldPose = canonicalPose;
				command.modelMatrix = makeModelMatrix(canonicalPose, canonicalPose.position);
				renderableDrawCommands[index] = command;

				CachedDrawCommandRecord record{};
				record.signature = signature;
				record.lastTouchedFrameId = activeRenderFrameId;
				record.command = command;
				// The per-object compatible-pose cache owns the one retained prior pose.
				// Base draw caching must not add another hidden publication owner.
				drawCommandCache.recordsByObjectId[entry.objectId] = std::move(record);
				++drawCommandCache.missCount;
			}
		} else {
			// Exact protected revisions prove the cached mesh/material/model inputs and
			// deterministic renderable order are unchanged. Only route selection is
			// frame-dependent, so update that compact field without hashing transforms
			// or probing the UUID map again.
			for(std::size_t index = 0U; index < renderables.size(); ++index) {
				renderableDrawCommands[index].geometryRoute =
					renderFrameSnapshot.renderables[index]
						.geometryRouteDecision.selected;
				// Static reuse still republishes each command through compact selectors;
				// restore the snapshot's canonical ordinal before a visible cache hit
				// materializes these retained base commands.
				renderableDrawCommands[index].conventionalVisibilityIdentityIndex =
					renderFrameSnapshot.renderables[index]
						.conventionalVisibilityIdentityIndex;
			}
			drawCommandCache.hitCount = renderables.size();
		}
		const bool drawMembershipChanged =
			drawCommandCache.observedSceneGeneration != drawSceneGeneration
			|| drawCommandCache.observedActiveMembershipRevision
				!= drawActiveMembershipRevision
			|| drawCommandCache.observedComponentRevision != drawComponentRevision
			|| drawCommandCache.observedHierarchyRevision != drawHierarchyRevision
			|| drawCommandCache.recordsByObjectId.size() != renderables.size();
		if(drawMembershipChanged) {
			std::unordered_set<TerraEngine::UUID> aliveDrawCommandObjects;
			aliveDrawCommandObjects.reserve(renderables.size());
			for(const RenderableEntry& entry : renderables) {
				aliveDrawCommandObjects.insert(entry.objectId);
			}
			PruneStaleDrawCommandCache(drawCommandCache, aliveDrawCommandObjects);
			PruneStaleVirtualGeometryTransitionCache(
				virtualGeometryTransitionCache, aliveDrawCommandObjects);
			drawCommandCache.observedSceneGeneration = drawSceneGeneration;
			drawCommandCache.observedActiveMembershipRevision =
				drawActiveMembershipRevision;
			drawCommandCache.observedComponentRevision = drawComponentRevision;
			drawCommandCache.observedHierarchyRevision = drawHierarchyRevision;
		}
		drawCommandCache.observedSceneGeneration = drawSceneGeneration;
		drawCommandCache.observedActiveMembershipRevision =
			drawActiveMembershipRevision;
		drawCommandCache.observedSpatialRevision = drawSpatialRevision;
		drawCommandCache.observedMeshComponentRevision =
			drawMeshComponentRevision;
		drawCommandCache.baseInputsValid = true;
		if (TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			renderGraphDiagnostics.backendFallbacks.push_back(
				"Draw command cache hits: "
				+ std::to_string(drawCommandCache.hitCount)
				+ "/"
				+ std::to_string(drawCommandCache.hitCount + drawCommandCache.missCount));
		}
	}
	// Base draw caches intentionally ignore animation presentation serials. Attach
	// the current immutable semantic owner after every hit/miss path so later
	// candidate and legacy shadow materialization cannot retain an old CPU pose.
	for (std::size_t index = 0; index < renderableDrawCommands.size()
		&& index < renderFrameSnapshot.renderables.size(); ++index)
	{
		auto& command = renderableDrawCommands[index];
		command.shadowCasterInputs = renderFrameSnapshot.renderables[index].shadowCasterInputs;
		command.objectLifetimeGeneration = command.shadowCasterInputs != nullptr
			? command.shadowCasterInputs->lifetimeGeneration : 0U;
	}
	// Visibility and LOD selection depend on stable geometry inputs, not the
	// animation publication produced for the same object this frame. Keep a
	// second submission signature for caches that still retain complete draw
	// commands; the visible cache refreshes animation bindings explicitly below.
	RenderableFrameIdentityCacheState& renderableFrameIdentityCache =
		lifetimeState->renderableFrameIdentityCache;
	bool frameIdentityMatches = renderableFrameIdentityCache.valid
		&& renderableFrameIdentityCache.sceneGeneration
			== capturedSceneGeneration
		&& renderableFrameIdentityCache.objectIds.size() == renderables.size();
	for(std::size_t index = 0U; frameIdentityMatches && index < renderables.size(); ++index) {
		frameIdentityMatches = renderableFrameIdentityCache.objectIds[index]
			== renderables[index].objectId;
	}
	if(!frameIdentityMatches) {
		renderableFrameIdentityCache.objectIds.clear();
		renderableFrameIdentityCache.indicesByObjectId.clear();
		renderableFrameIdentityCache.objectIds.reserve(renderables.size());
		renderableFrameIdentityCache.indicesByObjectId.reserve(renderables.size());
		for(std::size_t index = 0U; index < renderables.size(); ++index) {
			renderableFrameIdentityCache.objectIds.push_back(renderables[index].objectId);
			renderableFrameIdentityCache.indicesByObjectId.emplace(
				renderables[index].objectId, index);
		}
		renderableFrameIdentityCache.drawCommandSignatures.clear();
		renderableFrameIdentityCache.sceneGeneration = capturedSceneGeneration;
		renderableFrameIdentityCache.valid = true;
	}
	std::uint64_t renderableWorldSignature =
		renderableFrameIdentityCache.worldSignature;
	if(renderableFrameIdentityCache.drawCommandSignatures != drawCommandSignatures) {
		renderableWorldSignature = kRenderHashOffset;
		HashCombine(renderableWorldSignature,
			static_cast<std::uint64_t>(drawCommandSignatures.size()));
		for(std::size_t index = 0U; index < drawCommandSignatures.size(); ++index) {
			HashCombine(renderableWorldSignature,
				static_cast<std::uint64_t>(
					std::hash<TerraEngine::UUID>{}(renderables[index].objectId)));
			HashCombine(renderableWorldSignature, drawCommandSignatures[index]);
		}
		renderableFrameIdentityCache.drawCommandSignatures = drawCommandSignatures;
		renderableFrameIdentityCache.worldSignature = renderableWorldSignature;
	}
	std::uint64_t renderableSubmissionSignature = renderableWorldSignature;
	std::uint64_t renderableVisibilitySignature = renderableWorldSignature;
	// A pose publication normally changes only dynamic deformation data. Hash the
	// bounded admission decisions instead of the per-frame authority generation so
	// stable animated crowds reuse their expensive frustum and VG selection work.
	// Readiness, page-fallback, or conservative-bounds transitions still change the
	// key immediately and therefore cannot retain an empty pre-ready selection.
	for(std::size_t index = 0U; index < renderables.size(); ++index) {
		const Mesh* mesh = renderables[index].runtimeMesh;
		const bool usesVirtualGeometry = mesh != nullptr
			&& mesh->hasVirtualGeometry;
		const TerraEngine::Renderer::AnimationRenderBinding& binding =
			renderFrameSnapshot.renderables[index].animationBinding;
		const bool requiresUnboundedCoarseVisibility =
			binding.RequiresUnboundedCoarseVisibility(
				conservativeAnimationBounds[index]);
		std::uint64_t admissionFlags = 0U;
		admissionFlags |= requiresUnboundedCoarseVisibility ? 1U : 0U;
		admissionFlags |= binding.RequiresVirtualGeometryOwnerFallbackVisibility(
			usesVirtualGeometry) ? 2U : 0U;
		admissionFlags |= binding.IsComputeRasterDeformationSafe() ? 4U : 0U;
		HashCombine(renderableVisibilitySignature, admissionFlags);
		// An asset without a validated whole-animation envelope deliberately
		// bypasses CPU broadphase culling. Its exact deformed bounds can change
		// which virtual-geometry clusters are selected, so retain the pose token
		// only for that correctness fallback. Cooked envelopes keep this token out
		// of the view key and therefore reuse their stable per-view evidence.
		if(requiresUnboundedCoarseVisibility) {
			HashCombine(renderableVisibilitySignature,
				binding.posePublicationGeneration);
		}
	}
	// Shadow draw lists retain complete bindings, so any authority publication,
	// authored Animator edit, hierarchy rebind, or animation/deformable resource
	// publication must invalidate them. These monotonic protected tokens cover the
	// same changes without rehashing every binding field for every crowd member.
	// The presentation root is the sole animation publication acquired for this
	// frame. Its authority generation invalidates retained selections without a
	// second load from AnimationSystem that could race presentation publication.
	HashCombine(renderableSubmissionSignature,
		animationPresentationFrame != nullptr && animationPresentationFrame->IsValid()
			? animationPresentationFrame->authority->authorityGeneration : 0U);
	HashCombine(renderableSubmissionSignature, capturedAnimatorComponentRevision);
	HashCombine(renderableSubmissionSignature, capturedHierarchyRevision);
	HashCombine(renderableSubmissionSignature,
		capturedAnimationBindingResourceRevision);
	// Mesh, material, and texture reloads do not alter animation bindings. Include
	// their independent publication generation so retained visible/shadow command
	// evidence cannot outlive the payload that supplied its selectors.
	HashCombine(renderableSubmissionSignature, renderFrameSnapshot.resourceRevision);
	// A producer-sealed GPU topology always advances through its separate root.
	// CPU deformation remains a sparse companion plan and refreshes only the pages
	// that own those exceptional lanes; it must not disable dense publication for
	// the thousands of GPU-resident owners in the same frame.
	const bool hasDenseAnimationPublication =
		hasDenseAnimationTopologyCandidate;
	const std::unordered_map<TerraEngine::UUID, std::size_t>&
		renderableCommandIndices =
			renderableFrameIdentityCache.indicesByObjectId;

	auto collectCandidateIndices = [&](const TerraEngine::Scene::SpatialQueryResult& query) {
		std::vector<std::size_t> candidateIndices;
		candidateIndices.reserve(std::min<std::size_t>(
			renderables.size(),
			query.hits.size() + renderables.size()));
		std::vector<bool> spatiallyVisible(renderables.size(), false);
		// Convert the spatial result to a stable renderable-order mask. Properly
		// cooked deformable meshes already publish a whole-animation envelope;
		// only legacy/unproven animated bounds need the fail-open append below.
		for (const TerraEngine::Scene::SpatialQueryHit& hit : query.hits)
		{
			if (const auto lookup = renderableLookup.find(hit.entity); lookup != renderableLookup.end())
			{
				spatiallyVisible[lookup->second] = true;
			}
		}
		for (std::size_t index = 0u; index < renderables.size(); ++index)
		{
			const Mesh* mesh = renderables[index].runtimeMesh;
			const bool ownerFallbackNeedsVisibility =
				renderFrameSnapshot.renderables[index].animationBinding
					.RequiresVirtualGeometryOwnerFallbackVisibility(
						mesh != nullptr && mesh->hasVirtualGeometry);
			if (spatiallyVisible[index]
				|| renderFrameSnapshot.renderables[index].animationBinding
					.RequiresUnboundedCoarseVisibility(
						conservativeAnimationBounds[index])
				|| ownerFallbackNeedsVisibility)
			{
				// The later virtual-geometry selector expands this exact case into a
				// complete owner draw. Appending it here is deliberately narrow: healthy
				// page-local animation still benefits from ordinary CPU broadphase culling.
				candidateIndices.push_back(index);
			}
		}
		return candidateIndices;
	};

	auto buildDrawCommandsFromCandidateIndices = [&](const std::vector<std::size_t>& candidateIndices) {
		std::vector<RenderItemRecord> commands;
		commands.reserve(candidateIndices.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const std::size_t candidateIndex : candidateIndices)
		{
			commands.push_back(renderableDrawCommands[candidateIndex]);
			commands.back().animation = BuildPublishedAnimationGeneration(
				renderFrameSnapshot.renderables[candidateIndex].animation);
			commands.back().animationBinding =
					TerraEngine::Renderer::PublishedAnimationRenderBinding(
						renderFrameSnapshot.renderables[candidateIndex].animationBinding);
			const auto identity = conventionalVisibilityIdentityIndices.find(
				commands.back().objectId);
			commands.back().conventionalVisibilityIdentityIndex =
				identity != conventionalVisibilityIdentityIndices.end()
					? identity->second : 0u;
		}
		return commands;
	};

	std::vector<VirtualGeometryAssetRuntime> gameVirtualGeometryDiagnosticsAssets;
	std::vector<VirtualGeometryAssetRuntime> sceneVirtualGeometryDiagnosticsAssets;
	std::vector<VirtualGeometryAssetRuntime> shadowVirtualGeometryDiagnosticsAssets;
	std::unordered_set<std::uint64_t> activeVirtualGeometryViewPolicyKeys;
	std::unordered_set<std::uint64_t> activeVirtualGeometryTransitionKeys;
	std::unordered_set<std::uint64_t> activeVirtualGeometryDebugModeKeys;
	auto collectVirtualGeometryDiagnosticsAssets = [&](const std::vector<RenderItemRecord>& commands, RenderViewType viewType) {
		std::vector<VirtualGeometryAssetRuntime>& target = viewType == RenderViewType::Scene
			? sceneVirtualGeometryDiagnosticsAssets
			: (viewType == RenderViewType::Shadow ? shadowVirtualGeometryDiagnosticsAssets : gameVirtualGeometryDiagnosticsAssets);
		std::unordered_set<TerraEngine::UUID> seenMeshes;
		for (const RenderItemRecord& command : commands)
		{
			if (seenMeshes.contains(command.meshId))
			{
				continue;
			}
			const auto capturedMesh = frameMeshResources.find(command.meshId);
			if (const Mesh* mesh = capturedMesh != frameMeshResources.end()
					? capturedMesh->second : nullptr;
				mesh != nullptr && mesh->hasVirtualGeometry)
			{
				target.push_back(getVirtualGeometryRuntime(command.meshId, *mesh));
				seenMeshes.insert(command.meshId);
			}
		}
	};

	auto recordVirtualGeometryViewSelection = [&](
		const std::vector<RenderItemRecord>& commands,
		const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters,
		const std::vector<TerraEngine::Renderer::VirtualGeometryPageNeedRecord>& pageNeeds,
		const std::vector<VirtualGeometryClusterPacket>& clusterPackets,
		const std::vector<ClusterWorkBatch>& clusterBatches,
		const std::vector<CompactedVisibilitySet>& compactionResults,
		const std::vector<FallbackCoverageChain>& fallbackCoverage,
		const std::vector<VirtualGeometryLODTransitionState>& lodTransitionStates,
		VirtualGeometryDebugVisualizationMode debugVisualizationMode,
		RenderOutputKind outputKind,
		RenderViewType viewType,
		const glm::uvec2& targetSize,
		std::uint32_t outputId,
		bool shadowPass,
		bool exportRuntime,
		const std::vector<std::uint32_t>* virtualGeometryCommandIndices = nullptr,
		bool reuseValidatedSelection = false,
		const VirtualGeometryViewSelectionPacket* preparedSelection = nullptr,
		const VisibleCommandCacheRecord* cachedSelectionEvidence = nullptr) {
		TerraEngine::Renderer::VirtualGeometryViewSelectionState state;
		state.outputKind = outputKind;
		state.viewType = viewType;
		state.targetSize = targetSize;
		state.outputId = outputId;
		state.debugVisualizationMode = NormalizeVirtualGeometryDebugVisualizationMode(debugVisualizationMode);
		state.shadowPass = shadowPass;
		state.exportRuntime = exportRuntime;
		state.viewGeneration = activeRenderFrameId;
		const auto makeViewKind = [shadowPass, outputKind]() {
			if(shadowPass) return TerraEngine::Renderer::VirtualGeometryViewKind::Shadow;
			if(outputKind == RenderOutputKind::SceneView) return TerraEngine::Renderer::VirtualGeometryViewKind::Editor;
			if(outputKind == RenderOutputKind::OffscreenTexture) return TerraEngine::Renderer::VirtualGeometryViewKind::Capture;
			return TerraEngine::Renderer::VirtualGeometryViewKind::Camera;
		};
		const std::uint64_t policyKey = (static_cast<std::uint64_t>(
			static_cast<std::uint32_t>(viewType)) << 32u) | outputId;
		activeVirtualGeometryViewPolicyKeys.insert(policyKey);
		activeVirtualGeometryDebugModeKeys.insert(
			BuildVirtualGeometryDebugModeKey(viewType, outputId));
		TerraEngine::Renderer::VirtualGeometryViewPolicyState policyState{};
		policyState.kind = makeViewKind();
		policyState.viewId = policyKey;
		policyState.targetGeneration = (static_cast<std::uint64_t>(targetSize.x) << 32u)
			| targetSize.y;
		policyState.projectionGeneration = policyState.targetGeneration;
		// Geometry selection history must not span a reloaded mesh payload even
		// when the renderer session itself remains alive.
		policyState.geometryGeneration = renderFrameSnapshot.resourceRevision;
		policyState.residencyGeneration =
			capturedVirtualGeometryResidencyGeneration;
		if(const auto previous = virtualGeometryViewPolicies.find(policyKey);
			previous != virtualGeometryViewPolicies.end())
		{
			policyState.resized = previous->second.targetGeneration != policyState.targetGeneration;
		}
		TerraEngine::Renderer::VirtualGeometryViewPolicyState historyOwner = policyState;
		if(policyState.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Shadow
			|| policyState.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Capture)
		{
			const auto compatibleOwner = std::find_if(
				virtualGeometryViewPolicies.begin(), virtualGeometryViewPolicies.end(),
				[&policyState](const auto &entry) {
					return (entry.second.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Camera
						|| entry.second.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Editor)
						&& entry.second.geometryGeneration == policyState.geometryGeneration
						&& entry.second.residencyGeneration == policyState.residencyGeneration;
				});
			if(compatibleOwner != virtualGeometryViewPolicies.end())
			{
				historyOwner = compatibleOwner->second;
			}
		}
		policyState.historyOwnerViewId = policyState.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Camera
			|| policyState.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Editor
			? policyState.viewId : historyOwner.viewId;
		const TerraEngine::Renderer::VirtualGeometryViewPolicyDecision viewPolicy =
			TerraEngine::Renderer::EvaluateVirtualGeometryViewPolicy(policyState, historyOwner);
		virtualGeometryViewPolicies[policyKey] = policyState;
		// Geometry/residency reuse is allowed only as immutable inputs. Capture and
		// shadow views deliberately retain a main-pass/fail-open visibility policy
		// until they own a compatible history themselves.
		state.hiZOcclusionAllowed = viewPolicy.historyValid;
		state.hiZInactiveReason.clear();
		if(!viewPolicy.historyValid && !shadowPass)
		{
			state.hiZInactiveReason = "View-owned virtual-geometry history is not reusable; visibility remains conservative.";
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Copy VG Selection Inputs",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(cachedSelectionEvidence != nullptr
				&& cachedSelectionEvidence->retainedSelectionEvidence)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reference Cached VG Selection Evidence",
					TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				state.retainedSelectionEvidence =
					cachedSelectionEvidence->retainedSelectionEvidence;
			}
			else
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Copy Unretained VG Selection Evidence",
					TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				state.selectedClusters = selectedClusters;
				state.pageNeeds = pageNeeds;
				state.clusterPackets = clusterPackets;
				state.clusterBatches = clusterBatches;
				state.compactionResults = compactionResults;
				state.fallbackCoverage = fallbackCoverage;
				state.lodTransitionStates = lodTransitionStates;
			}
			if(preparedSelection != nullptr)
			{
				// Fresh selection already built and validated these current-frame
				// records. The view snapshot needs its own immutable copies, but must not
				// rebuild and revalidate the same packet graph a second time.
				state.gpuPacketRecords = preparedSelection->gpuPacketRecords;
				state.gpuBatchDescriptors = preparedSelection->gpuBatchDescriptors;
				state.gpuPacketFrameSets = preparedSelection->gpuPacketFrameSets;
				state.gpuCompactionDispatches = preparedSelection->gpuCompactionDispatches;
				state.generatedSubmissions = preparedSelection->generatedSubmissions;
				state.gpuCounterReadbacks = preparedSelection->gpuCounterReadbacks;
				state.gpuFallbackRecords = preparedSelection->gpuFallbackRecords;
			}
			else if(cachedSelectionEvidence != nullptr)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reference Cached VG GPU Evidence",
					TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				// The cache signature validates every input that shapes these immutable
				// tables. The enclosing view carries the current publication identity,
				// so retaining the table owner avoids two diagnostic-only deep copies.
				state.retainedGpuEvidence = cachedSelectionEvidence->retainedGpuEvidence;
			}
		}
		const auto appendVirtualGeometryDraw = [&](const RenderItemRecord& command) {
			TerraEngine::Renderer::VirtualGeometryDrawRange range{};
			range.selectedLodIndex = command.selectedVirtualLod;
			range.meshletOffset = command.virtualMeshletOffset;
			range.meshletCount = command.virtualMeshletCount;
			range.virtualGeometryActive = (command.virtualGeometryFlags & kVirtualGeometryFlagActive) != 0u;
			range.failOpen = (command.virtualGeometryFlags & kVirtualGeometryFlagFailOpen) != 0u;
			range.hiZSelfOcclusionGuardPassed = true;
			range.hiZNearPlaneGuardPassed = true;
			range.hiZThinGeometryGuardPassed = true;
			range.selectedClusterOffset = command.selectedVirtualClusterOffset;
			range.selectedClusterCount = command.selectedVirtualClusterCount;
			range.pageNeedOffset = command.selectedVirtualPageNeedOffset;
			range.pageNeedCount = command.selectedVirtualPageNeedCount;
			range.pageId = command.virtualGeometryPageId;
			range.fallbackPageId = command.virtualGeometryFallbackPageId;
			range.payloadSource = command.virtualGeometryPayloadSource;
			range.emittedSource = command.virtualGeometryEmittedSource;
			range.pagePayloadByteOffset = command.virtualGeometryPagePayloadByteOffset;
			range.pagePayloadByteSize = command.virtualGeometryPagePayloadByteSize;
			range.payloadBufferIndex = command.virtualGeometryPayloadBufferIndex;
			range.payloadDescriptorIndex = command.virtualGeometryPayloadDescriptorIndex;
			state.hiZSelfOcclusionGuardPassed = state.hiZSelfOcclusionGuardPassed && range.hiZSelfOcclusionGuardPassed;
			state.hiZNearPlaneGuardPassed = state.hiZNearPlaneGuardPassed && range.hiZNearPlaneGuardPassed;
			state.hiZThinGeometryGuardPassed = state.hiZThinGeometryGuardPassed && range.hiZThinGeometryGuardPassed;
			if (state.hiZInactiveReason.empty() && !range.hiZInactiveReason.empty())
			{
				state.hiZInactiveReason = range.hiZInactiveReason;
			}
			if (range.failOpen)
			{
				range.fallbackReason = shadowPass
					? "shadow virtual geometry kept conservative visibility"
					: "view virtual geometry failed open; see fallback diagnostics";
			}
			if (!range.virtualGeometryActive && range.meshletCount == 0u && !range.failOpen)
			{
				return;
			}
			state.draws.push_back({
				command.objectId,
				command.meshId,
				std::move(range),
				command.taskShaderCullingEnabled,
				command.conventionalVisibilityIdentityIndex == 0u
					? 0xffffffffu
					: command.conventionalVisibilityIdentityIndex - 1u
			});
		};
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build VG View Draw Records",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(cachedSelectionEvidence != nullptr
				&& cachedSelectionEvidence->hasViewDrawEvidence
				&& cachedSelectionEvidence->viewDraws)
			{
				state.retainedDraws = cachedSelectionEvidence->viewDraws;
			}
			else
			{
				// A cache hit already owns the exact contributing command indices. Reserve
				// their upper bound once so dense crowds do not repeatedly relocate records
				// while rebuilding the current frame's immutable view publication.
				state.draws.reserve(virtualGeometryCommandIndices != nullptr
					? virtualGeometryCommandIndices->size() : commands.size());
				if(virtualGeometryCommandIndices != nullptr) {
					for(const std::uint32_t commandIndex : *virtualGeometryCommandIndices) {
						if(commandIndex < commands.size()) {
							appendVirtualGeometryDraw(commands[commandIndex]);
						}
					}
				} else {
					for(const RenderItemRecord& command : commands) {
						appendVirtualGeometryDraw(command);
					}
				}
			}
		}
		state.hasHiZValidationState = true;
		state.hiZValidationState = {};
		state.hiZValidationState.renderSize = targetSize;
		state.hiZValidationState.depthSceneSignature = static_cast<std::uint64_t>(selectedClusters.size())
			^ (static_cast<std::uint64_t>(pageNeeds.size()) << 32u)
			^ static_cast<std::uint64_t>(state.DrawRecords().size());
		if (shadowPass || viewType == RenderViewType::Shadow)
		{
			state.hiZValidationState.state = VirtualGeometryHiZState::Unsupported;
			state.hiZValidationState.reason = "Shadow virtual geometry uses conservative visibility and does not sample camera Hi-Z.";
			state.hiZValidationState.failOpen = true;
		}
		else if (targetSize.x == 0u || targetSize.y == 0u)
		{
			state.hiZValidationState.state = VirtualGeometryHiZState::MissingPyramid;
			state.hiZValidationState.reason = "Render target size is unavailable, so no current Hi-Z pyramid can be sampled.";
			state.hiZValidationState.failOpen = true;
		}
		else if (!state.hiZOcclusionAllowed)
		{
			state.hiZValidationState.state = VirtualGeometryHiZState::FailOpen;
			state.hiZValidationState.reason = state.hiZInactiveReason.empty()
				? "Hi-Z occlusion is failing open for this view."
				: state.hiZInactiveReason;
			state.hiZValidationState.failOpen = true;
		}
		else
		{
			state.hiZValidationState.state = VirtualGeometryHiZState::Active;
			state.hiZValidationState.pyramidSize = targetSize;
			state.hiZValidationState.pyramidReady = true;
			state.hiZValidationState.warmupFrames = 1u;
			state.hiZValidationState.failOpen = false;
		}
		if ((shadowPass || viewType == RenderViewType::Shadow)
			&& state.ClusterPackets().empty())
		{
			for (const TerraEngine::Renderer::VirtualGeometryViewDrawRecord& draw :
				state.DrawRecords())
			{
				if (!draw.range.virtualGeometryActive || draw.range.meshletCount == 0u)
				{
					continue;
				}
				VirtualGeometryClusterPacket packet;
				packet.packetId = static_cast<std::uint32_t>(state.clusterPackets.size());
				packet.viewId = outputId;
				packet.viewType = RenderViewType::Shadow;
				packet.objectId = draw.objectId;
				packet.renderableIndex = draw.renderableIndex;
				packet.meshId = draw.meshId;
				packet.selectedLod = draw.range.selectedLodIndex;
				packet.clusterId = draw.range.selectedClusterOffset;
				packet.meshletOffset = draw.range.meshletOffset;
				packet.meshletCount = draw.range.meshletCount;
				packet.fallbackPacket = draw.range.failOpen;
				packet.payloadSource = draw.range.failOpen
					? VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback
					: VirtualGeometryPagePayloadSource::EmbeddedMeshPayload;
				packet.emittedSource = ResolveVirtualGeometryEmittedSource(packet.payloadSource);
				packet.pagePayloadByteSize = static_cast<std::uint64_t>(packet.meshletCount);
				packet.payloadBufferIndex = draw.range.payloadBufferIndex;
				packet.payloadDescriptorIndex = draw.range.payloadDescriptorIndex;
				packet.debugIdentity = packet.packetId ^ 0x5a5a0000u;
				state.clusterPackets.push_back(packet);
			}
			state.clusterBatches = BuildVirtualGeometryClusterBatches(state.clusterPackets);
		}
		if(preparedSelection == nullptr && cachedSelectionEvidence == nullptr) {
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Packet Upload Planning", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			state.gpuPacketRecords = BuildGpuVirtualGeometryPacketRecords(
				state.ClusterPackets(),
				activeRenderFrameId,
				state.debugVisualizationMode);
			state.gpuBatchDescriptors = BuildGpuVirtualGeometryBatchDescriptors(
				state.gpuPacketRecords,
				activeRenderFrameId,
				state.debugVisualizationMode);
		}
		std::string gpuPacketValidationReason;
		bool gpuPacketRecordsValid = true;
		// A visible-command cache hit reuses selection inputs that passed these
		// exact CPU-oracle checks when the cache record was created. The signature
		// includes view, geometry, residency, settings, and renderer generations;
		// current-frame packet builders only restamp the frame identity. Repeating
		// the same linear validation would add work proportional to every visible
		// packet without discovering a new mutable input.
		if(preparedSelection == nullptr && !reuseValidatedSelection) {
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Validate VG Packet Records",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			gpuPacketRecordsValid = ValidateGpuVirtualGeometryPacketRecords(
				state.ClusterPackets(),
				state.SelectedClusters(),
				state.gpuPacketRecords,
				activeRenderFrameId,
				state.debugVisualizationMode,
				&gpuPacketValidationReason);
		}
		if (preparedSelection == nullptr && cachedSelectionEvidence == nullptr
			&& (!state.ClusterPackets().empty() || !state.gpuBatchDescriptors.empty()))
		{
			GpuVirtualGeometryPacketFrameSet frameSet = BuildGpuVirtualGeometryPacketFrameSet(
				state.viewType,
				state.outputId,
				activeRenderFrameId,
				state.targetSize,
				state.debugVisualizationMode,
				0u,
				static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
				0u,
				static_cast<std::uint32_t>(state.gpuBatchDescriptors.size()));
			if (!gpuPacketRecordsValid)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Fallback Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				frameSet.validity = VirtualGeometryGpuFrameSetValidity::FailOpen;
				frameSet.packetCount = 0u;
				frameSet.batchCount = 0u;
				frameSet.compactedPacketCount = 0u;
				frameSet.generatedSubmissionCount = 0u;
				frameSet.fallbackReason = gpuPacketValidationReason;
				state.gpuFallbackRecords.push_back(BuildVirtualGeometryGpuFallbackRecord(
					state.viewType,
					state.outputId,
					activeRenderFrameId,
					gpuPacketValidationReason,
					static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
					static_cast<std::uint32_t>(state.gpuBatchDescriptors.size()),
					"GPU packet records failed CPU-oracle validation; existing CPU-selected draw coverage remains authoritative."));
				state.gpuPacketRecords.clear();
				state.gpuBatchDescriptors.clear();
			}
			state.gpuPacketFrameSets.push_back(std::move(frameSet));
		}
		if(preparedSelection == nullptr && cachedSelectionEvidence == nullptr) {
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Generated Submission Validation", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			state.generatedSubmissions = BuildGeneratedVirtualGeometrySubmissionRecords(
				state.gpuBatchDescriptors,
				state.gpuPacketRecords,
				activeRenderFrameId);
			std::string generatedSubmissionValidationReason;
			const bool generatedSubmissionsValid = reuseValidatedSelection
				|| ValidateGeneratedVirtualGeometrySubmissionRecords(
					state.generatedSubmissions,
					state.gpuBatchDescriptors,
					activeRenderFrameId,
					state.viewType,
					state.outputId,
					static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
					&generatedSubmissionValidationReason);
			if (!generatedSubmissionsValid)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Fallback Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				state.gpuFallbackRecords.push_back(BuildVirtualGeometryGpuFallbackRecord(
					state.viewType,
					state.outputId,
					activeRenderFrameId,
					generatedSubmissionValidationReason,
					static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
					static_cast<std::uint32_t>(state.gpuBatchDescriptors.size()),
					"Generated virtual geometry submissions failed validation; CPU-expanded packet batches remain authoritative."));
				for (GeneratedVirtualGeometrySubmissionRecord& submission : state.generatedSubmissions)
				{
					submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::FailOpen;
					submission.fallbackReason = generatedSubmissionValidationReason;
				}
			}
		}
		if(preparedSelection == nullptr && cachedSelectionEvidence == nullptr) {
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Compaction Dispatch Planning", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			state.gpuCompactionDispatches = BuildGpuPacketCompactionDispatches(
				state.gpuPacketFrameSets,
				state.hiZValidationState,
				state.debugVisualizationMode);
			for (const CompactedVisibilitySet& compaction : state.CompactionResults())
			{
				state.gpuCounterReadbacks.push_back(BuildVirtualGeometryCounterReadbackRecord(
					compaction,
					activeRenderFrameId,
					VirtualGeometryCounterSource::Prepared,
					0u,
					"Prepared CPU compaction counters are available; GPU readback is populated after backend support is active."));
			}
		}
		if (shadowPass || viewType == RenderViewType::Shadow)
		{
			ShadowVisibilitySelection shadowSelection{};
			shadowSelection.shadowViewId = outputId;
			shadowSelection.frameIndex = activeRenderFrameId;
			shadowSelection.candidatePackets = static_cast<std::uint32_t>(
				state.ClusterPackets().size());
			shadowSelection.selectedPackets = static_cast<std::uint32_t>(
				state.ClusterPackets().size());
			for (const TerraEngine::Renderer::VirtualGeometryViewDrawRecord& draw :
				state.DrawRecords())
			{
				if (!draw.range.virtualGeometryActive)
				{
					continue;
				}
				shadowSelection.selectedMeshlets += draw.range.meshletCount;
				shadowSelection.dispatchedMeshlets += draw.range.meshletCount;
				if (draw.range.failOpen)
				{
					shadowSelection.fallbackReasons.push_back(draw.range.fallbackReason.empty()
						? "shadow virtual geometry failed open"
						: draw.range.fallbackReason);
				}
			}
			state.shadowVisibilitySelections.push_back(std::move(shadowSelection));
		}
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build VG HiZ Evidence",
					TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				HiZRegressionSnapshot hiZRegression{};
				hiZRegression.viewId = outputId;
				hiZRegression.viewType = viewType;
				hiZRegression.frameIndex = activeRenderFrameId;
				hiZRegression.state = state.hiZValidationState.state;
				hiZRegression.reason = state.hiZValidationState.reason;
				hiZRegression.recoveryFrames = state.hiZValidationState.warmupFrames;
				for (const TerraEngine::Renderer::VirtualGeometryViewDrawRecord& draw :
					state.DrawRecords())
				{
					// This snapshot is a prepared, view-local copy of the current Hi-Z
					// contract. GPU readback still owns exact culled counts; prepared
					// counters only prove that the new batched path did not introduce a
					// new hard-disable or false-occlusion policy before submission.
					if (!draw.range.virtualGeometryActive)
					{
						continue;
					}
					hiZRegression.visibleMeshlets += draw.range.meshletCount;
					if (draw.range.failOpen)
					{
						hiZRegression.failOpenMeshlets += draw.range.meshletCount;
						hiZRegression.invalidDepthSamples += draw.range.meshletCount;
					}
					else if (state.hiZValidationState.state == VirtualGeometryHiZState::Active)
					{
						hiZRegression.testedMeshlets += draw.range.meshletCount;
					}
				}
				hiZRegression.baselineMatched =
					state.hiZValidationState.state == VirtualGeometryHiZState::Active
					|| state.hiZValidationState.state == VirtualGeometryHiZState::Unsupported
					|| state.hiZValidationState.failOpen;
				state.hiZRegressionSnapshots.push_back(std::move(hiZRegression));
			}
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build VG Page Evidence",
					TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				if(cachedSelectionEvidence != nullptr)
				{
					// Requests and payloads are already shared by the retained selection
					// owner. The view state carries the current frame identity; individual
					// source rows describe signature-stable selection evidence.
				}
				else
				{
					PopulateVirtualGeometryPageRequestRecords(state);
					PopulateVirtualGeometryPageSourceRecords(state, activeRenderFrameId);
				}
			}
			renderFrameSnapshot.virtualGeometryViewSelections.push_back(std::move(state));
		};

	auto applyVirtualGeometrySelection = [&](std::vector<RenderItemRecord>& commands, const glm::mat4& viewProjection, RenderOutputKind outputKind, RenderViewType viewType, std::uint32_t outputId, const glm::vec3& cameraPosition, const glm::uvec2& targetSize, const VirtualGeometryQualityProfile& viewVirtualGeometryQuality) {
		VirtualGeometryViewSelectionPacket packet;
		std::vector<RenderItemRecord> expandedCommands;
		expandedCommands.reserve(commands.size());
		for (RenderItemRecord& command : commands)
		{
			// Visualizer ownership belongs to the view, not the geometry route. Stamp
			// the selected mode before routing so low-poly direct draws, fail-open
			// fallbacks, and adaptive virtual geometry all reach Vulkan consistently.
			const std::uint32_t debugMode =
				static_cast<std::uint32_t>(viewVirtualGeometryQuality.debugVisualizationMode)
				& kVirtualGeometryDebugModeMask;
			command.virtualGeometryFlags &=
				~(kVirtualGeometryDebugModeMask << kVirtualGeometryDebugModeShift);
			command.virtualGeometryFlags |= debugMode << kVirtualGeometryDebugModeShift;
			const auto capturedMesh = frameMeshResources.find(command.meshId);
			const Mesh* mesh = capturedMesh != frameMeshResources.end()
				? capturedMesh->second : nullptr;
			if (mesh != nullptr
				&& !mesh->hasVirtualGeometry
				&& command.geometryRoute == TerraEngine::Renderer::GeometryRoute::DirectMeshlet
				&& !mesh->virtualGeometryDetailLevels.empty())
			{
				// Low-poly assets select one compact owner-mesh range per camera. This is
				// conventional resident work: no virtual flags, packets, feedback, or
				// page requests are manufactured for the direct path.
				const VirtualGeometryAssetRuntime& runtime =
					getVirtualGeometryRuntime(command.meshId, *mesh);
				const float projectedRadiusPixels = EstimateProjectedMeshRadiusPixels(
					*mesh,
					command.modelMatrix,
					cameraPosition,
					targetSize);
				const TerraEngine::Renderer::VirtualGeometryDrawRange range =
					RenderSystem::SelectDirectResidentLodRange(
						runtime,
						viewVirtualGeometryQuality,
						projectedRadiusPixels);
				command.virtualMeshletOffset = range.meshletOffset;
				command.virtualMeshletCount = range.meshletCount;
				command.selectedVirtualLod = range.selectedLodIndex;
				expandedCommands.push_back(command);
				continue;
			}
			if (mesh == nullptr || !mesh->hasVirtualGeometry
				|| !command.UsesVirtualGeometryRoute())
			{
				// Conventional low-poly work remains an ordinary indexed draw.
				// Do not synthesize virtual cluster/page metadata for it.
				expandedCommands.push_back(command);
				continue;
			}

			const VirtualGeometryAssetRuntime& runtime =
				getVirtualGeometryRuntime(command.meshId, *mesh);
			const float projectedRadiusPixels = EstimateProjectedMeshRadiusPixels(*mesh, command.modelMatrix, cameraPosition, targetSize);
			const std::uint64_t transitionKey = BuildVirtualGeometryTransitionKey(
				outputKind, viewType, outputId, targetSize,
				command.objectId, command.meshId);
			activeVirtualGeometryTransitionKeys.insert(transitionKey);
			const auto previousTransition = virtualGeometryTransitionCache.find(transitionKey);
			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult* previousSelection =
				previousTransition != virtualGeometryTransitionCache.end()
					? &previousTransition->second
					: nullptr;
			TerraEngine::Renderer::VirtualGeometryClusterSelectionResult selection = SelectVirtualGeometryStableClusterCut(
				runtime,
				command.meshId,
				command.objectId,
				viewVirtualGeometryQuality,
				projectedRadiusPixels,
				command.modelMatrix,
				cameraPosition,
				targetSize,
				command.taskShaderCullingEnabled && taskShaderCullingEnabled,
				!mesh->meshletBounds.empty(),
				previousSelection,
				viewType,
				outputId);
			virtualGeometryTransitionCache[transitionKey] = selection;
			if (selection.transitionState.valid)
			{
				packet.lodTransitionStates.push_back(selection.transitionState);
			}
			TerraEngine::Renderer::VirtualGeometryDrawRange range = selection.range;
			range.selectedClusterOffset = static_cast<std::uint32_t>(packet.selectedClusters.size());
			range.pageNeedOffset = static_cast<std::uint32_t>(packet.pageNeeds.size());
			range.selectedClusterCount = static_cast<std::uint32_t>(selection.selectedClusters.size());
			range.pageNeedCount = static_cast<std::uint32_t>(selection.pageNeeds.size());
			std::string coneDisabledReason;
			const bool coneCullingSafe = AreVirtualGeometryClustersConeCullingEligible(runtime, selection.selectedClusters, &coneDisabledReason);
			const std::uint32_t virtualGeometryFlags = (range.virtualGeometryActive ? kVirtualGeometryFlagActive : 0u)
				| (range.failOpen ? kVirtualGeometryFlagFailOpen : 0u)
				| (range.hiZSelfOcclusionGuardPassed ? kVirtualGeometryFlagHiZSelfOcclusionGuard : 0u)
				| (range.hiZNearPlaneGuardPassed ? kVirtualGeometryFlagHiZNearPlaneGuard : 0u)
				| (range.hiZThinGeometryGuardPassed ? kVirtualGeometryFlagHiZThinGeometryGuard : 0u)
				| (coneCullingSafe ? kVirtualGeometryFlagConeCullingSafe : 0u)
				// Processor-expanded VG still keeps the selected LOD range, but its
				// animated vertices must use the proven owner palette bridge until
				// device-authored page work has completed on this backend.
				| (command.animationBinding.RequiresConservativeCoarseVisibility()
					&& !deviceAuthoredVirtualGeometryAvailable
					? kVirtualGeometryFlagOwnerPaletteDeformation : 0u)
				| (debugMode << kVirtualGeometryDebugModeShift);
			if (!coneCullingSafe && TerraEngine::Renderer::RenderDiagnosticsRequested() && !coneDisabledReason.empty())
			{
				renderGraphDiagnostics.backendFallbacks.push_back("Virtual geometry cone culling disabled: " + coneDisabledReason);
			}
			if (range.failOpen && TerraEngine::Renderer::RenderDiagnosticsRequested())
			{
				TerraEngine::Renderer::AddVirtualGeometryFailOpenDiagnostic(renderGraphDiagnostics, command.meshId, range);
			}
			std::vector<TerraEngine::Renderer::VirtualGeometryPageNeedRecord> annotatedPageNeeds = selection.pageNeeds;

			if (command.animationBinding.RequiresOwnerMeshDeformationFallback())
			{
				// Legacy/unsupported pages have no accepted compute publication.
				// Do not reinterpret their page-local cluster range as an owner
				// range; retain complete conservative coverage instead.
				command.virtualMeshletOffset = 0U;
				command.virtualMeshletCount = mesh->runtimeLayout.meshletCount;
				command.selectedVirtualLod = range.selectedLodIndex;
				command.selectedVirtualClusterOffset = range.selectedClusterOffset;
				command.selectedVirtualClusterCount = range.selectedClusterCount;
				command.selectedVirtualPageNeedOffset = range.pageNeedOffset;
				command.selectedVirtualPageNeedCount = range.pageNeedCount;
				command.virtualGeometryPageId = range.pageId;
				command.virtualGeometryFallbackPageId = range.fallbackPageId;
				command.virtualGeometryPayloadSource =
					VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
				command.virtualGeometryEmittedSource =
					VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback;
				command.virtualGeometryPagePayloadByteOffset = 0U;
				command.virtualGeometryPagePayloadByteSize = 0U;
				command.virtualGeometryPayloadBufferIndex = 0xffffffffU;
				command.virtualGeometryPayloadDescriptorIndex = 0xffffffffU;
				command.virtualGeometryPacketDescriptorIndex = 0xffffffffU;
				command.virtualGeometryFlags =
					(virtualGeometryFlags | kVirtualGeometryFlagFailOpen)
					& ~kVirtualGeometryFlagConeCullingSafe;
				expandedCommands.push_back(command);
				packet.selectedClusters.insert(
					packet.selectedClusters.end(),
					selection.selectedClusters.begin(),
					selection.selectedClusters.end());
				packet.pageNeeds.insert(
					packet.pageNeeds.end(),
					annotatedPageNeeds.begin(),
					annotatedPageNeeds.end());
				continue;
			}

			if (selection.selectedClusters.empty())
			{
				command.virtualMeshletOffset = range.meshletOffset;
				command.virtualMeshletCount = range.meshletCount;
				command.selectedVirtualLod = range.selectedLodIndex;
				command.selectedVirtualClusterOffset = range.selectedClusterOffset;
				command.selectedVirtualClusterCount = range.selectedClusterCount;
				command.selectedVirtualPageNeedOffset = range.pageNeedOffset;
				command.selectedVirtualPageNeedCount = range.pageNeedCount;
				command.virtualGeometryPageId = range.pageId;
				command.virtualGeometryFallbackPageId = range.fallbackPageId;
				command.virtualGeometryPayloadSource = range.failOpen
					? VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback
					: VirtualGeometryPagePayloadSource::Unknown;
				command.virtualGeometryEmittedSource = ResolveVirtualGeometryEmittedSource(command.virtualGeometryPayloadSource);
				command.virtualGeometryFlags = virtualGeometryFlags;
				expandedCommands.push_back(command);
				packet.selectedClusters.insert(packet.selectedClusters.end(), selection.selectedClusters.begin(), selection.selectedClusters.end());
				packet.pageNeeds.insert(packet.pageNeeds.end(), annotatedPageNeeds.begin(), annotatedPageNeeds.end());
				continue;
			}

			const std::uint32_t clusterPacketBase = static_cast<std::uint32_t>(packet.clusterPackets.size());
			std::vector<VirtualGeometryClusterPacket> localPackets = BuildVirtualGeometryClusterPackets(
				runtime,
				selection.selectedClusters,
				command.materialId,
				viewType,
				outputId,
				clusterPacketBase);
			// Project the packet AABB once during view preparation. The same bounds
			// drive editor picking, CPU compaction, and the microtriangle route split,
			// keeping all three consumers on one immutable estimate.
			const glm::mat4 packetClipTransform =
				viewProjection * command.modelMatrix;
			for(VirtualGeometryClusterPacket& localPacket : localPackets)
			{
				localPacket.renderableIndex =
					command.conventionalVisibilityIdentityIndex == 0u
						? std::numeric_limits<std::uint32_t>::max()
						: command.conventionalVisibilityIdentityIndex - 1u;
				glm::vec2 minimumUv(
					std::numeric_limits<float>::max());
				glm::vec2 maximumUv(
					std::numeric_limits<float>::lowest());
				bool projectionValid = true;
				for(std::uint32_t corner = 0u; corner < 8u; ++corner)
				{
					const glm::vec3 localCorner(
						(corner & 1u) != 0u
							? localPacket.boundsMax.x
							: localPacket.boundsMin.x,
						(corner & 2u) != 0u
							? localPacket.boundsMax.y
							: localPacket.boundsMin.y,
						(corner & 4u) != 0u
							? localPacket.boundsMax.z
							: localPacket.boundsMin.z);
					const glm::vec4 clip = packetClipTransform
						* glm::vec4(localCorner, 1.0f);
					if(!std::isfinite(clip.w) || clip.w <= 1.0e-5f)
					{
						projectionValid = false;
						break;
					}
					const glm::vec2 ndc = glm::vec2(clip) / clip.w;
					// Camera projections already include Terra's Vulkan Y flip, and
					// the renderer uses a positive-height viewport. Match the actual
					// framebuffer transform used by hardware and compute raster.
					const glm::vec2 uv = ndc * 0.5f + glm::vec2(0.5f);
					minimumUv = glm::min(minimumUv, uv);
					maximumUv = glm::max(maximumUv, uv);
				}
				if(projectionValid)
				{
					localPacket.projectedBounds = glm::vec4(
						minimumUv, maximumUv);
				}
			}
			// Resolve route policy while this immutable command still owns the
			// exact material identity. Numeric route/bin values are copied into
			// packet flags, so protected Vulkan recording never reopens mutable
			// material state.
			const auto capturedMaterial = frameMaterialResources.find(command.materialId);
			const Material* packetMaterial = capturedMaterial != frameMaterialResources.end()
				? capturedMaterial->second : nullptr;
			const VirtualGeometryMaterialExtension* materialPolicy =
				packetMaterial != nullptr
					&& packetMaterial->virtualGeometryExtension.has_value()
				? &packetMaterial->virtualGeometryExtension.value()
				: nullptr;
			const std::uint32_t materialBin =
				materialPolicy != nullptr ? materialPolicy->shadingBin : 0u;
			const std::uint32_t materialBinCapacity =
				materialPolicy != nullptr
					&& materialPolicy->maximumShadingBins != 0u
				? std::min(materialPolicy->maximumShadingBins, 256u)
				: 64u;
			const bool materialForcesCompute =
				materialPolicy != nullptr
				&& materialPolicy->fallbackPolicy
					== VirtualGeometryMaterialFallbackPolicy::Compute;
			const bool materialDisablesVirtualGeometry =
				materialPolicy != nullptr
				&& materialPolicy->fallbackPolicy
					== VirtualGeometryMaterialFallbackPolicy::DisableVirtualGeometry;
			const bool alphaMasked = packetMaterial != nullptr
				&& packetMaterial->header.alphaMode
					== static_cast<std::uint32_t>(AlphaMode::Mask);
			const bool defaultSurfaceShader = packetMaterial != nullptr
				&& (packetMaterial->header.shaderCollectionId
						== TerraEngine::EMPTY_UUID
					|| packetMaterial->header.shaderCollectionId
						== capturedDefaultShaderCollectionId
					|| packetMaterial->header.shaderCollectionId
						== capturedDefaultTaskShaderCollectionId);
			const bool opaqueComputeSurface = packetMaterial != nullptr
				&& packetMaterial->header.alphaMode
					!= static_cast<std::uint32_t>(AlphaMode::Blend);
			const bool deformationSafeComputeSurface =
				command.animationBinding
					.IsComputeRasterDeformationSafe();
			std::vector<TerraEngine::Renderer::VirtualGeometryRasterCandidate>
				rasterCandidates;
			rasterCandidates.reserve(localPackets.size());
			for(std::size_t packetIndex = 0u;
				packetIndex < localPackets.size();
				++packetIndex)
			{
				const VirtualGeometryClusterPacket& localPacket =
					localPackets[packetIndex];
				TerraEngine::Renderer::VirtualGeometryRasterCandidate candidate;
				candidate.stableId = localPacket.debugIdentity;
				candidate.materialBin = materialBin;
				candidate.hardwareSupported =
					hardwareVirtualGeometryRouteAvailable
					&& !materialForcesCompute
					&& !materialDisablesVirtualGeometry;
				candidate.computeSupported =
					computeVirtualGeometryRouteAvailable
					&& !materialDisablesVirtualGeometry
					&& defaultSurfaceShader
					&& opaqueComputeSurface
					&& deformationSafeComputeSurface
					&& localPacket.projectedBounds != glm::vec4(0.0f);
				candidate.alphaMasked = alphaMasked;
				candidate.requiresStableIdentity = true;
				const glm::vec2 clampedMinimum = glm::clamp(
					glm::vec2(localPacket.projectedBounds),
					glm::vec2(0.0f),
					glm::vec2(1.0f));
				const glm::vec2 clampedMaximum = glm::clamp(
					glm::vec2(
						localPacket.projectedBounds.z,
						localPacket.projectedBounds.w),
					glm::vec2(0.0f),
					glm::vec2(1.0f));
				const glm::vec2 projectedExtent = glm::max(
					clampedMaximum - clampedMinimum,
					glm::vec2(0.0f));
				candidate.projectedPixelArea =
					projectedExtent.x * static_cast<float>(targetSize.x)
					* projectedExtent.y * static_cast<float>(targetSize.y);
				for(std::uint32_t meshletIndex = localPacket.meshletOffset;
					meshletIndex < localPacket.meshletOffset
						+ localPacket.meshletCount
						&& meshletIndex < mesh->meshletRecords.size();
					++meshletIndex)
				{
					candidate.triangleCount +=
						mesh->meshletRecords[meshletIndex]
							.primitiveIndexCount / 3u;
				}
				candidate.preferCompute = materialForcesCompute;
				candidate.sourceOrdinal =
					static_cast<std::uint32_t>(packetIndex);
				rasterCandidates.push_back(candidate);
			}
			const TerraEngine::Renderer::VirtualGeometryRasterBinPlan rasterPlan =
				hybridValidationSceneRequested
				? TerraEngine::Renderer::BuildVirtualGeometryHybridRasterBinPlan(
					rasterCandidates,
					materialBinCapacity)
				: TerraEngine::Renderer::BuildVirtualGeometryRasterBinPlan(
					rasterCandidates,
					materialBinCapacity);
			for(std::size_t packetIndex = 0u;
				packetIndex < localPackets.size();
				++packetIndex)
			{
				VirtualGeometryClusterPacket& localPacket =
					localPackets[packetIndex];
				const TerraEngine::Renderer::VirtualGeometryRasterCandidate&
					candidate = rasterCandidates[packetIndex];
				// Raster plans are stable-ID ordered, but retain each packet's
				// source ordinal. Resolve through that ordinal so duplicate debug
				// hashes cannot reorder ownership, page needs, or cluster records.
				const auto plannedOrdinal = std::find(
					rasterPlan.routeSourceOrdinals.begin(),
					rasterPlan.routeSourceOrdinals.end(),
					static_cast<std::uint32_t>(packetIndex));
				const std::size_t plannedIndex =
					static_cast<std::size_t>(std::distance(
						rasterPlan.routeSourceOrdinals.begin(),
						plannedOrdinal));
				TerraEngine::Renderer::VirtualGeometryRasterRoute rasterRoute =
					plannedOrdinal == rasterPlan.routeSourceOrdinals.end()
						|| plannedIndex >= rasterPlan.routes.size()
					? TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed
					: rasterPlan.routes[plannedIndex];
				if(hybridValidationSceneRequested
					&& std::find(
						rasterPlan.fallbackStableIds.begin(),
						rasterPlan.fallbackStableIds.end(),
						candidate.stableId)
						!= rasterPlan.fallbackStableIds.end())
				{
					if(!localPacket.fallbackReason.empty())
					{
						localPacket.fallbackReason += "; ";
					}
					localPacket.fallbackReason +=
						"hybrid validation kept an unsupported candidate visible through indexed fail-open";
				}
				if (validationRouteOverride.has_value())
				{
					const TerraEngine::Renderer::VirtualGeometryRasterRoute requestedRoute =
						*validationRouteOverride;
					const bool packetSupportsRequestedRoute =
						(requestedRoute == TerraEngine::Renderer::VirtualGeometryRasterRoute::Hardware
							&& candidate.hardwareSupported)
						|| (requestedRoute == TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute
							&& candidate.computeSupported)
						|| requestedRoute == TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed;
					if (packetSupportsRequestedRoute) {
						rasterRoute = requestedRoute;
					} else {
						rasterRoute = TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed;
						if (!localPacket.fallbackReason.empty()) localPacket.fallbackReason += "; ";
						localPacket.fallbackReason +=
							"requested validation route is unsupported by this packet/material; used visible indexed fallback";
					}
				}
				if(rasterRoute
					== TerraEngine::Renderer::VirtualGeometryRasterRoute::Fallback)
				{
					rasterRoute =
						TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed;
					if(!localPacket.fallbackReason.empty())
					{
						localPacket.fallbackReason += "; ";
					}
					localPacket.fallbackReason +=
						"material shading-bin capacity overflow used visible indexed fallback";
				}
				localPacket.rasterRoute =
					static_cast<std::uint32_t>(rasterRoute);
				localPacket.materialBin = std::min(materialBin, 255u);
			}
			if (localPackets.size() == selection.selectedClusters.size())
			{
				std::vector<VirtualGeometryClusterPacket> compactedPackets;
				CompactedVisibilitySet compaction = CompactVirtualGeometryClusterPackets(
					localPackets,
					compactedPackets,
					viewType,
					outputId);
				std::vector<ClusterWorkBatch> localBatches = BuildVirtualGeometryClusterBatches(
					compactedPackets,
					clusterPacketBase,
					static_cast<std::uint32_t>(packet.clusterBatches.size()));
				compaction.submittedBatches = static_cast<std::uint32_t>(localBatches.size());

				// Page-need records are authored by selection before batching.
				// Patch them with the packet/batch that currently preserves
				// coverage so streaming diagnostics can follow the same work item
				// that Vulkan receives.
				for (TerraEngine::Renderer::VirtualGeometryPageNeedRecord& need : annotatedPageNeeds)
				{
					std::size_t matchedPacketIndex = 0u;
					bool matchedPacket = false;
					for (std::size_t packetIndex = 0u; packetIndex < compactedPackets.size(); ++packetIndex)
					{
						const VirtualGeometryClusterPacket& localPacket = compactedPackets[packetIndex];
						if (localPacket.fallbackPacket || localPacket.clusterId == need.clusterId)
						{
							matchedPacketIndex = packetIndex;
							matchedPacket = true;
							if (localPacket.clusterId == need.clusterId)
							{
								break;
							}
						}
					}
					if (!matchedPacket)
					{
						continue;
					}
					const std::uint32_t globalPacketIndex = clusterPacketBase + static_cast<std::uint32_t>(matchedPacketIndex);
					need.packetId = compactedPackets[matchedPacketIndex].packetId;
					for (const ClusterWorkBatch& batch : localBatches)
					{
						if (globalPacketIndex >= batch.packetOffset && globalPacketIndex < batch.packetOffset + batch.packetCount)
						{
							need.batchId = batch.batchId;
							need.packetOffset = batch.packetOffset;
							need.packetCount = batch.packetCount;
							break;
						}
					}
				}
				for (const TerraEngine::Renderer::VirtualGeometryPageNeedRecord& need : annotatedPageNeeds)
				{
					if (!need.fallbackRequired)
					{
						continue;
					}
					FallbackCoverageChain chain;
					chain.meshId = need.meshId;
					chain.desiredClusterId = need.clusterId;
					chain.fallbackPageId = 0u;
					chain.coverageStatus = VirtualGeometryFallbackCoverageStatus::Complete;
					chain.reason = "resident fallback coverage preserved while desired page streams";
					for (const VirtualGeometryClusterPacket& fallbackPacket : compactedPackets)
					{
						if (!fallbackPacket.fallbackPacket)
						{
							continue;
						}
						chain.fallbackClusterId = fallbackPacket.clusterId;
						chain.fallbackPageId = fallbackPacket.pageId;
						chain.substitutedMeshlets = fallbackPacket.meshletCount;
						break;
					}
					if (chain.substitutedMeshlets == 0u)
					{
						chain.coverageStatus = VirtualGeometryFallbackCoverageStatus::Partial;
						chain.reason = "fallback request is pending; compacted packets kept fail-open coverage";
					}
					packet.fallbackCoverage.push_back(std::move(chain));
				}

				packet.clusterPackets.insert(packet.clusterPackets.end(), compactedPackets.begin(), compactedPackets.end());
				packet.clusterBatches.insert(packet.clusterBatches.end(), localBatches.begin(), localBatches.end());
				packet.compactionResults.push_back(compaction);
				packet.selectedClusters.insert(packet.selectedClusters.end(), selection.selectedClusters.begin(), selection.selectedClusters.end());
				packet.pageNeeds.insert(packet.pageNeeds.end(), annotatedPageNeeds.begin(), annotatedPageNeeds.end());

				for (const ClusterWorkBatch& batch : localBatches)
				{
					const std::uint32_t localPacketOffset = batch.packetOffset - clusterPacketBase;
					const VirtualGeometryClusterPacket& firstPacket = compactedPackets[localPacketOffset];
					const VirtualGeometryClusterPacket& lastPacket = compactedPackets[localPacketOffset + batch.packetCount - 1u];
					RenderItemRecord batchCommand = command;
					batchCommand.virtualMeshletOffset = firstPacket.meshletOffset;
					batchCommand.virtualMeshletCount = (lastPacket.meshletOffset + lastPacket.meshletCount) - firstPacket.meshletOffset;
					batchCommand.selectedVirtualLod = firstPacket.selectedLod;
					batchCommand.selectedVirtualClusterOffset = range.selectedClusterOffset + (firstPacket.packetId - clusterPacketBase);
					batchCommand.selectedVirtualClusterCount = batch.packetCount;
					batchCommand.selectedVirtualPageNeedOffset = range.pageNeedOffset;
					batchCommand.selectedVirtualPageNeedCount = 0u;
					batchCommand.virtualGeometryPageId = firstPacket.pageId;
					batchCommand.virtualGeometryFallbackPageId = firstPacket.fallbackPageId;
					batchCommand.virtualGeometryPayloadSource = firstPacket.payloadSource;
					batchCommand.virtualGeometryEmittedSource = firstPacket.emittedSource;
					batchCommand.virtualGeometryPagePayloadByteOffset = firstPacket.pagePayloadByteOffset;
					batchCommand.virtualGeometryPagePayloadByteSize = firstPacket.pagePayloadByteSize;
					batchCommand.virtualGeometryPayloadBufferIndex = firstPacket.payloadBufferIndex;
					batchCommand.virtualGeometryPayloadDescriptorIndex = firstPacket.payloadDescriptorIndex;
					// Page-local payload uploads are keyed by the submitted packet id.
					// Do not reuse selected-cluster offsets here: package imports and
					// fallback compaction can make selection rows and packet rows drift.
					batchCommand.virtualGeometryPacketDescriptorIndex = firstPacket.packetId;
					for (std::uint32_t packetIndex = localPacketOffset; packetIndex < localPacketOffset + batch.packetCount; ++packetIndex)
					{
						if (compactedPackets[packetIndex].fallbackPacket)
						{
							batchCommand.selectedVirtualPageNeedCount = range.pageNeedCount;
							break;
						}
					}
					// The route belongs to this homogeneous packet batch. Carry it in
					// the established flags word so Vulkan can omit compute-owned main
					// draws only after the visibility composite is proven recordable.
					batchCommand.virtualGeometryFlags = virtualGeometryFlags
						| ((firstPacket.rasterRoute
							& kVirtualGeometryRasterRouteMask)
							<< kVirtualGeometryRasterRouteShift);
					expandedCommands.push_back(batchCommand);
				}
				continue;
			}

			{
				// If corrupt metadata made every selected cluster unresolvable,
				// keep the preselection range visible and let validation/diagnostic
				// paths report the bad cluster ids instead of dropping the object.
				command.virtualMeshletOffset = range.meshletOffset;
				command.virtualMeshletCount = range.meshletCount;
				command.selectedVirtualLod = range.selectedLodIndex;
				command.selectedVirtualClusterOffset = range.selectedClusterOffset;
				command.selectedVirtualClusterCount = range.selectedClusterCount;
				command.selectedVirtualPageNeedOffset = range.pageNeedOffset;
				command.selectedVirtualPageNeedCount = range.pageNeedCount;
				command.virtualGeometryPageId = range.pageId;
				command.virtualGeometryFallbackPageId = range.fallbackPageId;
				command.virtualGeometryPayloadSource = VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
				command.virtualGeometryEmittedSource = VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback;
				command.virtualGeometryFlags = virtualGeometryFlags | kVirtualGeometryFlagFailOpen;
				expandedCommands.push_back(command);
			}
			packet.selectedClusters.insert(packet.selectedClusters.end(), selection.selectedClusters.begin(), selection.selectedClusters.end());
			packet.pageNeeds.insert(packet.pageNeeds.end(), annotatedPageNeeds.begin(), annotatedPageNeeds.end());
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Packet Upload Planning", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			packet.gpuPacketRecords = BuildGpuVirtualGeometryPacketRecords(
				packet.clusterPackets,
				activeRenderFrameId,
				viewVirtualGeometryQuality.debugVisualizationMode);
			packet.gpuBatchDescriptors = BuildGpuVirtualGeometryBatchDescriptors(
				packet.gpuPacketRecords,
				activeRenderFrameId,
				viewVirtualGeometryQuality.debugVisualizationMode);
		}
		std::string gpuPacketValidationReason;
		const bool gpuPacketRecordsValid = ValidateGpuVirtualGeometryPacketRecords(
			packet.clusterPackets,
			packet.selectedClusters,
			packet.gpuPacketRecords,
			activeRenderFrameId,
			viewVirtualGeometryQuality.debugVisualizationMode,
			&gpuPacketValidationReason);
		if (!packet.gpuPacketRecords.empty() || !packet.gpuBatchDescriptors.empty())
		{
			GpuVirtualGeometryPacketFrameSet frameSet = BuildGpuVirtualGeometryPacketFrameSet(
				viewType,
				outputId,
				activeRenderFrameId,
				targetSize,
				viewVirtualGeometryQuality.debugVisualizationMode,
				0u,
				static_cast<std::uint32_t>(packet.gpuPacketRecords.size()),
				0u,
				static_cast<std::uint32_t>(packet.gpuBatchDescriptors.size()));
			if (!gpuPacketRecordsValid)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Fallback Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				frameSet.validity = VirtualGeometryGpuFrameSetValidity::FailOpen;
				frameSet.packetCount = 0u;
				frameSet.batchCount = 0u;
				frameSet.compactedPacketCount = 0u;
				frameSet.generatedSubmissionCount = 0u;
				frameSet.fallbackReason = gpuPacketValidationReason;
				packet.gpuPacketRecords.clear();
				packet.gpuBatchDescriptors.clear();
			}
			packet.gpuPacketFrameSets.push_back(std::move(frameSet));
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Generated Submission Validation", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			packet.generatedSubmissions = BuildGeneratedVirtualGeometrySubmissionRecords(
				packet.gpuBatchDescriptors,
				packet.gpuPacketRecords,
				activeRenderFrameId);
			std::string generatedSubmissionValidationReason;
			if (!ValidateGeneratedVirtualGeometrySubmissionRecords(
				packet.generatedSubmissions,
				packet.gpuBatchDescriptors,
				activeRenderFrameId,
				viewType,
				outputId,
				static_cast<std::uint32_t>(packet.gpuPacketRecords.size()),
				&generatedSubmissionValidationReason))
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Fallback Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				packet.gpuFallbackRecords.push_back(BuildVirtualGeometryGpuFallbackRecord(
					viewType,
					outputId,
					activeRenderFrameId,
					generatedSubmissionValidationReason,
					static_cast<std::uint32_t>(packet.gpuPacketRecords.size()),
					static_cast<std::uint32_t>(packet.gpuBatchDescriptors.size()),
					"Generated virtual geometry submissions failed validation; CPU-expanded packet batches remain authoritative."));
				for (GeneratedVirtualGeometrySubmissionRecord& submission : packet.generatedSubmissions)
				{
					submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::FailOpen;
					submission.fallbackReason = generatedSubmissionValidationReason;
				}
			}
		}
		VirtualGeometryHiZValidationState packetHiZState;
		packetHiZState.state = targetSize.x == 0u || targetSize.y == 0u
			? VirtualGeometryHiZState::MissingPyramid
			: VirtualGeometryHiZState::Active;
		packetHiZState.renderSize = targetSize;
		packetHiZState.pyramidSize = targetSize;
		packetHiZState.pyramidReady = packetHiZState.state == VirtualGeometryHiZState::Active;
		packetHiZState.failOpen = packetHiZState.state != VirtualGeometryHiZState::Active;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Compaction Dispatch Planning", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			packet.gpuCompactionDispatches = BuildGpuPacketCompactionDispatches(
				packet.gpuPacketFrameSets,
				packetHiZState,
				viewVirtualGeometryQuality.debugVisualizationMode);
			for (const CompactedVisibilitySet& compaction : packet.compactionResults)
			{
				packet.gpuCounterReadbacks.push_back(BuildVirtualGeometryCounterReadbackRecord(
					compaction,
					activeRenderFrameId,
					VirtualGeometryCounterSource::Prepared,
					0u,
					"Prepared CPU compaction counters are available; GPU readback is populated after backend support is active."));
			}
		}
		commands = std::move(expandedCommands);
		collectVirtualGeometryDiagnosticsAssets(commands, viewType);
		return packet;
	};

	std::atomic<bool> fixtureShadowSelectionApplied = false;
	auto applyShadowVirtualGeometrySelection = [&](std::vector<RenderItemRecord>& commands) {
		for (RenderItemRecord& command : commands)
		{
			const auto capturedMesh = frameMeshResources.find(command.meshId);
			const Mesh* mesh = capturedMesh != frameMeshResources.end()
				? capturedMesh->second : nullptr;
			if (mesh != nullptr
				&& !mesh->hasVirtualGeometry
				&& command.geometryRoute == TerraEngine::Renderer::GeometryRoute::DirectMeshlet
				&& !mesh->virtualGeometryDetailLevels.empty())
			{
				// Shadow silhouettes use direct LOD0 conservatively while camera views
				// remain free to choose a cheaper resident range.
				const VirtualGeometryDetailLevelRecord& baseDetail =
					mesh->virtualGeometryDetailLevels.front();
				command.virtualMeshletOffset = baseDetail.meshletOffset;
				command.virtualMeshletCount = baseDetail.meshletCount;
				command.selectedVirtualLod = baseDetail.lodIndex;
				continue;
			}
			if (mesh == nullptr || !mesh->hasVirtualGeometry
				|| !command.UsesVirtualGeometryRoute())
			{
				// Shadow views consume the same representation decision as the
				// camera view, preserving identity without hidden VG work.
				continue;
			}

			const VirtualGeometryAssetRuntime& runtime =
				getVirtualGeometryRuntime(command.meshId, *mesh);
			const TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryShadowLodRange(
				runtime,
				renderFrameSnapshot.renderSettings.virtualGeometry,
				!mesh->meshletBounds.empty());

			command.virtualMeshletOffset = range.meshletOffset;
			command.virtualMeshletCount = range.meshletCount;
			command.selectedVirtualLod = range.selectedLodIndex;
			command.virtualGeometryFlags = (range.virtualGeometryActive ? kVirtualGeometryFlagActive : 0u)
				| (range.failOpen ? kVirtualGeometryFlagFailOpen : 0u);
		}
		const auto& intent = renderFrameSnapshot.efficiencyFixtureIntent;
		if (intent.controlsApplied && intent.shadowSelection)
		{
			const auto target = std::find_if(fixtureTargets.begin(), fixtureTargets.end(), [&](const auto& row) { return row.objectId == intent.shadowSelection->objectId; });
			if (target != fixtureTargets.end())
				for (auto& command : commands)
					if (TerraEngine::Renderer::ApplyRendererEfficiencyFixtureSelection(command, *intent.shadowSelection, *target)) fixtureShadowSelectionApplied.store(true, std::memory_order_relaxed);
		}
		collectVirtualGeometryDiagnosticsAssets(commands, RenderViewType::Shadow);
	};

	auto buildVisibleStreamingTouchSet = [&](const std::vector<RenderItemRecord>& commands,
		RenderViewType viewType, std::uint32_t outputId,
		const glm::vec3& cameraPosition,
		TerraEngine::Resource::TerrainVisibleTextureTouchSnapshot* terrainTouches) {
		TerraEngine::Renderer::VisibleStreamingTouchSet touchSet;
		touchSet.viewType = viewType;
		touchSet.outputId = outputId;
		touchSet.sourceCommandCount = static_cast<std::uint32_t>(std::min<std::size_t>(commands.size(), std::numeric_limits<std::uint32_t>::max()));
		touchSet.valid = true;

		std::unordered_set<TerraEngine::UUID> uniqueMeshes;
		std::unordered_set<TerraEngine::UUID> uniqueMaterials;
		std::unordered_map<TerraEngine::UUID, std::size_t> textureTouchIndices;
		uniqueMeshes.reserve(commands.size());
		uniqueMaterials.reserve(commands.size());
		textureTouchIndices.reserve(commands.size());
		std::vector<TerraEngine::Resource::TerrainVisibleMaterialArea> visibleTerrainAreas;
		visibleTerrainAreas.reserve(commands.size());

		std::uint32_t sourceTouchCount = 0u;
		for (const RenderItemRecord& command : commands)
		{
			if (command.meshId != TerraEngine::EMPTY_UUID)
			{
				++sourceTouchCount;
				if (uniqueMeshes.insert(command.meshId).second)
				{
					touchSet.meshes.push_back({ command.meshId, ::VisibleStreamingResourceKind::Mesh, 100 });
				}
			}
			if (command.materialId != TerraEngine::EMPTY_UUID)
			{
				++sourceTouchCount;
				if (uniqueMaterials.insert(command.materialId).second)
				{
					touchSet.materials.push_back({ command.materialId, ::VisibleStreamingResourceKind::Material, 100 });
				}
			}
			const glm::vec3 objectPosition = glm::vec3(command.modelMatrix[3]);
			const double viewDistance = static_cast<double>(
				glm::length(objectPosition - cameraPosition));
			if(const auto terrainObject = frameTerrainObjects.find(command.objectId);
				terrainObject != frameTerrainObjects.end())
			{
				const auto terrainMaterial = frameTerrainMaterialBindings.find(
					terrainObject->second.second);
				if(terrainMaterial != frameTerrainMaterialBindings.end())
				{
					const auto appendArea = [&](TerraEngine::Terrain::TerrainAreaCoordinate area,
						double sampleSpacingMetres) {
						TerraEngine::Resource::TerrainVisibleMaterialArea visible{};
						visible.assetId = terrainObject->second.first;
						visible.area = area;
						visible.materialId = terrainMaterial->second.materialId;
						visible.weightTextureId = terrainMaterial->second.weightTextureId;
						visible.layerIdTextureId = terrainMaterial->second.layerIdTextureId;
						visible.layerCount = std::min<std::uint32_t>(
							terrainMaterial->second.layerCount,
							static_cast<std::uint32_t>(visible.layers.size()));
						for(std::uint32_t layerIndex = 0u;
							layerIndex < visible.layerCount; ++layerIndex)
						{
							const auto& source = terrainMaterial->second.layers[layerIndex];
							visible.layers[layerIndex] = {source.stableLayerId,
								source.baseColorTextureId, source.normalTextureId,
								source.ormTextureId, source.emissiveTextureId};
						}
						// The command translation is already localized to this view. Add the
						// canonical area offset in double precision so distant signed area
						// indices do not collapse before nearest-distance aggregation.
						const glm::dvec3 objectDelta = glm::dvec3(objectPosition - cameraPosition);
						const glm::dvec3 areaOffset{
							static_cast<double>(area.x) * 256.0 * sampleSpacingMetres,
							0.0,
							static_cast<double>(area.z) * 256.0 * sampleSpacingMetres};
						visible.nearestViewDistance = glm::length(objectDelta + areaOffset);
						visibleTerrainAreas.push_back(std::move(visible));
					};
					if(terrainMaterial->second.areaMappings.empty())
					{
						// Temporary direct origin proxies predate per-area phase tables.
						// Capture their real sources as area zero until their next publication.
						appendArea({}, 1.0);
					}
					else
					{
						for(const auto& area : terrainMaterial->second.areaMappings)
							appendArea(area.area, area.sampleSpacingMetres);
					}
				}
			}

			const auto capturedMaterial = frameMaterialResources.find(command.materialId);
			const Material* material = capturedMaterial != frameMaterialResources.end()
				? capturedMaterial->second : nullptr;
			if (material == nullptr)
			{
				continue;
			}
			for (const TextureBinding& binding : material->textures)
			{
				if (binding.textureUuid == TerraEngine::EMPTY_UUID)
				{
					continue;
				}
				++sourceTouchCount;
				auto [existingTexture, inserted] = textureTouchIndices.emplace(binding.textureUuid, touchSet.textures.size());
				if (inserted)
				{
					touchSet.textures.push_back({ binding.textureUuid, viewDistance, 90 });
				}
				else
				{
					TerraEngine::Renderer::VisibleTextureTouch& touch = touchSet.textures[existingTexture->second];
					touch.nearestViewDistance = std::min(touch.nearestViewDistance, viewDistance);
				}
			}
		}
		const auto preparedTerrainTouches = TerraEngine::Resource::
			PrepareTerrainVisibleTextureTouchSnapshot(activeRenderFrameId, visibleTerrainAreas);
		if(terrainTouches != nullptr) *terrainTouches = preparedTerrainTouches;
		if(preparedTerrainTouches.complete)
		{
			sourceTouchCount += static_cast<std::uint32_t>(std::min<std::size_t>(
				preparedTerrainTouches.touches.size()
					+ preparedTerrainTouches.duplicateEliminationCount,
				std::numeric_limits<std::uint32_t>::max() - sourceTouchCount));
			for(const TerraEngine::Resource::TerrainVisibleTextureTouch& terrainTouch
				: preparedTerrainTouches.touches)
			{
				auto [existingTexture, inserted] = textureTouchIndices.emplace(
					terrainTouch.textureId, touchSet.textures.size());
				if(inserted)
				{
					touchSet.textures.push_back({terrainTouch.textureId,
						terrainTouch.nearestViewDistance, terrainTouch.priority});
				}
				else
				{
					auto& touch = touchSet.textures[existingTexture->second];
					touch.nearestViewDistance = std::min(
						touch.nearestViewDistance, terrainTouch.nearestViewDistance);
					touch.priority = std::max(touch.priority, terrainTouch.priority);
				}
			}
		}

		const std::uint32_t uniqueTouchCount =
			static_cast<std::uint32_t>(std::min<std::size_t>(
				touchSet.meshes.size() + touchSet.materials.size() + touchSet.textures.size(),
				std::numeric_limits<std::uint32_t>::max()));
		touchSet.duplicateEliminationCount = sourceTouchCount > uniqueTouchCount ? sourceTouchCount - uniqueTouchCount : 0u;
		return touchSet;
	};

	// Worker visibility records resource demand without mutating ResourceManager.
	// Boundary 3 publishes these compact touch sets after all view work is sealed.
	std::vector<std::pair<RenderViewType,
		TerraEngine::Renderer::VisibleStreamingTouchSet>> pendingStreamingTouchSets;
	pendingStreamingTouchSets.reserve(renderFrameSnapshot.viewRequests.size());

	auto buildVisibleCommandReuseTelemetry = [&](RenderViewType viewType, std::uint32_t outputId, bool cacheHit, bool copiedCommands, const TerraEngine::Renderer::VisibleStreamingTouchSet& touchSet) {
		TerraEngine::Renderer::VisibleCommandReuseTelemetry telemetry;
		telemetry.viewType = viewType;
		telemetry.outputId = outputId;
		telemetry.cacheHit = cacheHit;
		telemetry.commandCount = touchSet.sourceCommandCount;
		telemetry.copiedCommandCount = copiedCommands ? touchSet.sourceCommandCount : 0u;
		telemetry.uniqueMeshTouches = static_cast<std::uint32_t>(std::min<std::size_t>(touchSet.meshes.size(), std::numeric_limits<std::uint32_t>::max()));
		telemetry.uniqueMaterialTouches = static_cast<std::uint32_t>(std::min<std::size_t>(touchSet.materials.size(), std::numeric_limits<std::uint32_t>::max()));
		telemetry.uniqueTextureTouches = static_cast<std::uint32_t>(std::min<std::size_t>(touchSet.textures.size(), std::numeric_limits<std::uint32_t>::max()));
		telemetry.duplicateResourceTouchesEliminated = touchSet.duplicateEliminationCount;
		telemetry.detailedDiagnosticsRequested = TerraEngine::Renderer::RenderDiagnosticsRequested();
		return telemetry;
	};

	const std::uint64_t virtualGeometryResidencyGeneration =
		capturedVirtualGeometryResidencyGeneration;
	auto buildVisibleDrawCommands = [&](const glm::mat4& viewProjection,
		const glm::uvec2& targetSize,
		RenderOutputKind outputKind,
		RenderViewType viewType,
		std::uint32_t outputId,
		const glm::vec3& cameraPosition,
		const TerraEngine::Renderer::RenderReferenceFrame& referenceFrame) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Visible Draw Commands Cache Lookup", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto localizeViewCommands = [&](std::vector<RenderItemRecord>& commands)
		{
			commands.erase(std::remove_if(commands.begin(), commands.end(),
				[&](RenderItemRecord& command)
				{
					const TerraEngine::Renderer::RenderLocalizedPoseMatrices localized =
						TerraEngine::Renderer::BuildLocalizedRenderPoseMatrices(
							command.canonicalWorldPose, referenceFrame);
					if (!localized.valid)
					{
						return true;
					}
					command.modelMatrix = localized.currentMatrix;
					return false;
				}), commands.end());
		};
		VirtualGeometryQualityProfile viewVirtualGeometryQuality = renderFrameSnapshot.renderSettings.virtualGeometry;
		viewVirtualGeometryQuality.debugVisualizationMode = GetVirtualGeometryDebugVisualizationMode(viewType, outputId);
		viewVirtualGeometryQuality = SanitizeVirtualGeometryQualityProfile(viewVirtualGeometryQuality);
		RenderSettings viewRenderSettings = renderFrameSnapshot.renderSettings;
		viewRenderSettings.virtualGeometry = viewVirtualGeometryQuality;
		std::uint64_t signature = BuildVisibleCommandCacheKey(
			renderFrameSnapshot.sceneRevision,
			BuildRenderSettingsSignature(viewRenderSettings, currentPreset, taskShaderCullingEnabled),
			renderableVisibilitySignature,
			viewProjection,
			targetSize,
			outputKind,
			viewType,
			outputId,
			BuildVirtualGeometryTransitionStateSignature(virtualGeometryTransitionCache, outputKind, viewType, outputId),
			virtualGeometryResidencyGeneration,
			renderFrameSnapshot.resourceRevision,
			snapshotRendererGeneration);
		// Slot lifetime keys and packet-template identities are topology-owned. A
		// rebuilt allocator root must never reuse a retained view-selection payload.
		HashCombine(signature, retainedRenderTopology.topology.generation);
		HashCombine(signature, static_cast<std::uint64_t>(
			TerraEngine::Scene::WorldPositionHash{}(referenceFrame.canonicalOrigin)));
		HashCombine(signature, referenceFrame.originGeneration);
		HashCombine(signature, referenceFrame.spatialGeneration);
		HashCombine(signature, referenceFrame.residencyGeneration);
		const auto materializeCachedCommands = [&](const VisibleCommandCacheRecord& record)
		{
			std::vector<RenderItemRecord> commands;
			const std::size_t count = record.stableOrdinals.size();
			commands.reserve(count);
			for(std::size_t index = 0u; index < count; ++index)
			{
				const std::uint32_t ordinal = record.stableOrdinals[index];
				if(ordinal >= renderableDrawCommands.size()
					|| ordinal >= renderables.size()
					|| index >= record.selectedLods.size()
					|| index >= record.routeSelectors.size()
					|| index >= record.editorTints.size()
					|| record.commandLanes == nullptr
					|| index >= record.commandLanes->size()) {
					continue;
				}
				RenderItemRecord command = renderableDrawCommands[ordinal];
				command.animation = BuildPublishedAnimationGeneration(
					renderFrameSnapshot.renderables[ordinal].animation);
				command.animationBinding =
					TerraEngine::Renderer::PublishedAnimationRenderBinding(
						renderFrameSnapshot.renderables[ordinal].animationBinding);
				ApplyRenderViewCommandLane((*record.commandLanes)[index],
					record.selectedLods[index],
					static_cast<TerraEngine::Renderer::GeometryRoute>(
						record.routeSelectors[index]),
					record.editorTints[index], command);
				commands.push_back(std::move(command));
			}
			localizeViewCommands(commands);
			return commands;
		};
		if (const auto existing = visibleCommandCache.recordsBySignature.find(signature); existing != visibleCommandCache.recordsBySignature.end())
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Visible Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			++visibleCommandCache.hitCount;
			existing->second.lastTouchedFrameId = activeRenderFrameId;
			std::vector<RenderItemRecord> cachedCommands;
			if(!existing->second.hasViewDrawEvidence
				|| !existing->second.viewDraws)
			{
				// Legacy/incomplete cache records still fail open through complete command
				// reconstruction. Current records retain compact view draws and do not need
				// a crowd-sized command array merely to republish the same selection.
				cachedCommands = materializeCachedCommands(existing->second);
			}
			if (TerraEngine::Renderer::RenderDiagnosticsRequested())
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Visible Diagnostics", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				if(!cachedCommands.empty())
				{
					collectVirtualGeometryDiagnosticsAssets(cachedCommands, viewType);
				}
				else
				{
					// Diagnostics need one runtime entry per mesh, not one complete draw
					// record per instance. Resolve unique cached ordinals directly from the
					// shared base working set and keep command materialization deferred.
					std::vector<RenderItemRecord> uniqueMeshCommands;
					std::unordered_set<TerraEngine::UUID> seenMeshes;
					for(const std::uint32_t ordinal : existing->second.stableOrdinals)
					{
						if(ordinal >= renderableDrawCommands.size()) continue;
						const RenderItemRecord& command = renderableDrawCommands[ordinal];
						if(seenMeshes.insert(command.meshId).second)
							uniqueMeshCommands.push_back(command);
					}
					collectVirtualGeometryDiagnosticsAssets(uniqueMeshCommands, viewType);
				}
			}
			const RenderViewType diagnosticsViewType = outputKind == RenderOutputKind::MainRuntimeView ? RenderViewType::ExportRuntime : viewType;
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Visible VG Selection Stats", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				const auto& retained = *existing->second.retainedSelectionEvidence;
				recordVirtualGeometryViewSelection(cachedCommands,
					retained.selectedClusters, retained.pageNeeds,
					retained.clusterPackets, retained.clusterBatches,
					retained.compactionResults, retained.fallbackCoverage,
					retained.lodTransitionStates,
					viewVirtualGeometryQuality.debugVisualizationMode, outputKind,
					diagnosticsViewType, targetSize, outputId, false,
					outputKind == RenderOutputKind::MainRuntimeView,
					&existing->second.virtualGeometryCommandIndices, true, nullptr,
					&existing->second);
			}
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Visible Streaming Touch Set", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				if (!existing->second.streamingTouchSet.valid)
				{
					existing->second.reuseTelemetry.conservativeRefresh = true;
					existing->second.reuseTelemetry.fallbackReason = "cached visible streaming touch set was unavailable";
					existing->second.streamingTouchSet = buildVisibleStreamingTouchSet(
						cachedCommands, viewType, outputId, cameraPosition,
						&existing->second.terrainTextureTouches);
				}
				pendingStreamingTouchSets.emplace_back(
					viewType, existing->second.streamingTouchSet);
			}
			VisibleCommandBuildResult cachedResult;
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reference Cached Visible Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				// Binding refresh is fused into the mandatory command materialization below.
				// The cache's revision-validated indices avoid both this former full loop
				// and a second crowd-sized overlay allocation on every hit.
				cachedResult.cachedRecord = &existing->second;
				cachedResult.commands = std::move(cachedCommands);
				cachedResult.packetEvidenceSignature = existing->second.signature;
				cachedResult.streamingTouchSet = existing->second.streamingTouchSet;
				cachedResult.terrainTextureTouches = existing->second.terrainTextureTouches;
				cachedResult.reuseTelemetry = buildVisibleCommandReuseTelemetry(viewType, outputId, true, true, existing->second.streamingTouchSet);
				if (existing->second.reuseTelemetry.conservativeRefresh)
				{
					cachedResult.reuseTelemetry.conservativeRefresh = true;
					cachedResult.reuseTelemetry.fallbackReason = existing->second.reuseTelemetry.fallbackReason;
				}
				cachedResult.diagnostics = existing->second.diagnostics;
				cachedResult.cacheHit = true;
			}
			return cachedResult;
		}

		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Rebuild Visible Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const TerraEngine::Scene::SpatialFrustum frustum = TerraEngine::Scene::ExtractFrustum(viewProjection);
		TerraEngine::Scene::SpatialQueryFilter filter{};
		filter.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;
		filter.collectDiagnostics = TerraEngine::Renderer::RenderDiagnosticsRequested();
		const TerraEngine::Scene::SpatialReferenceFrame indexedFrame =
			spatialIndex.GetReferenceFrame();
		VisibleCommandBuildResult result;
		result.packetEvidenceSignature = signature;
		if (indexedFrame.canonicalOrigin == referenceFrame.canonicalOrigin
			&& indexedFrame.generation == referenceFrame.spatialGeneration)
		{
			const TerraEngine::Scene::SpatialQueryResult query =
				spatialIndex.QueryFrustum(frustum, filter);
			result.commands = buildDrawCommandsFromCandidateIndices(
				collectCandidateIndices(query));
		}
		else
		{
			// A view outside the currently materialized region must not reinterpret
			// that region's float index. Until its resident region index is published,
			// localize the immutable command snapshot and fail closed on out-of-range
			// records; no live scene storage is scanned here.
			result.commands = renderableDrawCommands;
		}
		localizeViewCommands(result.commands);
		VirtualGeometryViewSelectionPacket selectionPacket = applyVirtualGeometrySelection(result.commands, viewProjection, outputKind, viewType, outputId, cameraPosition, targetSize, viewVirtualGeometryQuality);
		const RenderViewType diagnosticsViewType = outputKind == RenderOutputKind::MainRuntimeView ? RenderViewType::ExportRuntime : viewType;
		recordVirtualGeometryViewSelection(result.commands, selectionPacket.selectedClusters, selectionPacket.pageNeeds, selectionPacket.clusterPackets, selectionPacket.clusterBatches, selectionPacket.compactionResults, selectionPacket.fallbackCoverage, selectionPacket.lodTransitionStates, viewVirtualGeometryQuality.debugVisualizationMode, outputKind, diagnosticsViewType, targetSize, outputId, false, outputKind == RenderOutputKind::MainRuntimeView, nullptr, true, &selectionPacket);
		result.selectedClusters = std::move(selectionPacket.selectedClusters);
		result.pageNeeds = std::move(selectionPacket.pageNeeds);
		result.clusterPackets = std::move(selectionPacket.clusterPackets);
		result.clusterBatches = std::move(selectionPacket.clusterBatches);
		result.gpuPacketRecords = std::move(selectionPacket.gpuPacketRecords);
		result.gpuBatchDescriptors = std::move(selectionPacket.gpuBatchDescriptors);
		result.gpuPacketFrameSets = std::move(selectionPacket.gpuPacketFrameSets);
		result.gpuCompactionDispatches = std::move(selectionPacket.gpuCompactionDispatches);
		result.generatedSubmissions = std::move(selectionPacket.generatedSubmissions);
		result.gpuCounterReadbacks = std::move(selectionPacket.gpuCounterReadbacks);
		result.gpuFallbackRecords = std::move(selectionPacket.gpuFallbackRecords);
		result.compactionResults = std::move(selectionPacket.compactionResults);
		result.fallbackCoverage = std::move(selectionPacket.fallbackCoverage);
		result.lodTransitionStates = std::move(selectionPacket.lodTransitionStates);
		result.diagnostics = latestSpatialRefreshDiagnostics;
		const TerraEngine::Scene::SpatialDiagnosticsSnapshot queryDiagnostics = spatialIndex.GetDiagnosticsSnapshot();
		result.diagnostics.visibleCandidateCount = queryDiagnostics.visibleCandidateCount;
		result.diagnostics.rejectedObjectCount = queryDiagnostics.rejectedObjectCount;
		result.diagnostics.fallbackObjectCount = queryDiagnostics.fallbackObjectCount;
		result.diagnostics.lastQueryDurationMs = queryDiagnostics.lastQueryDurationMs;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Visible Streaming Touch Set", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			result.streamingTouchSet = buildVisibleStreamingTouchSet(
				result.commands, viewType, outputId, cameraPosition,
				&result.terrainTextureTouches);
			result.reuseTelemetry = buildVisibleCommandReuseTelemetry(viewType, outputId, false, false, result.streamingTouchSet);
		}
		pendingStreamingTouchSets.emplace_back(viewType, result.streamingTouchSet);
		VisibleCommandCacheRecord record;
		record.signature = signature;
		record.lastTouchedFrameId = activeRenderFrameId;
		record.stableOrdinals.reserve(result.commands.size());
		record.selectedLods.reserve(result.commands.size());
		record.routeSelectors.reserve(result.commands.size());
		record.editorTints.reserve(result.commands.size());
		std::vector<RenderViewCommandLane> recordCommandLanes;
		recordCommandLanes.reserve(result.commands.size());
		for(std::size_t commandIndex = 0U;
			commandIndex < result.commands.size(); ++commandIndex)
		{
			const RenderItemRecord& command = result.commands[commandIndex];
			const auto current = renderableCommandIndices.find(command.objectId);
			const bool indexRepresentable = current != renderableCommandIndices.end()
				&& current->second <= std::numeric_limits<std::uint32_t>::max();
			if(!indexRepresentable) continue;
			record.stableOrdinals.push_back(
				static_cast<std::uint32_t>(current->second));
			record.selectedLods.push_back(command.selectedVirtualLod);
			record.routeSelectors.push_back(
				static_cast<std::uint32_t>(command.geometryRoute));
			record.editorTints.push_back(command.editorTint);
			recordCommandLanes.push_back(MakeRenderViewCommandLane(command));
			const bool contributesVirtualGeometry =
				(command.virtualGeometryFlags & (kVirtualGeometryFlagActive
					| kVirtualGeometryFlagFailOpen)) != 0U
				|| command.virtualMeshletCount != 0U;
			if(contributesVirtualGeometry
				&& record.stableOrdinals.size() - 1u
					<= std::numeric_limits<std::uint32_t>::max()) {
				record.virtualGeometryCommandIndices.push_back(
					static_cast<std::uint32_t>(
						record.stableOrdinals.size() - 1u));
			}
		}
		auto retainedSelectionEvidence = std::make_shared<TerraEngine::Renderer::
			VirtualGeometryRetainedSelectionEvidence>();
		retainedSelectionEvidence->selectedClusters = result.selectedClusters;
		retainedSelectionEvidence->pageNeeds = result.pageNeeds;
		retainedSelectionEvidence->clusterPackets = result.clusterPackets;
		retainedSelectionEvidence->clusterBatches = result.clusterBatches;
		auto retainedGpuEvidence =
			std::make_shared<VirtualGeometryRetainedGpuEvidence>();
		retainedGpuEvidence->packetRecords = result.gpuPacketRecords;
		retainedGpuEvidence->batchDescriptors = result.gpuBatchDescriptors;
		retainedGpuEvidence->packetFrameSets = result.gpuPacketFrameSets;
		retainedGpuEvidence->compactionDispatches = result.gpuCompactionDispatches;
		retainedGpuEvidence->generatedSubmissions = result.generatedSubmissions;
		retainedGpuEvidence->counterReadbacks = result.gpuCounterReadbacks;
		retainedGpuEvidence->fallbackRecords = result.gpuFallbackRecords;
		record.retainedGpuEvidence = std::move(retainedGpuEvidence);
		retainedSelectionEvidence->compactionResults =
			result.compactionResults;
		retainedSelectionEvidence->fallbackCoverage =
			result.fallbackCoverage;
		retainedSelectionEvidence->lodTransitionStates =
			result.lodTransitionStates;
		if(!renderFrameSnapshot.virtualGeometryViewSelections.empty())
		{
			const TerraEngine::Renderer::VirtualGeometryViewSelectionState& viewEvidence =
				renderFrameSnapshot.virtualGeometryViewSelections.back();
			retainedSelectionEvidence->pageRequests = viewEvidence.PageRequests();
			retainedSelectionEvidence->residentPagePayloads =
				viewEvidence.ResidentPagePayloads();
			retainedSelectionEvidence->emittedPageSources =
				viewEvidence.EmittedPageSources();
			record.viewDraws = std::make_shared<const std::vector<
				TerraEngine::Renderer::VirtualGeometryViewDrawRecord>>(
					viewEvidence.DrawRecords());
			record.hasViewDrawEvidence = true;
		}
		record.commandLanes = std::make_shared<const std::vector<
			RenderViewCommandLane>>(std::move(recordCommandLanes));
		record.retainedSelectionEvidence = std::move(retainedSelectionEvidence);
		record.streamingTouchSet = result.streamingTouchSet;
		record.terrainTextureTouches = result.terrainTextureTouches;
		record.reuseTelemetry = result.reuseTelemetry;
		record.diagnostics = result.diagnostics;
		visibleCommandCache.recordsBySignature[signature] = std::move(record);
		TrimSignatureCache(visibleCommandCache, 32u);
		++visibleCommandCache.missCount;
		return result;
	};

	// Pack reflected lights in parallel so large scenes keep the CPU upload path
	// scalable while preserving deterministic ordering and stable light IDs.
	std::vector<FrameLightCandidate> packedLights;
	if (!lights.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Pack Lights", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		LightCandidateCacheState& lightCandidateCache = lifetimeState->lightCandidateCache;
		lightCandidateCache.hitCount = 0u;
		lightCandidateCache.missCount = 0u;
		std::vector<std::uint64_t> lightCandidateSignatures(lights.size(), 0u);
		std::unordered_set<TerraEngine::UUID> aliveLightIds;
		aliveLightIds.reserve(lights.size());

		auto makeLightCandidate = [&](const LightEntry& entry,
			const TerraEngine::Scene::WorldPose& transform) {
			const LightComponent& light = entry.light;
			const glm::quat normalizedRotation = glm::normalize(transform.rotation);
			glm::vec3 direction = normalizedRotation * glm::vec3(0.0f, 0.0f, -1.0f);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) || glm::length(direction) < 1e-5f)
			{
				direction = glm::vec3(0.0f, -1.0f, 0.0f);
			}
			else
			{
				direction = glm::normalize(direction);
			}

			glm::vec3 finalColor = light.color;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.appearance == LightAppearance::FilterAndTemperature)
			{
				finalColor *= LightComponent::KelvinToRGB(light.temperature);
			}

			float clampedIntensity = std::max(light.intensity, 0.0f);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.type != LightType::Directional && light.unit == LightUnit::Lumens)
			{
				clampedIntensity /= (4.0f * 3.14159265f); // Convert Lumens to Candela
			}

			const float clampedRange = std::max(light.range, 0.001f);
			const float clampedInnerAngle = std::clamp(light.innerConeAngle, 0.0f, 1.56f);
			const float clampedOuterAngle = std::clamp(std::max(light.outerConeAngle, clampedInnerAngle + 0.001f), 0.0f, 1.57f);

			GPULight gpuLight{};
			// Translation is localized per view after light selection. Shared light
			// candidates never store an absolute float position.
			gpuLight.positionAndType = glm::vec4(
				glm::vec3(0.0F),
				static_cast<float>(static_cast<std::uint32_t>(light.type)));
			gpuLight.directionAndRange = glm::vec4(direction, clampedRange);
			gpuLight.colorAndIntensity = glm::vec4(
				std::max(finalColor.x, 0.0f),
				std::max(finalColor.y, 0.0f),
				std::max(finalColor.z, 0.0f),
				clampedIntensity);
			gpuLight.coneAnglesAndArea = glm::vec4(
				std::cos(clampedInnerAngle),
				std::cos(clampedOuterAngle),
				std::max(light.width, 0.001f),
				std::max(light.height, 0.001f));
			gpuLight.shadowAndPadding = glm::vec4(light.castsShadows ? 1.0f : 0.0f, -1.0f, 0.0f, 0.0f);
			gpuLight.extraParams = glm::vec4(
				std::clamp(light.shadowStrength, 0.0f, 1.0f),
				std::max(light.shadowBias, 0.0f),
				std::max(light.indirectMultiplier, 0.0f),
				light.authoredPriority);

			FrameLightCandidate candidate{};
			candidate.lightId = entry.lightId;
			candidate.canonicalPosition = transform.position;
			candidate.gpuLight = gpuLight;
			candidate.stabilityPreference = std::max(light.stabilityPreference, 0.0f);
			candidate.candidateFlags = 0u;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.castsShadows)
			{
				candidate.candidateFlags |= kFrameLightCandidateFlagShadowCapable;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.type == LightType::Area)
			{
				candidate.candidateFlags |= kFrameLightCandidateFlagSpecialCost;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.type == LightType::Directional)
			{
				candidate.candidateFlags |= kFrameLightCandidateFlagDirectional;
			}
			return candidate;
		};

		RunRenderChunkedWork(
			lights.size(),
			renderGraphWorkerCount,
			kRenderParallelThreshold,
			"RenderSystem Task: Hash Light Chunk",
			[&](std::size_t start, std::size_t end)
			{
				const auto observeRange = [&](TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint checkpoint) {
					if(!scratchValidation || !scratchValidation->onEvent) return;
					TerraEngine::Renderer::RenderFrameScratchValidationEvent event;
					event.checkpoint = checkpoint;
					event.episode = capturedScratchDiagnostics;
					event.start = start; event.end = end;
					event.workerLimit = renderGraphWorkerCount;
					event.rangeDigest = 14695981039346656037ULL;
					TerraEngine::Renderer::AppendScratchValidationWord(event.rangeDigest, start, 8U);
					TerraEngine::Renderer::AppendScratchValidationWord(event.rangeDigest, end, 8U);
					event.computedSignatureDigest = 14695981039346656037ULL;
					for(std::size_t i = start; i < end; ++i) {
						TerraEngine::Renderer::AppendScratchLightValidationDigest(event.rangeDigest, lights[i].lightId, lights[i].light);
						if(checkpoint == TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint::LightReadEnd)
							TerraEngine::Renderer::AppendScratchValidationWord(event.computedSignatureDigest, lightCandidateSignatures[i], 8U);
					}
					scratchValidation->onEvent(event);
				};
				observeRange(TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint::LightReadBegin);
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (std::size_t i = start; i < end; ++i)
				{
					const LightEntry& entry = lights[i];
					lightCandidateSignatures[i] = BuildLightCandidateSignature(getWorldTransform(entry.entity), entry.light);
				}
				observeRange(TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint::LightReadEnd);
			});
		packedLights.reserve(lights.size());
		for (std::size_t i = 0u; i < lights.size(); ++i)
		{
			const LightEntry& entry = lights[i];
			aliveLightIds.insert(entry.lightId);
			const std::uint64_t signature = lightCandidateSignatures[i];
			auto cacheEntry = lightCandidateCache.recordsByLightId.find(entry.lightId);
			if (cacheEntry != lightCandidateCache.recordsByLightId.end() && cacheEntry->second.signature == signature)
			{
				cacheEntry->second.lastTouchedFrameId = activeRenderFrameId;
				packedLights.push_back(cacheEntry->second.candidate);
				++lightCandidateCache.hitCount;
				continue;
			}

			CachedLightCandidateRecord record{};
			record.signature = signature;
			record.lastTouchedFrameId = activeRenderFrameId;
			record.candidate = makeLightCandidate(entry, getWorldTransform(entry.entity));
			packedLights.push_back(record.candidate);
			lightCandidateCache.recordsByLightId[entry.lightId] = std::move(record);
			++lightCandidateCache.missCount;
		}
		PruneStaleLightCandidateCache(lightCandidateCache, aliveLightIds);
		if (TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			renderGraphDiagnostics.backendFallbacks.push_back(
				"Light candidate cache hits: "
				+ std::to_string(lightCandidateCache.hitCount)
				+ "/"
				+ std::to_string(lightCandidateCache.hitCount + lightCandidateCache.missCount));
		}
	}

	std::vector<const RenderItemRecord*> renderableDrawCommandsByOrdinal(
		retainedRenderTopology.topology.records.size(), nullptr);
	for(const RenderItemRecord& command : renderableDrawCommands)
	{
		if(command.conventionalVisibilityIdentityIndex == 0u) continue;
		const std::uint32_t ordinal = command.conventionalVisibilityIdentityIndex - 1u;
		if(ordinal < renderableDrawCommandsByOrdinal.size())
			renderableDrawCommandsByOrdinal[ordinal] = &command;
	}
	const auto compactShadowDrawList = [&](TerraEngine::UUID lightId,
		const std::vector<RenderItemRecord>& commands)
	{
		ShadowDrawList result{};
		result.lightId = lightId;
		result.topologyGeneration = std::max<std::uint64_t>(
			1u, retainedRenderTopology.topology.generation);
		result.stableOrdinals.reserve(commands.size());
		result.selectedLods.reserve(commands.size());
		result.routeSelectors.reserve(commands.size());
		result.editorTints.reserve(commands.size());
		result.commandLanes.reserve(commands.size());
		for(const RenderItemRecord& command : commands)
		{
			if(command.conventionalVisibilityIdentityIndex == 0u)
				continue;
			const std::uint32_t ordinal =
				command.conventionalVisibilityIdentityIndex - 1u;
			if(ordinal >= retainedRenderTopology.topology.records.size()
				|| !retainedRenderTopology.topology.records[ordinal].groupKey.IsComplete())
				continue;
			result.stableOrdinals.push_back(ordinal);
			result.selectedLods.push_back(command.selectedVirtualLod);
			result.routeSelectors.push_back(
				static_cast<std::uint32_t>(command.geometryRoute));
			result.editorTints.push_back(command.editorTint);
			result.commandLanes.push_back(MakeRenderViewCommandLane(command));
		}
		return result;
	};
	const auto materializeShadowDrawList = [&](const ShadowDrawList& shadow)
	{
		std::vector<RenderItemRecord> commands;
		if(!shadow.HasParallelLengths()) return commands;
		commands.reserve(shadow.stableOrdinals.size());
		for(std::size_t itemIndex = 0u;
			itemIndex < shadow.stableOrdinals.size(); ++itemIndex)
		{
			const std::uint32_t ordinal = shadow.stableOrdinals[itemIndex];
			if(ordinal >= renderableDrawCommandsByOrdinal.size()
				|| renderableDrawCommandsByOrdinal[ordinal] == nullptr) continue;
			RenderItemRecord command = *renderableDrawCommandsByOrdinal[ordinal];
			ApplyRenderViewCommandLane(shadow.commandLanes[itemIndex],
				shadow.selectedLods[itemIndex],
				static_cast<TerraEngine::Renderer::GeometryRoute>(
					shadow.routeSelectors[itemIndex]),
				shadow.editorTints[itemIndex], command);
			commands.push_back(std::move(command));
		}
		return commands;
	};

	auto buildShadowDrawLists = [&](std::span<const FrameLightCandidate> ownedViewLights,
		const glm::vec3& viewCameraLocalPosition, const TerraEngine::Renderer::RenderReferenceFrame& viewReferenceFrame,
		RenderViewType viewType = RenderViewType::Scene, std::uint32_t outputId = 0U) {
		// Per-view frame storage owns these candidates after the producer vector moves.
		// Chunk jobs settle before this borrowed span leaves the protected builder.
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Shadow Draw Lists Cache Lookup", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Spatial queries use the index's own frame, regardless of a view rebase.
		const auto indexedFrame = spatialIndex.GetReferenceFrame();
		TerraEngine::Scene::WorldDelta cameraDelta;
		cameraDelta.offsetMetres = glm::dvec3(viewCameraLocalPosition);
		const auto canonicalCamera = TerraEngine::Scene::ApplyWorldDelta(viewReferenceFrame.canonicalOrigin, cameraDelta);
		const auto localizedCamera = canonicalCamera.Succeeded()
			? TerraEngine::Scene::LocalizeWorldPosition(canonicalCamera.position, indexedFrame.canonicalOrigin, indexedFrame.maximumLocalDistanceMetres)
			: TerraEngine::Scene::WorldLocalizationResult{{}, canonicalCamera.error};
		const glm::vec3 cameraLocalPosition = localizedCamera.Succeeded() ? glm::vec3(localizedCamera.offsetMetres) : glm::vec3(0);
		std::vector<FrameLightCandidate> queryLights(ownedViewLights.begin(), ownedViewLights.end());
		std::vector<bool> queryLightLocalized(queryLights.size(), false);
		for (std::size_t i = 0; i < queryLights.size(); ++i) {
			const auto localized = TerraEngine::Scene::LocalizeWorldPosition(queryLights[i].canonicalPosition,
				indexedFrame.canonicalOrigin, indexedFrame.maximumLocalDistanceMetres);
			queryLightLocalized[i] = localized.Succeeded();
			queryLights[i].gpuLight.positionAndType = glm::vec4(localized.Succeeded() ? glm::vec3(localized.offsetMetres) : glm::vec3(0),
				queryLights[i].gpuLight.positionAndType.w);
		}
		std::uint64_t signature = BuildShadowDrawListCacheKey(
			renderFrameSnapshot.sceneRevision,
			renderPreparationSettingsSignature,
			renderableSubmissionSignature,
			BuildLightSetSignature(queryLights),
			cameraLocalPosition);
		// Fixture controls change actual selector bytes, so old compact lists must
		// not bypass their application. The generation is stable across step frames.
		HashCombine(signature, renderFrameSnapshot.efficiencyFixtureIntent.controlGeneration);
		HashCombine(signature, indexedFrame.generation);
		HashCombine(signature, std::hash<double>{}(indexedFrame.maximumLocalDistanceMetres));
		HashCombine(signature, TerraEngine::Scene::WorldPositionHash{}(indexedFrame.canonicalOrigin));
		HashCombine(signature, localizedCamera.Succeeded() ? 1U : 0U);
		for (const bool known : queryLightLocalized) HashCombine(signature, known ? 1U : 0U);
		const auto* casterSelection = TerraEngine::Renderer::ResolveRendererEfficiencyFixtureCasterSelection(
			renderFrameSnapshot.efficiencyFixtureIntent, viewType, outputId);
		// View-qualified membership is stable through this captured job batch.
		// Hash every UUID byte so equal camera positions cannot share different
		// controlled caster lists, or accidentally reuse an uncontrolled list.
		HashCombine(signature, static_cast<std::uint32_t>(viewType));
		HashCombine(signature, outputId);
		HashCombine(signature, casterSelection ? casterSelection->objectIds.size() : 0U);
		if (casterSelection) for (const auto& id : casterSelection->objectIds)
			for (const auto byte : id) HashCombine(signature, byte);
		if (const auto existing = shadowDrawListCache.recordsBySignature.find(signature); existing != shadowDrawListCache.recordsBySignature.end()
			&& existing->second.controlledViewType == viewType && existing->second.controlledOutputId == outputId
			&& existing->second.fixtureControlGeneration == renderFrameSnapshot.efficiencyFixtureIntent.controlGeneration
			&& existing->second.controlledCasterSelection == (casterSelection != nullptr)
			&& (casterSelection ? existing->second.controlledCasterIds == casterSelection->objectIds : existing->second.controlledCasterIds.empty()))
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			++shadowDrawListCache.hitCount;
			existing->second.lastTouchedFrameId = activeRenderFrameId;
			std::vector<ShadowDrawList> cachedShadowDrawLists;
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Copy Cached Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			cachedShadowDrawLists = existing->second.shadowDrawLists;
			}
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Shadow VG Selection Stats", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			for (std::size_t shadowIndex = 0u; shadowIndex < cachedShadowDrawLists.size(); ++shadowIndex)
			{
				const std::vector<RenderItemRecord> diagnosticCommands =
					materializeShadowDrawList(cachedShadowDrawLists[shadowIndex]);
				const auto& intent = renderFrameSnapshot.efficiencyFixtureIntent;
				if (intent.controlsApplied && intent.shadowSelection)
				{
					const auto proof = std::find_if(fixtureTargets.begin(), fixtureTargets.end(), [&](const auto& row) { return row.objectId == intent.shadowSelection->objectId; });
					if (proof != fixtureTargets.end()) for (const auto& actual : diagnosticCommands)
					{
						auto expected = actual;
						if (TerraEngine::Renderer::ApplyRendererEfficiencyFixtureSelection(expected, *intent.shadowSelection, *proof)
							&& expected.selectedVirtualLod == actual.selectedVirtualLod
							&& expected.virtualMeshletOffset == actual.virtualMeshletOffset
							&& expected.virtualMeshletCount == actual.virtualMeshletCount)
							fixtureShadowSelectionApplied.store(true, std::memory_order_relaxed);
					}
				}
				collectVirtualGeometryDiagnosticsAssets(
					diagnosticCommands, RenderViewType::Shadow);
				recordVirtualGeometryViewSelection(
					diagnosticCommands,
					{},
					{},
					{},
					{},
					{},
					{},
					{},
					VirtualGeometryDebugVisualizationMode::None,
					RenderOutputKind::OffscreenTexture,
					RenderViewType::Shadow,
					glm::uvec2(renderFrameSnapshot.renderSettings.shadowResolution, renderFrameSnapshot.renderSettings.shadowResolution),
					static_cast<std::uint32_t>(1000u + shadowIndex),
					true,
					false);
			}
			return cachedShadowDrawLists;
		}

		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Rebuild Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::vector<std::optional<ShadowDrawList>> shadowDrawListsByLight(ownedViewLights.size());
		TerraEngine::Scene::SpatialQueryFilter filter{};
		filter.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;

		RunRenderChunkedWork(
			ownedViewLights.size(),
			renderGraphWorkerCount,
			kRenderParallelThreshold,
			"RenderSystem Task: Build Shadow Draw Lists",
			[&](std::size_t start, std::size_t end)
			{
				for (std::size_t lightIndex = start; lightIndex < end; ++lightIndex)
				{
					const FrameLightCandidate& candidate = queryLights[lightIndex];
					if ((candidate.candidateFlags & kFrameLightCandidateFlagShadowCapable) == 0u)
					{
						continue;
					}

					ShadowDrawList shadowDrawList{};
					shadowDrawList.lightId = candidate.lightId;

					const std::uint32_t lightType = static_cast<std::uint32_t>(std::max(candidate.gpuLight.positionAndType.w, 0.0f) + 0.5f);
					TerraEngine::Scene::SpatialQueryResult query{};
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					const bool queryReady = lightType == static_cast<std::uint32_t>(LightType::Directional)
					? localizedCamera.Succeeded() : queryLightLocalized[lightIndex];
					if (!queryReady) {
						// An unrepresentable query must retain complete immutable caster coverage.
					}
					else if (lightType == static_cast<std::uint32_t>(LightType::Directional))
					{
						const glm::mat4 lightViewProjection = BuildDirectionalShadowViewProjection(candidate.gpuLight, cameraLocalPosition);
						query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (lightType == static_cast<std::uint32_t>(LightType::Point))
					{
						TerraEngine::Scene::SpatialSphere lightBounds{};
						lightBounds.center = glm::vec3(candidate.gpuLight.positionAndType);
						lightBounds.radius = std::max(candidate.gpuLight.directionAndRange.w, 0.5f);
						query = spatialIndex.QuerySphere(lightBounds, filter);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (lightType == static_cast<std::uint32_t>(LightType::Spot))
					{
						const glm::mat4 lightViewProjection = BuildSpotShadowViewProjection(candidate.gpuLight);
						query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (lightType == static_cast<std::uint32_t>(LightType::Area))
					{
						const glm::mat4 lightViewProjection = BuildAreaShadowViewProjection(candidate.gpuLight);
						query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
					}

					std::vector<RenderItemRecord> shadowCommands =
						queryReady ? buildDrawCommandsFromCandidateIndices(collectCandidateIndices(query)) : renderableDrawCommands;
					applyShadowVirtualGeometrySelection(shadowCommands);
					TerraEngine::Renderer::FilterRendererEfficiencyFixtureShadowCasters(shadowCommands, casterSelection);
					shadowDrawList = compactShadowDrawList(
						candidate.lightId, shadowCommands);
					shadowDrawListsByLight[lightIndex] = std::move(shadowDrawList);
				}
			});

		std::vector<ShadowDrawList> shadowDrawLists;
		shadowDrawLists.reserve(shadowDrawListsByLight.size());
		for (std::optional<ShadowDrawList>& shadowDrawList : shadowDrawListsByLight)
		{
			if (shadowDrawList.has_value())
			{
				shadowDrawLists.push_back(std::move(*shadowDrawList));
			}
		}
		for (std::size_t shadowIndex = 0u; shadowIndex < shadowDrawLists.size(); ++shadowIndex)
		{
			const std::vector<RenderItemRecord> diagnosticCommands =
				materializeShadowDrawList(shadowDrawLists[shadowIndex]);
			recordVirtualGeometryViewSelection(
				diagnosticCommands,
				{},
				{},
				{},
				{},
				{},
				{},
				{},
				VirtualGeometryDebugVisualizationMode::None,
				RenderOutputKind::OffscreenTexture,
				RenderViewType::Shadow,
				glm::uvec2(renderFrameSnapshot.renderSettings.shadowResolution, renderFrameSnapshot.renderSettings.shadowResolution),
				static_cast<std::uint32_t>(1000u + shadowIndex),
				true,
				false);
		}

		ShadowDrawListCacheRecord record;
		record.signature = signature;
		// The digest narrows lookup; exact qualified membership still authorizes
		// cache reuse, so collisions cannot substitute another view's caster set.
		record.controlledViewType = viewType; record.controlledOutputId = outputId;
		record.fixtureControlGeneration = renderFrameSnapshot.efficiencyFixtureIntent.controlGeneration;
		record.controlledCasterSelection = casterSelection != nullptr;
		if (casterSelection) record.controlledCasterIds = casterSelection->objectIds;
		record.lastTouchedFrameId = activeRenderFrameId;
		record.shadowDrawLists = shadowDrawLists;
		shadowDrawListCache.recordsBySignature[signature] = std::move(record);
		TrimSignatureCache(shadowDrawListCache, 32u);
		++shadowDrawListCache.missCount;
		return shadowDrawLists;
	};

	SkyRuntimeState skyRuntimeState{};
	const SkyComponent* activeSkyComponent = capturedSkyComponent.has_value()
		? std::addressof(*capturedSkyComponent) : nullptr;
	const LightEntry* activeSkyDirectionalLight = nullptr;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Resolve Sky State", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const bool hasActiveSkyComponent = activeSkyComponent != nullptr;
		skyRuntimeState.isValid = true;
		skyRuntimeState.usingFallback = true;
		skyRuntimeState.hasProblem = false;
		skyRuntimeState.dirty = skyStateDirty;
		skyRuntimeState.forceBlackFallback = !hasActiveSkyComponent;
		std::string skyIssueMessage;

		const TerraEngine::UUID skyObjectId = capturedSkyObjectId;
		const LightEntry* skyObjectDirectionalLight = nullptr;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const LightEntry& lightEntry : lights)
		{
			if (lightEntry.lightId != skyObjectId)
			{
				continue;
			}
			if (lightEntry.light.type == LightType::Directional)
			{
				skyObjectDirectionalLight = &lightEntry;
			}
			break;
		}

		glm::vec3 baseSunDirection = kSkyReferenceSunDirection;
		const LightEntry* resolvedDirectionalLight = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hasActiveSkyComponent)
		{
			resolvedDirectionalLight = skyObjectDirectionalLight;
			activeSkyDirectionalLight = resolvedDirectionalLight;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (resolvedDirectionalLight == nullptr)
			{
				skyRuntimeState.forceBlackFallback = true;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (resolvedDirectionalLight != nullptr)
		{
			if (const auto lightTransform = worldTransforms->find(resolvedDirectionalLight->entity); lightTransform != worldTransforms->end())
			{
				const glm::quat lightRotation = glm::normalize(lightTransform->second.rotation);
				const glm::vec3 lightForward = lightRotation * glm::vec3(0.0f, 0.0f, -1.0f);
				baseSunDirection = SafeNormalizeDirection(-lightForward, baseSunDirection);
			}
		}

		const float skyRotationOffset = activeSkyComponent != nullptr ? ClampSkyRotationOffset(activeSkyComponent->rotationOffsetDegrees) : 0.0f;
		skyRuntimeState.resolvedSunDirection = ApplySkyYawOffset(baseSunDirection, skyRotationOffset);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hasActiveSkyComponent && !skyRuntimeState.forceBlackFallback && !activeSkyComponent->sourceAsset.empty())
		{
			const TerraEngine::UUID resolvedTextureId = capturedSkyTextureId;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (resolvedTextureId == TerraEngine::EMPTY_UUID)
			{
				skyRuntimeState.hasProblem = true;
				skyIssueMessage = "Scene sky source could not be resolved. Rendering the fallback background.";
			}
			else
			{
				skyRuntimeState.resolvedSkyHandle = resolvedTextureId;
				skyRuntimeState.usingFallback = false;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skyRuntimeState.hasProblem)
		{
			const std::string sourceAsset = activeSkyComponent != nullptr ? activeSkyComponent->sourceAsset : std::string();
			const std::string issueKey = sourceAsset + "|" + skyIssueMessage;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (issueKey != lastLoggedSkyIssueKey)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "[Sky] " << skyIssueMessage << " Source: " << sourceAsset);
				lastLoggedSkyIssueKey = issueKey;
			}
		}
		else
		{
			lastLoggedSkyIssueKey.clear();
		}
	}

	RenderViewBuildState frameData;
	frameData.frameId = activeRenderFrameId;
	frameData.deltaTimeSeconds = deltaTime;
	frameData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
	frameData.cullingEpsilon = 0.001f;
	frameData.activePreset = currentPreset;
	frameData.customOverrideActive = (currentPreset == QualityPreset::Custom);
	frameData.settings = SanitizeRenderSettings(currentSettings);
	// Reorder only owned captured candidates and copied settings. The backend's
	// ordinary ApplyFrameLightBudgets consumes these genuine inputs for each view.
	if (renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied
		&& !TerraEngine::Renderer::ApplyRendererEfficiencyFixtureLightingControls(renderFrameSnapshot.efficiencyFixtureIntent,
			packedLights, frameData.settings, renderFrameSnapshot.efficiencyFixtureIntent.controlReason))
		renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied = false;
	frameData.efficiencyOptions = renderFrameSnapshot.efficiencyOptions;
	frameData.efficiencyFixtureIntent = renderFrameSnapshot.efficiencyFixtureIntent;
	frameData.sky.renderBackground = true;
	frameData.sky.hasTexture = skyRuntimeState.resolvedSkyHandle != TerraEngine::EMPTY_UUID && !skyRuntimeState.usingFallback;
	frameData.sky.usingFallback = skyRuntimeState.usingFallback;
	frameData.sky.forceBlack = skyRuntimeState.forceBlackFallback;
	frameData.sky.intensity = activeSkyDirectionalLight != nullptr ? std::max(activeSkyDirectionalLight->light.intensity, 0.0f) : 1.0f;
	frameData.sky.exposure = activeSkyComponent != nullptr ? ClampSkyExposure(activeSkyComponent->exposure) : 1.0f;
	frameData.sky.rotationOffsetDegrees = activeSkyComponent != nullptr ? ClampSkyRotationOffset(activeSkyComponent->rotationOffsetDegrees) : 0.0f;
	frameData.sky.horizonTint = activeSkyComponent != nullptr ? ClampSkyTint(activeSkyComponent->horizonTint) : frameData.sky.horizonTint;
	frameData.sky.zenithTint = activeSkyComponent != nullptr ? ClampSkyTint(activeSkyComponent->zenithTint) : frameData.sky.zenithTint;
	frameData.sky.groundTint = activeSkyComponent != nullptr ? ClampSkyTint(activeSkyComponent->groundTint) : frameData.sky.groundTint;
	frameData.sky.sunDirection = skyRuntimeState.resolvedSunDirection;
	frameData.sky.textureId = skyRuntimeState.resolvedSkyHandle;
	frameData.lightCandidates = std::move(packedLights);
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Fetch UI Batch", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		frameData.uiBatch = capturedUiBatch;
	}
	{
		// StreamingPrepare completed before this render workload. Copy only
		// integrity-checked decoded values into the immutable frame payload;
		// Vulkan still must upload them and observe its frame fence before the
		// protected streaming system may publish Resident state.
		for (const TerraEngine::Resource::VirtualGeometryStreamingDecodedPage& decoded
			: capturedVirtualGeometryStreamingPlan.decodedPages)
		{
			if (decoded.status != TerraEngine::Resource::VirtualGeometryStreamingDecodeStatus::PendingUpload
				|| decoded.decodedBytes.empty())
			{
				continue;
			}

			VirtualGeometryDecodedPageUploadRecord upload{};
			upload.assetId = decoded.request.assetId;
			upload.pageId = decoded.request.pageId;
			upload.fallbackPageId = decoded.request.fallbackPageId;
			upload.clusterId = decoded.request.clusterId;
			upload.dependencyPageIds = decoded.request.dependencyPageIds;
			upload.publicationGeneration = std::max<std::uint64_t>(activeRenderFrameId, 1u);
			// Exact canonical page totals are mandatory before PendingUpload, so
			// the immutable backend record never substitutes decoded bytes for a
			// missing stored-byte count.
			upload.storedByteCount = decoded.request.expectedStoredByteCount;
			upload.decodedByteCount = decoded.decodedBytes.size();
			upload.integrityValue = decoded.request.expectedIntegrityValue;
			upload.decodedBytes = decoded.decodedBytes;
			frameData.virtualGeometryDecodedPageUploads.push_back(std::move(upload));
		}
	}

	// Boundary 2: shared route, spatial, light, upload, and frame-header
	// preparation is complete. Per-view visibility and publication sealing are a
	// separate worker stage so both costs remain independently diagnosable.
	co_await std::suspend_always{};

	TerraEngine::Scene::SpatialDiagnosticsSnapshot sceneSpatialDiagnostics{};
	TerraEngine::Scene::SpatialDiagnosticsSnapshot gameSpatialDiagnostics{};
	TerraEngine::Renderer::VisibleCommandReuseTelemetry sceneVisibleCommandTelemetry{};
	TerraEngine::Renderer::VisibleCommandReuseTelemetry gameVisibleCommandTelemetry{};
	// Convert each camera submission into an explicit output owner so the RHI
	// can give Scene, Game, and future offscreen cameras independent snapshot
	// ranges instead of inferring ownership from call order alone.
	auto makeOutputRequest = [&](RenderOutputKind kind, RenderViewType diagnosticsView, std::uint32_t outputId) {
		RenderOutputRequest request{};
		request.kind = kind;
		request.priority = RenderOutputPriority::Required;
		request.diagnosticsView = diagnosticsView;
		// Normal editor targets follow panel dimensions; sealed fixture targets
		// retain their actual native extent through query, submission and readback.
		if (kind == RenderOutputKind::SceneView)
		{
			request.targetSize = effectiveSceneTargetSize;
		}
		else if (kind == RenderOutputKind::GameView || kind == RenderOutputKind::MainRuntimeView)
		{
			request.targetSize = effectiveGameTargetSize;
		}
		request.outputId = outputId;
		return request;
	};
	struct PreparedRenderSubmission
	{
		// Commands and RenderViewBuildState are temporary worker scratch. Queueing
		// lowers commands into compact selectors and transfers every owned vector
		// directly into this immutable presentation. Keeping a second frame copy here
		// would duplicate the complete grouped-instance stream for every camera.
		RenderOutputRequest output;
		TerraEngine::Renderer::RenderViewSelection selection;
		TerraEngine::Renderer::RenderViewPresentationInput presentation;
	};
	const auto makePresentationInput = [](RenderViewBuildState&& frame,
		const RenderOutputRequest& output) {
		// Transfer only presentation state and backend-neutral evidence. Command
		// materialization intentionally stays out of this new contract: the
		// compact selection and typed submission ranges below describe geometry.
		// The caller has finished the view build, so moving its vectors preserves
		// single ownership and avoids a crowd-sized copy at the queue boundary.
		TerraEngine::Renderer::RenderViewPresentationInput input{};
		input.output = output;
		input.referenceFrame = frame.referenceFrame;
		input.viewMatrix = frame.viewMatrix;
		input.projectionMatrix = frame.projectionMatrix;
		input.cameraLocalPosition = frame.cameraLocalPosition;
		input.deltaTimeSeconds = frame.deltaTimeSeconds;
		input.globalTaskShaderCullingEnabled = frame.globalTaskShaderCullingEnabled;
		input.cullingEpsilon = frame.cullingEpsilon;
		input.activePreset = frame.activePreset;
		input.customOverrideActive = frame.customOverrideActive;
		input.settings = frame.settings;
		input.efficiencyOptions = frame.efficiencyOptions;
		input.efficiencyFixtureIntent = frame.efficiencyFixtureIntent;
		if (input.efficiencyFixtureIntent.controlsApplied && input.efficiencyFixtureIntent.depthControls
			&& input.efficiencyFixtureIntent.depthControls->oracleRecipe) {
			// This conversion already runs in the view preparation job (or the
			// explicitly requested inline reference). Only immutable owned controls
			// are read; no GPU mapping or live scene mutation happens on the worker.
			const auto& recipe=*input.efficiencyFixtureIntent.depthControls->oracleRecipe;
			auto pixels=std::make_shared<std::vector<std::uint32_t>>(std::size_t(recipe.extent.width)*recipe.extent.height);
			if (TerraEngine::Renderer::LightingDepthReduction::MaterializeDepthRecipe(recipe,*pixels))
				input.efficiencyDepthOracleInput=std::move(pixels);
			else {
				input.efficiencyFixtureIntent.controlsApplied=false;
				input.efficiencyFixtureIntent.controlReason="Captured isolated depth recipe could not materialize atomically.";
			}
		}
		input.renderOptimization = frame.renderOptimization;
		input.groupedInstanceRecords = std::move(frame.groupedInstanceRecords);
		input.groupedSubmissions = std::move(frame.groupedSubmissions);
		input.drawResourcePreparationCommandIndices =
			std::move(frame.drawResourcePreparationCommandIndices);
		input.sky = frame.sky;
		input.lightCandidates = std::move(frame.lightCandidates);
		input.editorContextTint = frame.editorContextTint;
		input.sceneViewGridVisible = frame.sceneViewGridVisible;
		input.sceneViewOrthographic = frame.sceneViewOrthographic;
		input.shadowDrawLists = std::move(frame.shadowDrawLists);
		input.uiBatch = std::move(frame.uiBatch);
		input.virtualGeometryPacketEvidenceSignature =
			frame.virtualGeometryPacketEvidenceSignature;
		input.virtualGeometryVisibilitySetupSignature =
			frame.virtualGeometryVisibilitySetupSignature;
		input.retainedVirtualGeometryGpuEvidence =
			std::move(frame.retainedVirtualGeometryGpuEvidence);
		if(input.retainedVirtualGeometryGpuEvidence == nullptr)
		{
			// Standalone frame data owns its packet vectors. Transfer them in this
			// path; cached evidence remains shared through the immutable root.
			input.virtualGeometryPackets = std::move(frame.virtualGeometryGpuPackets);
			input.virtualGeometryBatches = std::move(frame.virtualGeometryGpuBatches);
			input.virtualGeometryFrameSets = std::move(frame.virtualGeometryGpuFrameSets);
			input.virtualGeometryCompactionDispatches =
				std::move(frame.virtualGeometryGpuCompactionDispatches);
			input.virtualGeometryGeneratedSubmissions =
				std::move(frame.virtualGeometryGeneratedSubmissions);
			input.virtualGeometryCounterReadbacks =
				std::move(frame.virtualGeometryCounterReadbacks);
			input.virtualGeometryFallbackRecords =
				std::move(frame.virtualGeometryGpuFallbackRecords);
		}
		input.virtualGeometryVisibilityDiagnosticsRequested =
			frame.virtualGeometryVisibilityDiagnosticsRequested;
		input.virtualGeometryPageRetirements =
			std::move(frame.virtualGeometryPageRetirements);
		input.virtualGeometryDecodedPageUploads =
			std::move(frame.virtualGeometryDecodedPageUploads);
		return input;
	};
	std::vector<PreparedRenderSubmission> preparedRenderSubmissions;
	preparedRenderSubmissions.reserve(
		static_cast<std::size_t>(hasSceneViewCamera ? 1u : 0u)
		+ gameViewCameras.size()
		+ cameras.size()
		+ captureLimits.maxConcurrentCaptures);
	auto queuePreparedSubmission = [&](RenderViewBuildState&& preparedFrame,
		RenderOutputRequest output,
		const std::vector<RenderItemRecord>& commands,
		const VisibleCommandCacheRecord* cachedRecord)
	{
		PreparedRenderSubmission prepared{};
		prepared.output = std::move(output);
		prepared.selection.outputKind = prepared.output.kind;
		prepared.selection.viewType = prepared.output.diagnosticsView;
		prepared.selection.outputId = prepared.output.outputId;
		prepared.selection.generation = std::max<std::uint64_t>(
			1u, activeRenderFrameId);
		prepared.presentation = makePresentationInput(
			std::move(preparedFrame), prepared.output);
		const bool reuseCachedViewLanes = cachedRecord != nullptr
			&& prepared.presentation.editorContextTint == glm::vec4(0.0f)
			&& cachedRecord->stableOrdinals.size() == commands.size()
			&& cachedRecord->selectedLods.size() == commands.size()
			&& cachedRecord->routeSelectors.size() == commands.size()
			&& cachedRecord->editorTints.size() == commands.size()
			&& cachedRecord->commandLanes != nullptr
			&& cachedRecord->commandLanes->size() == commands.size();
		if(reuseCachedViewLanes)
		{
			// A revision-validated cache record already owns the exact compact lanes
			// produced by visibility selection. The first hit resolves stable indices
			// into lifetime keys; later frames share both immutable arrays directly.
			prepared.presentation.retainedCommandLanes = cachedRecord->commandLanes;
			if(cachedRecord->retainedViewSelection == nullptr
				|| cachedRecord->retainedCommandSelectionIndices == nullptr)
			{
				auto retainedSelection = std::make_shared<TerraEngine::Renderer::
					RenderViewSelectionPayload>();
				auto commandSelectionIndices =
					std::make_shared<std::vector<std::uint32_t>>();
				retainedSelection->visibleOrdinals.reserve(commands.size());
				retainedSelection->selectedLods.reserve(commands.size());
				retainedSelection->routeSelectors.reserve(commands.size());
				retainedSelection->packetTemplateIdentities.reserve(commands.size());
				retainedSelection->tintSelectors.reserve(commands.size());
				retainedSelection->editorTints.reserve(commands.size());
				commandSelectionIndices->reserve(commands.size());
				for(std::size_t index = 0u; index < commands.size(); ++index)
				{
					std::uint32_t visibleSelection = TerraEngine::Renderer::
						RenderSlotGenerationKey::kInvalidOrdinal;
					const std::uint32_t renderableIndex =
						cachedRecord->stableOrdinals[index];
					const std::uint32_t identityIndex = renderableIndex
						< renderableDrawCommands.size()
							? renderableDrawCommands[renderableIndex].
								conventionalVisibilityIdentityIndex : 0u;
					if(identityIndex != 0u)
					{
						const std::uint32_t ordinal = identityIndex - 1u;
						const std::shared_ptr<const TerraEngine::Renderer::
							RenderPersistentTopologyRoot>& slotTopology =
								retainedRenderTopology.slotAllocator.topology;
						const std::size_t pageIndex = ordinal
							/ TerraEngine::Renderer::kRenderPublicationPageCapacity;
						const std::size_t slotIndex = ordinal
							% TerraEngine::Renderer::kRenderPublicationPageCapacity;
						const TerraEngine::Renderer::RenderPersistentSlot* persistentSlot =
							slotTopology != nullptr && pageIndex < slotTopology->pages.size()
								&& slotTopology->pages[pageIndex] != nullptr
								&& slotIndex < slotTopology->pages[pageIndex]->slots.size()
									? &slotTopology->pages[pageIndex]->slots[slotIndex] : nullptr;
						if(persistentSlot != nullptr && persistentSlot->IsComplete())
						{
							visibleSelection = static_cast<std::uint32_t>(
								retainedSelection->visibleOrdinals.size());
							retainedSelection->visibleOrdinals.push_back(persistentSlot->key);
							retainedSelection->selectedLods.push_back(
								cachedRecord->selectedLods[index]);
							retainedSelection->routeSelectors.push_back(
								cachedRecord->routeSelectors[index]);
							retainedSelection->packetTemplateIdentities.push_back(
								persistentSlot->packetTemplateIdentity);
							const glm::vec4& editorTint = cachedRecord->editorTints[index];
							retainedSelection->tintSelectors.push_back(
								editorTint == glm::vec4(0.0f) ? 0u : 1u);
							retainedSelection->editorTints.push_back(editorTint);
						}
					}
					commandSelectionIndices->push_back(visibleSelection);
				}
				cachedRecord->retainedViewSelection = std::move(retainedSelection);
				cachedRecord->retainedCommandSelectionIndices =
					std::move(commandSelectionIndices);
			}
			prepared.selection.retainedPayload = cachedRecord->retainedViewSelection;
			prepared.presentation.retainedCommandSelectionIndices =
				cachedRecord->retainedCommandSelectionIndices;
		}
		else
		{
			prepared.presentation.commandLanes.reserve(commands.size());
			prepared.presentation.commandSelectionIndices.reserve(commands.size());
			for(const RenderItemRecord& command : commands)
			{
				prepared.presentation.commandLanes.push_back(
					MakeRenderViewCommandLane(command));
				std::uint32_t visibleSelection = TerraEngine::Renderer::
					RenderSlotGenerationKey::kInvalidOrdinal;
				if(command.conventionalVisibilityIdentityIndex != 0u)
				{
					const std::uint32_t ordinal =
						command.conventionalVisibilityIdentityIndex - 1u;
					const std::shared_ptr<const TerraEngine::Renderer::RenderPersistentTopologyRoot>&
						slotTopology = retainedRenderTopology.slotAllocator.topology;
					const std::size_t pageIndex = ordinal
						/ TerraEngine::Renderer::kRenderPublicationPageCapacity;
					const std::size_t slotIndex = ordinal
						% TerraEngine::Renderer::kRenderPublicationPageCapacity;
					const TerraEngine::Renderer::RenderPersistentSlot* persistentSlot =
						slotTopology != nullptr && pageIndex < slotTopology->pages.size()
							&& slotTopology->pages[pageIndex] != nullptr
							&& slotIndex < slotTopology->pages[pageIndex]->slots.size()
								? &slotTopology->pages[pageIndex]->slots[slotIndex] : nullptr;
					if(persistentSlot != nullptr && persistentSlot->IsComplete())
					{
						visibleSelection = static_cast<std::uint32_t>(
							prepared.selection.visibleOrdinals.size());
						prepared.selection.visibleOrdinals.push_back(persistentSlot->key);
						prepared.selection.selectedLods.push_back(command.selectedVirtualLod);
						prepared.selection.routeSelectors.push_back(
							static_cast<std::uint32_t>(command.geometryRoute));
						prepared.selection.packetTemplateIdentities.push_back(
							persistentSlot->packetTemplateIdentity);
						prepared.selection.tintSelectors.push_back(
							command.editorTint == glm::vec4(0.0f) ? 0u : 1u);
						prepared.selection.editorTints.push_back(command.editorTint);
					}
				}
				prepared.presentation.commandSelectionIndices.push_back(visibleSelection);
			}
		}
		preparedRenderSubmissions.push_back(std::move(prepared));
	};
	// Each materialized view marks an index in the canonical renderable table.
	// The final publication walks this mask once, producing sorted unique UUIDs
	// without rescanning every camera command and sorting duplicates afterward.
	std::vector<std::uint8_t> visibleAnimatedRenderableFlags(
		renderables.size(), 0u);
	auto copyVirtualGeometryFramePayload = [](RenderViewBuildState& target, const VisibleCommandBuildResult& source) {
		target.virtualGeometryPacketEvidenceSignature = source.packetEvidenceSignature;
		if(source.cachedRecord != nullptr) {
			// The cache owns immutable evidence for at least this prepared view's
			// lifetime. Share it through backend submission instead of cloning several
			// packet tables that no stage is permitted to mutate.
			target.retainedVirtualGeometryGpuEvidence =
				source.cachedRecord->retainedGpuEvidence;
			target.virtualGeometryGpuPackets.clear();
			target.virtualGeometryGpuBatches.clear();
			target.virtualGeometryGpuFrameSets.clear();
			target.virtualGeometryGpuCompactionDispatches.clear();
			target.virtualGeometryGeneratedSubmissions.clear();
			target.virtualGeometryCounterReadbacks.clear();
			target.virtualGeometryGpuFallbackRecords.clear();
		}
		else {
			target.retainedVirtualGeometryGpuEvidence.reset();
			target.virtualGeometryGpuPackets = source.GpuPacketRecords();
			target.virtualGeometryGpuBatches = source.GpuBatchDescriptors();
			target.virtualGeometryGpuFrameSets = source.GpuPacketFrameSets();
			target.virtualGeometryGpuCompactionDispatches =
				source.GpuCompactionDispatches();
			target.virtualGeometryGeneratedSubmissions =
				source.GeneratedSubmissions();
			target.virtualGeometryCounterReadbacks = source.GpuCounterReadbacks();
			target.virtualGeometryGpuFallbackRecords = source.GpuFallbackRecords();
		}
		// The compute visibility mirror is intentionally opt-in. Ordinary dense VG
		// already renders through mesh tasks, so rebuilding CPU triangles and full
		// pixel buffers without a diagnostics consumer only duplicates frame work.
		target.virtualGeometryVisibilityDiagnosticsRequested =
			TerraEngine::Renderer::RenderDiagnosticsRequested()
			|| std::any_of(source.GpuPacketFrameSets().begin(),
				source.GpuPacketFrameSets().end(),
				[](const GpuVirtualGeometryPacketFrameSet& frameSet) {
					return frameSet.debugMode
						!= VirtualGeometryDebugVisualizationMode::None;
				});
	};
	auto applySceneViewContextTint = [&](RenderViewBuildState& target,
		std::vector<RenderItemRecord>& commands) {
		if (!sceneViewContextTintEnabled || sceneViewContextFocusedObjectIds.empty())
		{
			return;
		}

		// Prefab context mode keeps the opened prefab branch in normal material
		// color and gently washes every surrounding scene command toward white.
		// This runs only on the Scene View command list before submission.
		const glm::vec4 contextTint(1.0f, 1.0f, 1.0f, 0.25f);
		target.editorContextTint = contextTint;
		// Focus membership is editor state outside immutable scene visibility
		// evidence. Force compute-raster setup to rebuild when context tint is active.
		target.virtualGeometryVisibilitySetupSignature = 0u;
		for (RenderItemRecord& command : commands)
		{
			command.editorTint = sceneViewContextFocusedObjectIds.contains(command.objectId)
				? glm::vec4(0.0f)
				: contextTint;
		}
	};
	auto buildGroupedSubmissions = [&](RenderViewBuildState& target,
		const std::vector<RenderItemRecord>& commands,
		RenderOutputKind outputKind, RenderViewType viewType,
		std::uint64_t viewGeneration,
		std::uint64_t materializedCompatibilitySignature,
		bool viewEvidenceReused) {
		// This view-local sidecar is built after visibility and editor tint are
		// final, while the original commands remain complete fallback coverage.
		target.groupedInstanceRecords.clear();
		target.groupedSubmissions.clear();
		target.drawResourcePreparationCommandIndices.clear();
		target.renderOptimization = {};
		target.renderOptimization.frameGeneration = activeRenderFrameId;
		target.renderOptimization.topologyGeneration =
			retainedRenderTopology.topology.generation;
		target.renderOptimization.viewGeneration = viewGeneration;
		target.renderOptimization.rendererGeneration = snapshotRendererGeneration;
		target.renderOptimization.visibleInstanceCount = static_cast<std::uint32_t>(
			std::min<std::size_t>(commands.size(),
				std::numeric_limits<std::uint32_t>::max()));
		target.renderOptimization.topologyInvalidations = renderTopologyRebuilt ? 1u : 0u;
		target.renderOptimization.topologyReuseHits = renderTopologyRebuilt ? 0u : 1u;
		// This is the cache result for this exact view, not topology reuse. Keeping
		// the counters independent prevents the profiler from reporting a stable
		// packet topology as though it had also reused frustum/VG evidence.
		target.renderOptimization.viewEvidenceReuseHits =
			viewEvidenceReused ? 1u : 0u;
		target.renderOptimization.viewEvidenceInvalidations =
			viewEvidenceReused ? 0u : 1u;
		// Measure immutable compatibility discovery separately from dynamic record
		// materialization. Keeping these phases distinct makes profiler evidence
		// actionable when a large crowd regresses in only one part of preparation.
		const auto topologyBuildBegin = std::chrono::steady_clock::now();
		// Stable mesh/material/pipeline membership lives in retainedRenderTopology.
		// This per-view map hashes only values that still come from the one shared
		// push packet. Packet descriptors, page IDs, page byte ranges, meshlet
		// offsets, and animation records are intentionally absent: RenderInstanceRecord
		// carries them to the task/mesh stages for each crowd member. Keeping those
		// per-instance selectors in the key would recreate one draw per page and
		// defeat device-authored page-local grouping.
		const auto compatible = [&](std::uint32_t left, std::uint32_t right) {
			const RenderItemRecord& a = commands[left]; const RenderItemRecord& b = commands[right];
			return a.meshId == b.meshId && a.materialId == b.materialId
				&& a.materialIndex == b.materialIndex
				&& a.textureIndex == b.textureIndex
				&& a.geometryRoute == b.geometryRoute
				&& a.virtualMeshletCount == b.virtualMeshletCount
				&& a.selectedVirtualLod == b.selectedVirtualLod
				&& a.virtualGeometryFlags == b.virtualGeometryFlags
				&& a.virtualGeometryPayloadSource == b.virtualGeometryPayloadSource
				&& a.taskShaderCullingEnabled == b.taskShaderCullingEnabled
				&& a.animationBinding.source == b.animationBinding.source
				&& a.animationBinding.RequiresConservativeCoarseVisibility()
					== b.animationBinding.RequiresConservativeCoarseVisibility()
				&& a.editorTint == b.editorTint;
		};
		const auto dynamicCompatibilityHash = [](const RenderItemRecord& command,
			std::uint32_t topologyTemplateIndex) {
			std::size_t hash = std::hash<std::uint32_t>{}(topologyTemplateIndex);
			const auto combine = [&hash](std::uint64_t value) {
				hash ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b97f4a7c15ULL
					+ (hash << 6u) + (hash >> 2u);
			};
			combine(command.virtualMeshletCount);
			combine(command.materialIndex); combine(command.textureIndex);
			combine(static_cast<std::uint32_t>(command.geometryRoute));
			combine(command.selectedVirtualLod); combine(command.virtualGeometryFlags);
			combine(static_cast<std::uint32_t>(command.virtualGeometryPayloadSource));
			combine(command.taskShaderCullingEnabled ? 1u : 0u);
			combine(static_cast<std::uint32_t>(command.animationBinding.source));
			combine(command.animationBinding.RequiresConservativeCoarseVisibility()
				? 1u : 0u);
			// Scene prefab context uses one shared tint for surrounding objects. The
			// tint remains a push-constant value, so equal-tint instances may share a
			// dispatch while focused and context groups remain visually independent.
			combine(std::bit_cast<std::uint32_t>(command.editorTint.x));
			combine(std::bit_cast<std::uint32_t>(command.editorTint.y));
			combine(std::bit_cast<std::uint32_t>(command.editorTint.z));
			combine(std::bit_cast<std::uint32_t>(command.editorTint.w));
			// A view can become visible before its GPU animation packet is sealed.
			// Group membership rejects that pre-ready command, so the retained cache
			// must rebuild when the exact packet identity becomes complete. Without
			// this bit, a startup miss permanently preserved an empty crowd grouping.
			combine(command.HasCompleteGpuAnimation() ? 1u : 0u);
			return hash;
		};
		// Materialization folds the dynamic animation authority lanes into the
		// visibility evidence while commands are already hot. Untinted views can use
		// that proof directly; context-tinted editor views take the complete scan
		// because focus membership can change independently of visibility evidence.
		std::uint64_t compatibilitySignature = 1469598103934665603ULL;
		const auto combineCompatibilitySignature = [&compatibilitySignature](
			std::uint64_t value) {
			compatibilitySignature ^= value + 0x9e3779b97f4a7c15ULL
				+ (compatibilitySignature << 6U) + (compatibilitySignature >> 2U);
		};
		combineCompatibilitySignature(target.renderOptimization.topologyGeneration);
		combineCompatibilitySignature(static_cast<std::uint32_t>(outputKind));
		combineCompatibilitySignature(static_cast<std::uint32_t>(viewType));
		if(materializedCompatibilitySignature != 0U
			&& target.editorContextTint == glm::vec4(0.0f))
		{
			combineCompatibilitySignature(materializedCompatibilitySignature);
		}
		else
		{
			for(std::uint32_t index = 0u; index < commands.size(); ++index) {
				const RenderItemRecord& command = commands[index];
				// Visible commands carry a one-based canonical snapshot index. Use that
				// index to reach the retained immutable membership instead of rebuilding
				// a mesh/material/pipeline key for every object in every rendered view.
				const std::uint32_t identityIndex = command.conventionalVisibilityIdentityIndex;
				const std::size_t topologyRecordIndex = identityIndex == 0u
					? retainedRenderTopology.topology.records.size()
					: static_cast<std::size_t>(identityIndex - 1u);
				if(topologyRecordIndex >= retainedRenderTopology.topology.records.size()) {
					combineCompatibilitySignature(index);
					continue;
				}
				const PersistentRenderTopologyRecord& topologyRecord =
					retainedRenderTopology.topology.records[topologyRecordIndex];
				combineCompatibilitySignature(dynamicCompatibilityHash(
					command, topologyRecord.groupTemplateIndex));
				combineCompatibilitySignature(
					static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(command.editorTint.x))
						| (static_cast<std::uint64_t>(
							std::bit_cast<std::uint32_t>(command.editorTint.y)) << 32U));
				combineCompatibilitySignature(
					static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(command.editorTint.z))
						| (static_cast<std::uint64_t>(
							std::bit_cast<std::uint32_t>(command.editorTint.w)) << 32U));
			}
		}
		std::uint64_t viewCacheKey = static_cast<std::uint64_t>(outputKind);
		viewCacheKey ^= static_cast<std::uint64_t>(viewType) << 8U;
		// View generation advances with frame publication, while compatible member
		// ordering depends only on this output/view lane and the signature above.
		// Keeping it out of the key turns this into a bounded persistent cache instead
		// of allocating a dead entry for every frame of a long editor session.
		GroupedSubmissionMembershipCacheRecord& membershipCache =
			groupedSubmissionMembershipCache.recordsByView[viewCacheKey];
		const bool membershipCacheHit = membershipCache.valid
			&& membershipCache.compatibilitySignature == compatibilitySignature;
		if(!membershipCacheHit) {
			std::vector<GroupedCompatibilityBucket> rebuiltBuckets;
			rebuiltBuckets.reserve(commands.size());
			std::unordered_map<std::size_t, std::vector<std::size_t>> bucketsByDynamicHash;
			bucketsByDynamicHash.reserve(commands.size());
			for(std::uint32_t index = 0u; index < commands.size(); ++index) {
				const RenderItemRecord& command = commands[index];
				const std::uint32_t identityIndex =
					command.conventionalVisibilityIdentityIndex;
				const std::size_t topologyRecordIndex = identityIndex == 0u
					? retainedRenderTopology.topology.records.size()
					: static_cast<std::size_t>(identityIndex - 1u);
				if(topologyRecordIndex >= retainedRenderTopology.topology.records.size()) {
					continue;
				}
				const PersistentRenderTopologyRecord& topologyRecord =
					retainedRenderTopology.topology.records[topologyRecordIndex];
				if(!(command.UsesGroupableMeshTaskRoute()
					&& (command.animationBinding.source == TerraEngine::Renderer::AnimationRenderPoseSource::BindPose
						|| command.HasCompleteGpuAnimation())
					&& topologyRecord.groupTemplateIndex
						!= PersistentRenderTopologyRecord::kInvalidGroupTemplateIndex)) {
					continue;
				}
				const std::size_t hash = dynamicCompatibilityHash(
					command, topologyRecord.groupTemplateIndex);
				auto& collisionCandidates = bucketsByDynamicHash[hash];
				auto existing = std::find_if(collisionCandidates.begin(),
					collisionCandidates.end(), [&](std::size_t bucketIndex) {
						return rebuiltBuckets[bucketIndex].topologyTemplateIndex
							== topologyRecord.groupTemplateIndex
							&& compatible(rebuiltBuckets[bucketIndex].representative, index);
					});
				if(existing == collisionCandidates.end()) {
					const std::size_t bucketIndex = rebuiltBuckets.size();
					GroupedCompatibilityBucket bucket{};
					bucket.topologyTemplateIndex = topologyRecord.groupTemplateIndex;
					bucket.representative = index;
					bucket.members.push_back(index);
					rebuiltBuckets.push_back(std::move(bucket));
					collisionCandidates.push_back(bucketIndex);
				}
				else {
					rebuiltBuckets[*existing].members.push_back(index);
				}
			}
			membershipCache.valid = true;
			membershipCache.compatibilitySignature = compatibilitySignature;
			membershipCache.buckets = std::move(rebuiltBuckets);
			membershipCache.submissionTemplates.clear();
			membershipCache.drawResourcePreparationCommandIndices.clear();
			membershipCache.submissionTemplates.reserve(
				membershipCache.buckets.size());
			membershipCache.drawResourcePreparationCommandIndices.reserve(
				commands.size());
			std::vector<std::uint8_t> groupedCommandCoverage(commands.size(), 0u);
			std::uint32_t firstInstance = 0u;
			for(const GroupedCompatibilityBucket& bucket : membershipCache.buckets) {
				if(bucket.members.size() <= 1u) {
					continue;
				}
				const RenderItemRecord& representative =
					commands[bucket.representative];
				membershipCache.drawResourcePreparationCommandIndices.push_back(
					bucket.representative);
				GroupedRenderSubmission submission{};
				submission.representativeCommandIndex = bucket.representative;
				submission.group.firstInstance = firstInstance;
				submission.group.instanceCount = static_cast<std::uint32_t>(
					bucket.members.size());
				submission.group.key.meshId = representative.meshId;
				submission.group.key.materialId = representative.materialId;
				submission.group.key.geometryRoute = static_cast<std::uint32_t>(
					representative.geometryRoute);
				submission.group.key.renderPassVariant = static_cast<std::uint32_t>(
					outputKind);
				submission.group.key.viewVariant = static_cast<std::uint32_t>(viewType);
				firstInstance += submission.group.instanceCount;
				membershipCache.submissionTemplates.push_back(std::move(submission));
				for(const std::uint32_t commandIndex : bucket.members) {
					if(commandIndex < groupedCommandCoverage.size()) {
						groupedCommandCoverage[commandIndex] = 1u;
					}
				}
			}
			for(std::uint32_t commandIndex = 0u;
				commandIndex < groupedCommandCoverage.size(); ++commandIndex)
			{
				if(groupedCommandCoverage[commandIndex] == 0u) {
					membershipCache.drawResourcePreparationCommandIndices.push_back(
						commandIndex);
				}
			}
		}
		membershipCache.lastTouchedFrameId = activeRenderFrameId;
		const std::vector<GroupedCompatibilityBucket>& buckets = membershipCache.buckets;
		target.groupedInstanceRecords.reserve(commands.size());
		target.groupedSubmissions = membershipCache.submissionTemplates;
		target.drawResourcePreparationCommandIndices =
			membershipCache.drawResourcePreparationCommandIndices;
		// Only publication generations vary for a retained group descriptor. Patch
		// those scalars in the view-local copy while the mesh/material/pass identity
		// and executable ranges remain cache-owned and immutable.
		for(GroupedRenderSubmission& submission : target.groupedSubmissions) {
			submission.group.topologyGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.viewGeneration = viewGeneration;
			submission.group.key.meshGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.key.materialGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.key.pipelineGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.key.residencyGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.key.rendererGeneration =
				target.renderOptimization.rendererGeneration;
			submission.group.executable = submission.group.key.IsComplete();
		}
		target.renderOptimization.topologyBuildNanoseconds =
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - topologyBuildBegin).count());
		const auto dynamicMaterializationBegin = std::chrono::steady_clock::now();
		for(const GroupedCompatibilityBucket& bucket : buckets) {
			if(bucket.members.size() > 1u) {
				for(const std::uint32_t commandIndex : bucket.members) {
					const RenderItemRecord& command = commands[commandIndex];
					RenderInstanceRecord record{}; record.modelMatrix = command.modelMatrix;
					record.editorTint = command.editorTint;
					// The identity key resolves through this frame's immutable table;
					// keeping it in the stream means every grouped member, rather than
					// only the representative push constants, can prove its owner.
					record.objectIdentity = glm::uvec4(
						command.conventionalVisibilityIdentityIndex,
						static_cast<std::uint32_t>(command.geometryRoute),
						command.virtualGeometryFlags,
						RenderInstanceRecord::PackCompatibilityPolicy(
							command.taskShaderCullingEnabled,
							static_cast<std::uint32_t>(
								command.animationBinding.source),
							command.animationBinding.
								RequiresConservativeCoarseVisibility()));
					record.materialAndTexture = glm::uvec4(command.materialIndex,
						command.textureIndex, commandIndex,
						command.conventionalVisibilityIdentityIndex);
					record.animationSelectors = glm::uvec4(
						command.animation.firstGpuWorkPacket,
						// Dense grouped rendering resolves the current immutable plan at the
						// backend boundary and consumes only the stable packet index. Keeping
						// this redundant generation zero prevents every crowd record from
						// becoming dirty each frame. Non-dense paths patch both selector words.
						0u,
						static_cast<std::uint32_t>(
							command.animation.rendererGeneration),
						static_cast<std::uint32_t>(
							command.animation.programGeneration));
					record.geometrySelectors = glm::uvec4(command.selectedVirtualLod, command.virtualGeometryPacketDescriptorIndex,
						command.virtualMeshletOffset, command.virtualMeshletCount);
					record.virtualGeometrySelectors = glm::uvec4(
						command.selectedVirtualClusterOffset, command.selectedVirtualClusterCount,
						command.selectedVirtualPageNeedOffset, command.selectedVirtualPageNeedCount);
					record.virtualGeometryPayloadSelectors = glm::uvec4(
						static_cast<std::uint32_t>(command.virtualGeometryPayloadSource),
						static_cast<std::uint32_t>(command.virtualGeometryEmittedSource),
						static_cast<std::uint32_t>(command.virtualGeometryPagePayloadByteOffset),
						static_cast<std::uint32_t>(command.virtualGeometryPagePayloadByteSize));
					target.groupedInstanceRecords.push_back(record);
				}
			}
		}
		target.renderOptimization.groupedInstanceCount = static_cast<std::uint32_t>(target.groupedInstanceRecords.size());
		target.renderOptimization.groupedSubmissionCount = static_cast<std::uint32_t>(target.groupedSubmissions.size());
		target.renderOptimization.fallbackInstanceCount = target.renderOptimization.visibleInstanceCount - target.renderOptimization.groupedInstanceCount;
		target.renderOptimization.fallbackSubmissionCount = target.renderOptimization.fallbackInstanceCount;
		target.renderOptimization.avoidedSubmissionCount = target.renderOptimization.groupedInstanceCount - target.renderOptimization.groupedSubmissionCount;
		target.renderOptimization.instanceStreamBytes = static_cast<std::uint32_t>(std::min<std::size_t>(
			target.groupedInstanceRecords.size() * sizeof(RenderInstanceRecord), std::numeric_limits<std::uint32_t>::max()));
		target.renderOptimization.dynamicMaterializationNanoseconds =
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - dynamicMaterializationBegin).count());
		// Validate the exact executable ranges through the production Vulkan work
		// cache contract before the backend is allowed to suppress fallback draws.
		// Group validation reads only instance counts and executable group ranges.
		// Keep one small contiguous group array, but validate the already-owned instance
		// stream directly instead of copying all instances and fallback draw commands.
		std::vector<SharedRenderGroup> dynamicGroups;
		dynamicGroups.reserve(target.groupedSubmissions.size());
		for (const GroupedRenderSubmission& submission
			: target.groupedSubmissions)
		{
			dynamicGroups.push_back(submission.group);
		}
		const TerraEngine::Renderer::Vulkan::GroupedInstanceStreamPlan groupedPlan =
			TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(
				target.groupedInstanceRecords,
				dynamicGroups,
				activeRenderFrameId,
				snapshotRendererGeneration,
				retainedRenderTopology.topology.valid,
				std::numeric_limits<std::uint32_t>::max() / sizeof(RenderInstanceRecord),
				snapshotRendererGeneration);
		if (!groupedPlan.valid && !target.groupedSubmissions.empty())
		{
			// Atomic fallback keeps the complete per-model command list authoritative.
			target.groupedInstanceRecords.clear();
			target.groupedSubmissions.clear();
			target.renderOptimization.groupedInstanceCount = 0u;
			target.renderOptimization.groupedSubmissionCount = 0u;
			target.renderOptimization.fallbackInstanceCount =
				target.renderOptimization.visibleInstanceCount;
			target.renderOptimization.fallbackSubmissionCount =
				target.renderOptimization.visibleInstanceCount;
			target.renderOptimization.avoidedSubmissionCount = 0u;
			target.renderOptimization.instanceStreamBytes = 0u;
			switch (groupedPlan.rejectionReason)
			{
			case RenderGroupRejectionReason::IncompleteKey:
				++target.renderOptimization.rejectedIncompleteKeyCount;
				break;
			case RenderGroupRejectionReason::InstanceRange:
				++target.renderOptimization.rejectedOutOfRangeCount;
				break;
			case RenderGroupRejectionReason::StaleGeneration:
				++target.renderOptimization.rejectedStaleGenerationCount;
				break;
			case RenderGroupRejectionReason::InstanceStreamAllocation:
				++target.renderOptimization.fallbackAllocationFailureCount;
				break;
			default:
				++target.renderOptimization.rejectedUnsupportedVariantCount;
				break;
			}
		}
		target.renderOptimization.valid = 1u;
	};
	const bool sceneViewTargetReady = fixedSceneCamera || !editorModeEnabled || (sceneViewTargetSize.x > 0u && sceneViewTargetSize.y > 0u);
	const bool gameViewTargetReady = fixedGameCamera || !editorModeEnabled || (gameViewTargetSize.x > 0u && gameViewTargetSize.y > 0u);
	const auto findReferenceFrame = [&](RenderOutputKind outputKind,
		std::uint32_t outputId)
	{
		for (const TerraEngine::Renderer::RenderFrameViewRequest& request :
			renderFrameSnapshot.viewRequests)
		{
			if (request.outputKind == outputKind && request.outputId == outputId)
			{
				return request.referenceFrame;
			}
		}
		return TerraEngine::Renderer::RenderReferenceFrame{};
	};
	const auto localizeFrameLights = [](RenderViewBuildState& view)
	{
		view.lightCandidates.erase(std::remove_if(
			view.lightCandidates.begin(), view.lightCandidates.end(),
			[&](FrameLightCandidate& candidate)
			{
				const TerraEngine::Renderer::RenderLocalizationResult localized =
					TerraEngine::Renderer::LocalizeRenderPosition(
						candidate.canonicalPosition, view.referenceFrame);
				if (!localized.Succeeded())
				{
					return true;
				}
				candidate.gpuLight.positionAndType = glm::vec4(
					localized.offsetMetres,
					candidate.gpuLight.positionAndType.w);
				return false;
			}), view.lightCandidates.end());
	};
	const auto localizeFixtureCamera = [&](RenderViewBuildState& view, const TerraEngine::Scene::WorldPosition& canonicalCamera) {
		// Sealed matrices already incorporate both baseline and per-step origins.
		// Applying the legacy UI-camera translation again would double the rebase.
		if(sealedCamera(view.referenceFrame.outputId == 1 ? RenderViewType::Scene : RenderViewType::Game,view.referenceFrame.outputId)) return;
		if (!view.efficiencyFixtureIntent.controlsApplied || !TerraEngine::Renderer::RendererEfficiencyFixtureOriginDelta(
			view.efficiencyFixtureIntent, view.referenceFrame.outputId == 1 ? RenderViewType::Scene : RenderViewType::Game,
			view.referenceFrame.outputId)) return;
		const auto localized = TerraEngine::Renderer::LocalizeRenderPosition(canonicalCamera, view.referenceFrame);
		if (!localized.Succeeded()) return; // Preflight already rejects unsafe view deltas.
		const glm::vec3 camera(localized.offsetMetres);
		view.viewMatrix = view.viewMatrix * glm::translate(glm::mat4(1), view.cameraLocalPosition - camera);
		view.cameraLocalPosition = camera;
	};

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if ((fixedSceneCamera || (!sealedCameraRecipeActive && hasSceneViewCamera)) && sceneViewTargetReady)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Scene View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Start from the small common frame header, then build this view in its own
		// owner. Copying the shared header before materialization avoids deep-copying
		// thousands of completed command and page records when the view is queued.
		RenderViewBuildState sceneFrameData = frameData;
		sceneFrameData.viewMatrix = fixedSceneCamera ? fixedSceneCamera->viewMatrix : sceneViewCamera.view;
		sceneFrameData.projectionMatrix = fixedSceneCamera ? fixedSceneCamera->projectionMatrix : sceneViewCamera.proj;
		sceneFrameData.referenceFrame = findReferenceFrame(
			RenderOutputKind::SceneView, 1U);
		sceneFrameData.cameraLocalPosition = fixedSceneCamera ? fixedSceneCamera->cameraLocalPosition : glm::vec3(0.0F);
		localizeFixtureCamera(sceneFrameData, sceneViewCamera.canonicalPosition);
		localizeFrameLights(sceneFrameData);
		sceneFrameData.sceneViewGridVisible = !fixedSceneCamera && sceneViewGridVisible;
		sceneFrameData.sceneViewOrthographic = !fixedSceneCamera && sceneViewCamera.orthographic;
		VisibleCommandBuildResult visibleCommands;
		std::vector<RenderItemRecord>& sceneCommands =
			lifetimeState->viewCommandScratch;
		sceneCommands.clear();
		std::uint64_t sceneGroupingCompatibilitySignature = 0U;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Scene View Visible Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			visibleCommands = buildVisibleDrawCommands(
				sceneFrameData.projectionMatrix * sceneFrameData.viewMatrix,
				effectiveSceneTargetSize, RenderOutputKind::SceneView,
				RenderViewType::Scene, 1U, sceneFrameData.cameraLocalPosition,
				sceneFrameData.referenceFrame);
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Materialize Scene View Commands",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			visibleCommands.PopulateSelectedItems(sceneCommands, renderables,
				renderableDrawCommands,
				&visibleAnimatedRenderableFlags, &renderableCommandIndices,
				&sceneGroupingCompatibilitySignature);
			sceneFrameData.virtualGeometryVisibilitySetupSignature =
				sceneGroupingCompatibilitySignature;
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Bind Scene View Geometry Evidence",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			copyVirtualGeometryFramePayload(sceneFrameData, visibleCommands);
		}
		sceneSpatialDiagnostics = visibleCommands.diagnostics;
		sceneVisibleCommandTelemetry = visibleCommands.reuseTelemetry;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Apply Scene View Context",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(!fixedSceneCamera) applySceneViewContextTint(sceneFrameData, sceneCommands);
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Scene View Groups",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			buildGroupedSubmissions(sceneFrameData, sceneCommands,
				RenderOutputKind::SceneView,
				RenderViewType::Scene, 1u,
				sceneGroupingCompatibilitySignature,
				visibleCommands.cacheHit);
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Scene View Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			// The spatial query uses its unchanged canonical index frame, not the
			// deliberately shifted GPU view frame. Rebasing cannot change membership.
			sceneFrameData.shadowDrawLists = buildShadowDrawLists(sceneFrameData.lightCandidates, sceneFrameData.cameraLocalPosition, sceneFrameData.referenceFrame, RenderViewType::Scene,1);
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Queue Scene View Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			queuePreparedSubmission(std::move(sceneFrameData),
				makeOutputRequest(RenderOutputKind::SceneView, RenderViewType::Scene, 1u),
				sceneCommands, visibleCommands.cachedRecord);
		}
	}
	else if (TerraEngine::Renderer::RenderDiagnosticsRequested())
	{
		renderGraphDiagnostics.delayedWork.push_back(
			hasSceneViewCamera
				? "Scene View skipped because its render target is not ready."
				: "Scene View skipped because the editor did not provide a scene camera this frame.");
	}

	// Asset-style prefab editing has no active runtime scene. When the editor
	// requests suppression, skip every Game View submission path so opening a
	// prefab asset cannot burn a full offscreen game render every frame.
	if (fixedGameCamera || (!sealedCameraRecipeActive && !gameViewSuppressed && gameViewTargetReady && !gameViewCameras.empty()))
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Game View Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Use the same existing GameView query/submission path with one immutable
		// sealed descriptor; author components and UI descriptors remain untouched.
		std::array<OverrideCameraData,1> sealedGameDescriptor{};
		std::span<const OverrideCameraData> effectiveGameCameras = gameViewCameras;
		if(fixedGameCamera) {
			sealedGameDescriptor[0] = {fixedGameCamera->viewMatrix,fixedGameCamera->projectionMatrix,fixedGameCamera->cameraPosition,false};
			effectiveGameCameras = sealedGameDescriptor;
		}
		for (const auto& cam : effectiveGameCameras)
		{
			RenderViewBuildState gameFrameData = frameData;
			gameFrameData.editorContextTint = glm::vec4(0.0f);
			gameFrameData.viewMatrix = cam.view;
			gameFrameData.projectionMatrix = cam.proj;
			gameFrameData.referenceFrame = findReferenceFrame(
				RenderOutputKind::GameView, 2U);
			gameFrameData.cameraLocalPosition = fixedGameCamera ? fixedGameCamera->cameraLocalPosition : glm::vec3(0.0F);
			localizeFixtureCamera(gameFrameData, cam.canonicalPosition);
			localizeFrameLights(gameFrameData);
			VisibleCommandBuildResult visibleCommands;
			std::vector<RenderItemRecord>& gameCommands =
				lifetimeState->viewCommandScratch;
			gameCommands.clear();
			std::uint64_t gameGroupingCompatibilitySignature = 0U;
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Game View Visible Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				visibleCommands = buildVisibleDrawCommands(
					gameFrameData.projectionMatrix * gameFrameData.viewMatrix,
					effectiveGameTargetSize, RenderOutputKind::GameView,
					RenderViewType::Game, 2U, gameFrameData.cameraLocalPosition,
					gameFrameData.referenceFrame);
			}
			visibleCommands.PopulateSelectedItems(gameCommands, renderables,
				renderableDrawCommands,
				&visibleAnimatedRenderableFlags, &renderableCommandIndices,
				&gameGroupingCompatibilitySignature);
			gameFrameData.virtualGeometryVisibilitySetupSignature =
				gameGroupingCompatibilitySignature;
			copyVirtualGeometryFramePayload(gameFrameData, visibleCommands);
			gameSpatialDiagnostics = visibleCommands.diagnostics;
			gameVisibleCommandTelemetry = visibleCommands.reuseTelemetry;
			buildGroupedSubmissions(gameFrameData, gameCommands,
				RenderOutputKind::GameView,
				RenderViewType::Game, 2u,
				gameGroupingCompatibilitySignature,
				visibleCommands.cacheHit);
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Game View Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				gameFrameData.shadowDrawLists = buildShadowDrawLists(gameFrameData.lightCandidates, gameFrameData.cameraLocalPosition, gameFrameData.referenceFrame, RenderViewType::Game,2);
			}
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Queue Game View Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				queuePreparedSubmission(std::move(gameFrameData),
					makeOutputRequest(RenderOutputKind::GameView, RenderViewType::Game, 2u),
					gameCommands, visibleCommands.cachedRecord);
			}
			latestGameViewFrameId = activeRenderFrameId;
		}
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (!sealedCameraRecipeActive && !gameViewSuppressed && gameViewTargetReady && cameras.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Blank Game View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderViewBuildState blankData{};
		blankData.frameId = activeRenderFrameId;
		blankData.viewMatrix = glm::mat4(1.0f);
		blankData.projectionMatrix = glm::mat4(1.0f);
		blankData.cameraLocalPosition = glm::vec3(0.0f);
		blankData.deltaTimeSeconds = deltaTime;
		blankData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
			blankData.cullingEpsilon = 0.001f;
			blankData.activePreset = currentPreset;
			blankData.customOverrideActive = (currentPreset == QualityPreset::Custom);
			blankData.settings = SanitizeRenderSettings(currentSettings);
			blankData.efficiencyOptions = renderFrameSnapshot.efficiencyOptions;
			blankData.efficiencyFixtureIntent = renderFrameSnapshot.efficiencyFixtureIntent;
			blankData.sky = frameData.sky;
			blankData.editorContextTint = glm::vec4(0.0f);
			blankData.virtualGeometryGpuPackets.clear();
			blankData.virtualGeometryGpuBatches.clear();
			blankData.virtualGeometryGpuFrameSets.clear();
			blankData.virtualGeometryGpuCompactionDispatches.clear();
				blankData.virtualGeometryGeneratedSubmissions.clear();
				blankData.virtualGeometryCounterReadbacks.clear();
				blankData.virtualGeometryGpuFallbackRecords.clear();
				blankData.virtualGeometryPageRetirements.clear();
				blankData.shadowDrawLists.clear();
			queuePreparedSubmission(std::move(blankData),
				makeOutputRequest(RenderOutputKind::GameView, RenderViewType::Game, 2u),
				{}, nullptr);
			latestGameViewFrameId = activeRenderFrameId;
	}
	else if (!sealedCameraRecipeActive && !gameViewSuppressed && gameViewTargetReady)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Runtime Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t cameraIndex = 0; cameraIndex < cameras.size(); ++cameraIndex)
		{
			RenderViewBuildState runtimeFrameData = frameData;
			const entt::entity cameraEntity = cameras[cameraIndex].entity;
			const auto& camera = cameras[cameraIndex].camera;
			const auto propagatedCameraTransform = worldTransforms->find(cameraEntity);
			TerraEngine::Scene::WorldPose cameraTransform =
				propagatedCameraTransform != worldTransforms->end()
					? propagatedCameraTransform->second : TerraEngine::Scene::WorldPose{};

			// Normalize the rotation so derived direction vectors remain unit length
			// even if authoring data drifts slightly off the identity quaternion.
			const glm::quat cameraRotation = glm::normalize(cameraTransform.rotation);
			const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
			const glm::vec3 forward = cameraRotation * defaultForward;
			glm::vec3 up = cameraRotation * camera.up;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (glm::length(up) < 0.0001f)
			{
				up = glm::vec3(0.0f, 1.0f, 0.0f);
			}

			// Clamp projection inputs to keep glm::perspective from receiving invalid values.
			const float clampedFov = std::max(0.001f, camera.verticalFovDegrees);
			const float safeAspect = camera.aspectRatio > 0.0f ? camera.aspectRatio : 1.0f;
			const float nearPlane = std::max(0.0001f, camera.nearPlane);
			const float farPlane = std::max(nearPlane + 0.001f, camera.farPlane);
			runtimeFrameData.viewMatrix = glm::lookAt(
				glm::vec3(0.0F), forward, glm::normalize(up));
			// Use an explicit Vulkan depth projection. Relying on GLM's global
			// depth macro is fragile because this file includes engine headers
			// before local GLM configuration defines are visible.
			runtimeFrameData.projectionMatrix = glm::perspectiveRH_ZO(glm::radians(clampedFov), safeAspect, nearPlane, farPlane);
			runtimeFrameData.projectionMatrix[1][1] *= -1;
			runtimeFrameData.cameraLocalPosition = glm::vec3(0.0F);
			runtimeFrameData.referenceFrame = findReferenceFrame(
				RenderOutputKind::MainRuntimeView, 2U);
			localizeFixtureCamera(runtimeFrameData, cameraTransform.position);
			localizeFrameLights(runtimeFrameData);
			// The editor publishes this descriptor through GameViewWindow. A
			// packaged runtime has no dock, so publish the exact authored camera
			// used for MainRuntimeView here as read-only capture metadata. This
			// lets exported-runtime validation request a real Vulkan readback
			// without inventing an editor-only camera owner.
			latestGameViewCamera = {
				runtimeFrameData.viewMatrix,
				runtimeFrameData.projectionMatrix,
				cameraTransform.position,
				false
			};
			hasLatestGameViewCamera = true;
			runtimeFrameData.editorContextTint = glm::vec4(0.0f);
			const VisibleCommandBuildResult visibleCommands = buildVisibleDrawCommands(
				runtimeFrameData.projectionMatrix * runtimeFrameData.viewMatrix,
				gameViewTargetSize, RenderOutputKind::MainRuntimeView,
				RenderViewType::Game, 2U, runtimeFrameData.cameraLocalPosition,
				runtimeFrameData.referenceFrame);
			std::vector<RenderItemRecord>& runtimeCommands =
				lifetimeState->viewCommandScratch;
			runtimeCommands.clear();
			std::uint64_t runtimeGroupingCompatibilitySignature = 0U;
			visibleCommands.PopulateSelectedItems(runtimeCommands, renderables,
				renderableDrawCommands,
				&visibleAnimatedRenderableFlags, &renderableCommandIndices,
				&runtimeGroupingCompatibilitySignature);
			runtimeFrameData.virtualGeometryVisibilitySetupSignature =
				runtimeGroupingCompatibilitySignature;
			copyVirtualGeometryFramePayload(runtimeFrameData, visibleCommands);
			gameSpatialDiagnostics = visibleCommands.diagnostics;
			buildGroupedSubmissions(runtimeFrameData, runtimeCommands,
				RenderOutputKind::MainRuntimeView,
				RenderViewType::Game, 2u,
				runtimeGroupingCompatibilitySignature,
				visibleCommands.cacheHit);
			runtimeFrameData.shadowDrawLists = buildShadowDrawLists(runtimeFrameData.lightCandidates, runtimeFrameData.cameraLocalPosition, runtimeFrameData.referenceFrame, RenderViewType::Game,2);

			queuePreparedSubmission(std::move(runtimeFrameData),
				makeOutputRequest(RenderOutputKind::MainRuntimeView,
					RenderViewType::Game, 2u), runtimeCommands,
				visibleCommands.cachedRecord);
			latestGameViewFrameId = activeRenderFrameId;
		}
	}
	else if (TerraEngine::Renderer::RenderDiagnosticsRequested() && gameViewSuppressed)
	{
		renderGraphDiagnostics.delayedWork.push_back("Game View skipped because the editor suppressed it for prefab asset editing.");
	}
	else if (TerraEngine::Renderer::RenderDiagnosticsRequested() && !gameViewTargetReady)
	{
		renderGraphDiagnostics.delayedWork.push_back("Game View skipped because its render target is not ready.");
	}

	std::deque<TerraEngine::Renderer::RenderCaptureRequest> captureRequestsForReadback;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		while (!pendingCaptureRequests.empty() && captureQueueState.activeCaptureCount < captureLimits.maxConcurrentCaptures)
		{
			captureRequestsForReadback.push_back(std::move(pendingCaptureRequests.front()));
			pendingCaptureRequests.pop_front();
			++captureQueueState.activeCaptureCount;
		}
		captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
	}
	for (const TerraEngine::Renderer::RenderCaptureRequest& captureRequest : captureRequestsForReadback)
	{
		const bool needsDedicatedCaptureRender = (captureRequest.sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView
				|| captureRequest.dedicatedRenderTarget)
			&& captureRequest.format == "image/png"
			&& rhi
			&& backendCapabilities.supportsImageReadback;
		if (!needsDedicatedCaptureRender)
		{
			continue;
		}

		// Camera-object and custom MCP requests already carry immutable camera
		// matrices prepared by editor automation. Submit them as a dedicated
		// offscreen render target so readback sees live GPU pixels without
		// activating disabled authored cameras or changing the visible Game View.
		RenderViewBuildState captureFrameData = frameData;
		captureFrameData.viewMatrix = captureRequest.viewMatrix;
		captureFrameData.projectionMatrix = captureRequest.projectionMatrix;
		captureFrameData.cameraLocalPosition = glm::vec3(0.0F);
		captureFrameData.editorContextTint = glm::vec4(0.0f);
		captureFrameData.uiBatch = {};
		const TerraEngine::Renderer::RenderCaptureViewIdentity captureIdentity =
			TerraEngine::Renderer::ResolveDedicatedCaptureViewIdentity(
				captureRequest.viewType);
		captureFrameData.referenceFrame = findReferenceFrame(
			RenderOutputKind::OffscreenTexture, captureIdentity.outputId);
		localizeFrameLights(captureFrameData);
		const VisibleCommandBuildResult visibleCommands = buildVisibleDrawCommands(
			captureFrameData.projectionMatrix * captureFrameData.viewMatrix,
			captureRequest.targetSize,
			RenderOutputKind::OffscreenTexture,
			captureRequest.viewType,
			captureIdentity.outputId,
			captureFrameData.cameraLocalPosition,
			captureFrameData.referenceFrame);
		std::vector<RenderItemRecord>& captureCommands =
			lifetimeState->viewCommandScratch;
		captureCommands.clear();
		std::uint64_t captureGroupingCompatibilitySignature = 0U;
		visibleCommands.PopulateSelectedItems(captureCommands, renderables,
			renderableDrawCommands,
			&visibleAnimatedRenderableFlags, &renderableCommandIndices,
			&captureGroupingCompatibilitySignature);
		captureFrameData.virtualGeometryVisibilitySetupSignature =
			captureGroupingCompatibilitySignature;
		copyVirtualGeometryFramePayload(captureFrameData, visibleCommands);
		buildGroupedSubmissions(captureFrameData, captureCommands,
			RenderOutputKind::OffscreenTexture,
			captureIdentity.viewType,
			captureIdentity.outputId,
			captureGroupingCompatibilitySignature,
			visibleCommands.cacheHit);
		captureFrameData.shadowDrawLists = buildShadowDrawLists(captureFrameData.lightCandidates, captureFrameData.cameraLocalPosition, captureFrameData.referenceFrame);

		RenderOutputRequest captureOutputRequest = makeOutputRequest(
			RenderOutputKind::OffscreenTexture, captureIdentity.viewType,
			captureIdentity.outputId);
		captureOutputRequest.targetSize = captureRequest.targetSize;
		queuePreparedSubmission(std::move(captureFrameData),
			std::move(captureOutputRequest), captureCommands,
			visibleCommands.cachedRecord);
	}
	if (renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied && !renderFrameSnapshot.efficiencyFixtureIntent.viewOrder.empty())
	{
		std::vector<std::size_t> actualSlots;
		std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureViewOrderEntry> actual;
		for (std::size_t index = 0; index < preparedRenderSubmissions.size(); ++index)
		{
			const auto& output = preparedRenderSubmissions[index].output;
			if (output.kind == RenderOutputKind::SceneView || output.kind == RenderOutputKind::GameView || output.kind == RenderOutputKind::MainRuntimeView)
			{ actualSlots.push_back(index); actual.push_back({output.diagnosticsView, output.outputId}); }
		}
		std::vector<std::size_t> order; std::string reason;
		if (TerraEngine::Renderer::ResolveRendererEfficiencyFixtureViewOrder(renderFrameSnapshot.efficiencyFixtureIntent, actual, order, reason))
		{
			// Move the actual presentation owners before sealing ranges and snapshot
			// slots. RHI iteration and vkQueueSubmit now consume this exact order.
			std::vector<PreparedRenderSubmission> reordered; reordered.reserve(preparedRenderSubmissions.size());
			for (const auto index : order) reordered.push_back(std::move(preparedRenderSubmissions[actualSlots[index]]));
			for (std::size_t index = 0; index < preparedRenderSubmissions.size(); ++index)
				if (std::find(actualSlots.begin(), actualSlots.end(), index) == actualSlots.end()) reordered.push_back(std::move(preparedRenderSubmissions[index]));
			preparedRenderSubmissions = std::move(reordered);
		}
		else { renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied = false; renderFrameSnapshot.efficiencyFixtureIntent.controlReason = reason; }
	}
	// View-local history is valid only while its exact output identity remains in
	// the protected request set. Removing a view retires its Hi-Z/LOD/debug state;
	// recreating an output ID or resizing it starts from conservative visibility.
	std::erase_if(virtualGeometryViewPolicies,
		[&activeVirtualGeometryViewPolicyKeys](const auto& entry) {
			return !activeVirtualGeometryViewPolicyKeys.contains(entry.first);
		});
	std::erase_if(virtualGeometryTransitionCache,
		[&activeVirtualGeometryTransitionKeys](const auto& entry) {
			return !activeVirtualGeometryTransitionKeys.contains(entry.first);
		});
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		std::erase_if(virtualGeometryDebugModes,
			[&activeVirtualGeometryDebugModeKeys](const auto& entry) {
				return !activeVirtualGeometryDebugModeKeys.contains(entry.first);
			});
	}

	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Publish Animated Visibility",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		TerraEngine::Renderer::AnimatedObjectVisibilitySnapshot visibility;
		visibility.frameId = activeRenderFrameId;
		visibility.sceneGeneration = protectedSceneGeneration;
		visibility.hasActiveView = !preparedRenderSubmissions.empty();
		visibility.valid = true;
		// Camera commands marked the canonical mask while their current animation
		// bindings were attached. Scan only shadow lists here so shadow-only casters
		// cannot freeze while avoiding a second scan of every camera command.
		for(const PreparedRenderSubmission& prepared : preparedRenderSubmissions)
		{
			for(const ShadowDrawList& shadow : prepared.presentation.shadowDrawLists)
			{
				for(const std::uint32_t ordinal : shadow.stableOrdinals)
				{
					if(ordinal >= renderFrameSnapshot.renderables.size()
						|| renderFrameSnapshot.renderables[ordinal].animationBinding.status
						!= TerraEngine::Renderer::AnimationRenderBindingStatus::Ready)
					{
						continue;
					}
					if(ordinal < visibleAnimatedRenderableFlags.size())
					{
						visibleAnimatedRenderableFlags[ordinal] = 1u;
					}
				}
			}
		}
		visibility.visibleObjectIds.reserve(renderables.size());
		for(std::size_t index = 0u; index < renderables.size(); ++index)
		{
			if(visibleAnimatedRenderableFlags[index] != 0u)
			{
				visibility.visibleObjectIds.push_back(renderables[index].objectId);
			}
		}
		visibility.canonicalUnique = true;
		TerraEngine::Renderer::PublishAnimatedObjectVisibilitySnapshot(
			std::move(visibility));
	}

	// Visible and dedicated-capture records are published separately. This lets
	// automation inspect a custom camera without changing visible viewport picks.
	std::vector<GpuVirtualGeometryPacketRecord> completedSceneVisibilityRecords;
	std::vector<GpuVirtualGeometryPacketRecord> completedGameVisibilityRecords;
	std::vector<GpuVirtualGeometryPacketRecord> completedSceneCaptureVisibilityRecords;
	std::vector<GpuVirtualGeometryPacketRecord> completedGameCaptureVisibilityRecords;
	CaptureConventionalVisibilityIdentitySnapshot completedSceneCaptureIdentitySnapshot;
	CaptureConventionalVisibilityIdentitySnapshot completedGameCaptureIdentitySnapshot;
	bool sceneCaptureProcessed = false;
	bool gameCaptureProcessed = false;
	for(const PreparedRenderSubmission& prepared : preparedRenderSubmissions)
	{
		if(prepared.output.kind == RenderOutputKind::SceneView)
		{
			completedSceneVisibilityRecords.assign(
				prepared.presentation.VirtualGeometryPackets().begin(),
				prepared.presentation.VirtualGeometryPackets().end());
		}
		else if(prepared.output.kind == RenderOutputKind::GameView
			|| prepared.output.kind == RenderOutputKind::MainRuntimeView)
		{
			completedGameVisibilityRecords.assign(
				prepared.presentation.VirtualGeometryPackets().begin(),
				prepared.presentation.VirtualGeometryPackets().end());
		}
		else if(prepared.output.kind == RenderOutputKind::OffscreenTexture)
		{
			if(prepared.output.diagnosticsView == RenderViewType::Scene) {
				completedSceneCaptureVisibilityRecords.assign(
					prepared.presentation.VirtualGeometryPackets().begin(),
					prepared.presentation.VirtualGeometryPackets().end());
				completedSceneCaptureIdentitySnapshot.frameId =
					prepared.selection.generation;
				completedSceneCaptureIdentitySnapshot.rendererGeneration = snapshotRendererGeneration;
				completedSceneCaptureIdentitySnapshot.outputId = prepared.output.outputId;
				completedSceneCaptureIdentitySnapshot.originGeneration =
					prepared.presentation.referenceFrame.originGeneration;
				completedSceneCaptureIdentitySnapshot.spatialGeneration =
					prepared.presentation.referenceFrame.spatialGeneration;
				completedSceneCaptureIdentitySnapshot.residencyGeneration =
					prepared.presentation.referenceFrame.residencyGeneration;
				completedSceneCaptureIdentitySnapshot.table =
					renderFrameSnapshot.conventionalVisibilityIdentityTable;
				sceneCaptureProcessed = true;
			}
			else
			{
				completedGameCaptureVisibilityRecords.assign(
					prepared.presentation.VirtualGeometryPackets().begin(),
					prepared.presentation.VirtualGeometryPackets().end());
				completedGameCaptureIdentitySnapshot.frameId =
					prepared.selection.generation;
				completedGameCaptureIdentitySnapshot.rendererGeneration = snapshotRendererGeneration;
				completedGameCaptureIdentitySnapshot.outputId = prepared.output.outputId;
				completedGameCaptureIdentitySnapshot.originGeneration =
					prepared.presentation.referenceFrame.originGeneration;
				completedGameCaptureIdentitySnapshot.spatialGeneration =
					prepared.presentation.referenceFrame.spatialGeneration;
				completedGameCaptureIdentitySnapshot.residencyGeneration =
					prepared.presentation.referenceFrame.residencyGeneration;
				completedGameCaptureIdentitySnapshot.table =
					renderFrameSnapshot.conventionalVisibilityIdentityTable;
				gameCaptureProcessed = true;
			}
		}
	}

	// Materialization already merged every camera, shadow set, and dedicated
	// capture into this canonical renderable-index bitset. Reuse it directly;
	// rescanning draw, cluster, and packet streams would hash the same object
	// identity up to three times before deformation selection even begins.
	const AnimatedGeometryBuildResult animatedGeometry =
		buildAnimatedGeometryPlan(
			visibleAnimatedRenderableFlags,
			renderableCommandIndices);
	renderFrameSnapshot.animatedGeometryPageDemandCount =
		animatedGeometry.demandCount;
	TerraEngine::Renderer::DeformationBackendWorkPlan selectedDeformation;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Deformation Backend Plan",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		selectedDeformation = TerraEngine::Renderer::BuildDeformationBackendWorkPlan(
			activeRenderFrameId, animatedGeometry.immutablePlan);
	}
	// Seal one immutable virtual-geometry envelope only after every view has
	// produced its own visibility packet list. The oracle normalizes this union
	// into bounded IDs; it does not replace the per-view CPU cut that remains the
	// coverage authority until Vulkan reports completed device-stage evidence.
	const TerraEngine::Renderer::VirtualGeometryBackendSelection backendSelection =
		TerraEngine::Renderer::SelectVirtualGeometryBackend(backendCapabilities);
	const bool hasVirtualGeometryRoute = std::any_of(
		renderFrameSnapshot.renderables.begin(),
		renderFrameSnapshot.renderables.end(),
		[](const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable) {
			return renderable.geometryRouteDecision.selected
					== TerraEngine::Renderer::GeometryRoute::VirtualGeometryHardware
				|| renderable.geometryRouteDecision.selected
					== TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute;
		});
	if(backendSelection.deviceAuthoredWorkAvailable && hasVirtualGeometryRoute)
	{
		std::vector<TerraEngine::Renderer::VirtualGeometryOracleNode> oracleNodes;
		for(const PreparedRenderSubmission &prepared : preparedRenderSubmissions)
		{
			for(const GpuVirtualGeometryPacketRecord& packet
				: prepared.presentation.VirtualGeometryPackets())
			{
				TerraEngine::Renderer::VirtualGeometryOracleNode node{};
				// Packet identity is local to a view, so the sealed all-view envelope
				// uses an ordinal value. Its deterministic order follows the immutable
				// prepared-submission sequence, while packet metadata remains view-owned.
				node.id = static_cast<std::uint64_t>(oracleNodes.size()) + 1u;
				node.resident = packet.meshletCount != 0u
					&& (packet.flags & kVirtualGeometryFlagFailOpen) == 0u;
				node.error = 1.0f;
				oracleNodes.push_back(node);
			}
		}
		if(!oracleNodes.empty())
		{
			const std::uint32_t boundedCapacity = std::min<std::uint32_t>(
				backendSelection.commandCapacity,
				static_cast<std::uint32_t>(std::min<std::size_t>(
					oracleNodes.size(), std::numeric_limits<std::uint32_t>::max())));
			const TerraEngine::Renderer::VirtualGeometrySelectionResult oracleSelection =
				TerraEngine::Renderer::SelectVirtualGeometryOracle(
					oracleNodes, 0.0f, boundedCapacity);
			TerraEngine::Renderer::VirtualGeometryGpuPlanOptions planOptions{};
			planOptions.packetCapacity = boundedCapacity;
			planOptions.frameId = activeRenderFrameId;
			planOptions.generation = snapshotRendererGeneration;
			planOptions.generatedWorkKind = backendSelection.generatedWorkKind;
			planOptions.usesIndirectCount =
				backendCapabilities.virtualGeometryGeneratedWork.supportsIndirectCount;
			auto immutableVirtualGeometryPlan = std::make_shared<const TerraEngine::Renderer::VirtualGeometryGpuPlan>(
				TerraEngine::Renderer::BuildVirtualGeometryGpuPlan(
					oracleSelection, planOptions));
			if(immutableVirtualGeometryPlan->IsComplete())
			{
				// Existing Vulkan implementations may return no counter rows. In that
				// case the replay remains pending rather than inventing an authoritative
				// value from prepared CPU packet counts.
				latestVirtualGeometryReplayArtifacts =
					virtualGeometryReplayReadbacks.PublishAuthoritative(
						completedVirtualGeometryCounters);
				virtualGeometryReplayReadbacks.EnqueuePending(*immutableVirtualGeometryPlan);
			}
			else
			{
				renderGraphDiagnostics.backendFallbacks.push_back(
					"Virtual-geometry immutable device-work envelope was invalid; CPU oracle coverage remains authoritative.");
			}
		}
	}
	else if(!hasVirtualGeometryRoute)
	{
		// A direct-only frame intentionally has no VG backend envelope. Normal
		// resource uploads and conventional mesh-task draws continue separately.
		renderGraphDiagnostics.degradedWork.push_back(
			"Virtual-geometry feedback, decode, residency, compaction, and executor stages were suppressed for direct meshlet routes.");
	}
	else if(!backendSelection.fallbackReason.empty())
	{
		if(TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			renderGraphDiagnostics.backendFallbacks.push_back(backendSelection.fallbackReason);
		}
	}
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Seal Frame Publication",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto publicationSealStart = std::chrono::steady_clock::now();
		// Renderer recovery creates a new publication session.  Resetting before
		// building the candidate guarantees a pre-loss root is never paired with
		// post-recovery Vulkan resources.
		const std::uint64_t publicationSessionGeneration =
			std::max<std::uint64_t>(1u, snapshotRendererGeneration);
		if(renderFramePublicationStore.SessionGeneration()
			!= publicationSessionGeneration)
		{
			renderFramePublicationStore.ResetForSession(
				publicationSessionGeneration);
		}
		const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>
			previousPublication = renderFramePublicationStore.Load();
		auto publication = std::make_shared<TerraEngine::Renderer::RenderFramePublication>();
		publication->frameId = activeRenderFrameId;
		publication->referenceFrames.reserve(
			renderFrameSnapshot.viewRequests.size());
		for (const TerraEngine::Renderer::RenderFrameViewRequest& request :
			renderFrameSnapshot.viewRequests)
		{
			publication->referenceFrames.push_back(request.referenceFrame);
		}
		publication->rendererSessionGeneration = publicationSessionGeneration;
		publication->backendResourceGeneration = protectedBackendResourceGeneration;
		publication->sceneGeneration = protectedSceneGeneration;
		publication->animationPresentationGeneration = gpuPresentation != nullptr
			? gpuPresentation->presentationGeneration : 0u;
		publication->expectedPreviousGeneration = previousPublication != nullptr
			? previousPublication->generation : 0u;
		publication->generation = publication->expectedPreviousGeneration + 1u;
		renderGraphDiagnostics.publicationGeneration = publication->generation;
		renderGraphDiagnostics.rendererSessionGeneration = publicationSessionGeneration;
		const std::shared_ptr<const TerraEngine::Renderer::RenderPersistentTopologyRoot>
			allocatorTopology = retainedRenderTopology.slotAllocator.topology;
		if(allocatorTopology == nullptr
			|| allocatorTopology->rendererSessionGeneration != publicationSessionGeneration)
			throw std::runtime_error(
				"Renderer slot allocator has no topology root for the active session.");
		if(retainedPersistentTopologyRoot != nullptr)
		{
			const std::size_t sharedPageCount = std::min(
				retainedPersistentTopologyRoot->pages.size(), allocatorTopology->pages.size());
			for(std::size_t pageIndex = 0u; pageIndex < sharedPageCount; ++pageIndex)
			{
				if(retainedPersistentTopologyRoot->pages[pageIndex]
					== allocatorTopology->pages[pageIndex])
					++publication->receipt.reusedPersistentPages;
			}
		}
		retainedPersistentTopologyRoot = allocatorTopology;
		publication->persistent = allocatorTopology;
		const auto persistentPublicationEnd = std::chrono::steady_clock::now();

		auto dynamicRoot = std::make_shared<
			TerraEngine::Renderer::RenderDynamicStateRoot>();
		dynamicRoot->generation = std::max<std::uint64_t>(1u, activeRenderFrameId);
		dynamicRoot->rendererSessionGeneration = publicationSessionGeneration;
		// Dense GPU animation is a separate immutable publication root. Its pose and
		// plan generations advance every frame, but the renderer's geometry lanes do
		// not: transforms, resources, packet ordinals, and binding topology remain
		// stable. Keep those pages until a real geometry/binding mutation occurs and
		// resolve the current animation generations from backendWork at consumption.
		std::uint64_t dynamicSourceSignature = hasDenseAnimationPublication
			? renderableWorldSignature : renderableSubmissionSignature;
		if(hasDenseAnimationPublication) {
			HashCombine(dynamicSourceSignature,
				capturedAnimatorComponentRevision);
			HashCombine(dynamicSourceSignature, capturedHierarchyRevision);
			HashCombine(dynamicSourceSignature,
				capturedAnimationBindingResourceRevision);
			HashCombine(dynamicSourceSignature, renderFrameSnapshot.resourceRevision);
		}
		HashCombine(dynamicSourceSignature, allocatorTopology->generation);
		HashCombine(dynamicSourceSignature, virtualGeometryResidencyGeneration);
		HashCombine(dynamicSourceSignature, publicationSessionGeneration);
		const auto lanesEqual = [](const TerraEngine::Renderer::RenderDynamicLane& left,
			const TerraEngine::Renderer::RenderDynamicLane& right) noexcept
		{
			return left.key == right.key && left.transformGeneration == right.transformGeneration
				&& (left.shadowCasterInputs == right.shadowCasterInputs
					|| (left.shadowCasterInputs != nullptr && right.shadowCasterInputs != nullptr
						&& *left.shadowCasterInputs == *right.shadowCasterInputs))
				&& left.hasCanonicalWorldPose == right.hasCanonicalWorldPose
				&& left.canonicalWorldPose.position == right.canonicalWorldPose.position
				&& left.canonicalWorldPose.rotation == right.canonicalWorldPose.rotation
				&& left.canonicalWorldPose.scale == right.canonicalWorldPose.scale
				&& left.animationPresentationGeneration == right.animationPresentationGeneration
				&& left.animationBinding.animationAuthorityGeneration
					== right.animationBinding.animationAuthorityGeneration
				&& left.animationBinding.posePublicationGeneration
					== right.animationBinding.posePublicationGeneration
				&& left.animationBinding.source == right.animationBinding.source
				&& left.animationBinding.status == right.animationBinding.status
				&& left.animationBinding.safePose == right.animationBinding.safePose
				&& left.animationBinding.cpuDeformation
					== right.animationBinding.cpuDeformation
				&& left.animationBinding.cpuDeformationRecordIndex
					== right.animationBinding.cpuDeformationRecordIndex
				&& left.animation == right.animation
				&& left.materialOverrideGeneration == right.materialOverrideGeneration
				&& left.boundsGeneration == right.boundsGeneration
				&& left.lifecycleGeneration == right.lifecycleGeneration
				&& left.resourceGeneration == right.resourceGeneration
				&& left.occupied == right.occupied
				&& std::memcmp(&left.previousModelMatrix, &right.previousModelMatrix,
					sizeof(glm::mat4)) == 0
				&& std::memcmp(&left.instance, &right.instance,
					sizeof(RenderInstanceRecord)) == 0;
		};
		const std::shared_ptr<const TerraEngine::Renderer::RenderDynamicStateRoot>
			previousDynamicRoot = retainedDynamicStateRoot != nullptr
				&& retainedDynamicStateRoot->rendererSessionGeneration
					== publicationSessionGeneration
				? retainedDynamicStateRoot : nullptr;
		const bool reusableDynamicBase = previousDynamicRoot != nullptr
			&& retainedDynamicStateSourceSignature == dynamicSourceSignature
			&& previousDynamicRoot->pages.size() == publication->persistent->pages.size();
		std::vector<std::uint32_t> currentCpuDynamicPageIndices;
		bool cpuPageMappingComplete = true;
		if(hasDenseAnimationPublication) {
			currentCpuDynamicPageIndices.reserve(
				cpuDeformationRenderableIndices.size());
			for(const std::uint32_t renderableIndex :
				cpuDeformationRenderableIndices) {
				if(renderableIndex >= retainedRenderTopology.orderedSlotKeys.size()) {
					cpuPageMappingComplete = false;
					break;
				}
				const TerraEngine::Renderer::RenderSlotGenerationKey key =
					retainedRenderTopology.orderedSlotKeys[renderableIndex];
				if(!key.IsValid()) {
					cpuPageMappingComplete = false;
					break;
				}
				currentCpuDynamicPageIndices.push_back(
					key.ordinal
						/ TerraEngine::Renderer::kRenderPublicationPageCapacity);
			}
			std::sort(currentCpuDynamicPageIndices.begin(),
				currentCpuDynamicPageIndices.end());
			currentCpuDynamicPageIndices.erase(std::unique(
				currentCpuDynamicPageIndices.begin(),
				currentCpuDynamicPageIndices.end()),
				currentCpuDynamicPageIndices.end());
		}
		const bool retainCompleteDynamicRoot = reusableDynamicBase
			&& cpuPageMappingComplete
			&& currentCpuDynamicPageIndices.empty()
			&& retainedCpuDynamicPageIndices.empty();
		std::vector<std::shared_ptr<const TerraEngine::Renderer::RenderDynamicPage>>
			preparedDynamicPages(publication->persistent->pages.size());
		std::vector<std::uint8_t> changedDynamicPages(
			publication->persistent->pages.size(), 0U);
		std::vector<std::uint8_t> pagesRequiringDynamicRefresh(
			publication->persistent->pages.size(), reusableDynamicBase ? 0U : 1U);
		if(reusableDynamicBase) {
			preparedDynamicPages = previousDynamicRoot->pages;
			const auto markCpuPages = [&pagesRequiringDynamicRefresh](
				const std::vector<std::uint32_t>& pageIndices) noexcept {
				for(const std::uint32_t pageIndex : pageIndices) {
					if(pageIndex < pagesRequiringDynamicRefresh.size()) {
						pagesRequiringDynamicRefresh[pageIndex] = 1U;
					}
				}
			};
			markCpuPages(retainedCpuDynamicPageIndices);
			markCpuPages(currentCpuDynamicPageIndices);
		}
		if(!cpuPageMappingComplete) {
			std::fill(pagesRequiringDynamicRefresh.begin(),
				pagesRequiringDynamicRefresh.end(), 1U);
		}
		// Keep a few retired page sets ready for the asynchronous backend without
		// allowing an unusually slow consumer to turn this cache into unbounded
		// retained memory. Acquire itself refuses to hand out any page still owned
		// by a sealed publication.
		const std::size_t dynamicPagePoolLimit = std::max<std::size_t>(
			16u, publication->persistent->pages.size() * 3u);
		// Each immutable page owns an independent 256-lane range. Build those ranges
		// concurrently and join before publishing the root; no worker touches live
		// ECS state or another page, so ordering and atomic visibility remain intact.
		if(!retainCompleteDynamicRoot)
		{
                  RunRenderChunkedWork(
                      publication->persistent->pages.size(),
                      renderGraphWorkerCount, 2U,
                      "RenderSystem Task: Build Dynamic Publication Pages",
                      [&](std::size_t pageBegin, std::size_t pageEnd) {
                        for (std::size_t pageIndex = pageBegin;
                             pageIndex < pageEnd; ++pageIndex) {
						  if(pagesRequiringDynamicRefresh[pageIndex] == 0U)
							continue;
                          const std::shared_ptr<
                              const TerraEngine::Renderer::RenderPersistentPage>
                              &persistentPage =
                                  publication->persistent->pages[pageIndex];
                          if (persistentPage == nullptr)
                            throw std::runtime_error(
                                "Renderer dynamic publication cannot address a "
                                "missing persistent page.");
						  auto page = dynamicPageRecyclePool.Acquire(
						      dynamicPagePoolLimit);
						  // This page has no publication owner when Acquire returns it.
						  // Clearing retains vector capacity, avoiding repeated 256-lane
						  // heap allocations for every stable animated crowd frame.
						  page->lanes.clear();
						  page->changedLanes.clear();
						  page->generation = dynamicRoot->generation;
                          page->firstOrdinal = persistentPage->firstOrdinal;
                          page->lanes.reserve(persistentPage->slots.size());
                          page->changedLanes.reserve(
                              persistentPage->slots.size());
                          const std::shared_ptr<
                              const TerraEngine::Renderer::RenderDynamicPage>
                              previousPage =
                                  previousDynamicRoot != nullptr &&
                                          pageIndex <
                                              previousDynamicRoot->pages.size()
                                      ? previousDynamicRoot->pages[pageIndex]
                                      : nullptr;
                          bool anyLaneChanged =
                              previousPage == nullptr ||
                              previousPage->lanes.size() !=
                                  persistentPage->slots.size();
                          for (std::size_t laneIndex = 0u;
                               laneIndex < persistentPage->slots.size();
                               ++laneIndex) {
                            const TerraEngine::Renderer::RenderPersistentSlot
                                &persistent = persistentPage->slots[laneIndex];
                            TerraEngine::Renderer::RenderDynamicLane lane{};
                            if (!persistent.occupied) {
                              const bool laneChanged =
                                  previousPage == nullptr ||
                                  laneIndex >= previousPage->lanes.size() ||
                                  previousPage->lanes[laneIndex].occupied;
                              page->changedLanes.push_back(laneChanged);
                              page->lanes.push_back(std::move(lane));
                              anyLaneChanged = anyLaneChanged || laneChanged;
                              continue;
                            }
                            const std::uint32_t ordinal =
                                persistent.key.ordinal;
                            const std::uint32_t renderableIndex =
                                ordinal < retainedRenderTopology
                                              .renderableIndexByOrdinal.size()
                                    ? retainedRenderTopology
                                          .renderableIndexByOrdinal[ordinal]
                                    : TerraEngine::Renderer::
                                          RenderSlotGenerationKey::
                                              kInvalidOrdinal;
                            if (renderableIndex == TerraEngine::Renderer::
                                                       RenderSlotGenerationKey::
                                                           kInvalidOrdinal ||
                                renderableIndex >=
                                    renderFrameSnapshot.renderables.size() ||
                                renderFrameSnapshot.renderables[renderableIndex]
                                        .objectId != persistent.objectId)
                              throw std::runtime_error(
                                  "Renderer persistent slot has no matching "
                                  "captured dynamic record.");
                            const TerraEngine::Renderer::
                                RenderFrameRenderableRecord &renderable =
                                    renderFrameSnapshot
                                        .renderables[renderableIndex];
                            lane.key = persistent.key;
							// Retained pages are view-independent. The precise canonical pose
							// below is localized by each sealed view's command resolver.
							lane.instance.modelMatrix = glm::mat4(1.0F);
							lane.canonicalWorldPose = renderable.canonicalWorldPose;
							lane.hasCanonicalWorldPose = true;
                            lane.previousModelMatrix =
                                previousPage != nullptr &&
                                        laneIndex <
                                            previousPage->lanes.size() &&
                                        previousPage->lanes[laneIndex].key ==
                                            lane.key &&
                                        previousPage->lanes[laneIndex].occupied
                                    ? previousPage->lanes[laneIndex]
                                          .instance.modelMatrix
									: glm::mat4(1.0F);
                            lane.instance.objectIdentity = glm::uvec4(
                                renderable.conventionalVisibilityIdentityIndex,
                                0u, 0u, 0u);
                            lane.instance.materialAndTexture = glm::uvec4(
                                0u, 0u, persistent.key.ordinal,
                                renderable.conventionalVisibilityIdentityIndex);
                            lane.instance.animationSelectors = glm::uvec4(
                                static_cast<std::uint32_t>(
                                    renderable.animation.stableInstanceId),
                                static_cast<std::uint32_t>(
                                    renderable.animation
                                        .animationPlanGeneration),
                                renderable.animation.firstGpuWorkPacket,
                                renderable.animation.gpuStageMask);
							lane.shadowCasterInputs = renderable.shadowCasterInputs;
                            lane.animationBinding = TerraEngine::Renderer::
                                PublishedAnimationRenderBinding(
                                    renderable.animationBinding);
                            lane.animation = BuildPublishedAnimationGeneration(
                                renderable.animation);
                            lane.instance.geometrySelectors = glm::uvec4(
                                renderable.virtualGeometry.selectedLodIndex,
                                renderable.virtualGeometry
                                    .payloadDescriptorIndex,
                                renderable.virtualGeometry.meshletOffset,
                                renderable.virtualGeometry.meshletCount);
							lane.transformGeneration =
								renderable.transformGeneration;
                            lane.animationPresentationGeneration =
                                renderable.animation.animationPlanGeneration;
                            lane.materialOverrideGeneration =
                                renderable.invalidation.material;
                            lane.boundsGeneration =
                                renderable.invalidation.asset;
                            lane.lifecycleGeneration =
                                renderable.invalidation.renderer;
                            lane.resourceGeneration =
                                renderable.invalidation.residency;
                            lane.occupied = true;
                            const bool laneChanged =
                                previousPage == nullptr ||
                                laneIndex >= previousPage->lanes.size() ||
                                !lanesEqual(previousPage->lanes[laneIndex],
                                            lane);
                            page->changedLanes.push_back(laneChanged);
                            page->lanes.push_back(std::move(lane));
                            anyLaneChanged = anyLaneChanged || laneChanged;
                          }
                          // The worker has now constructed and compared every
                          // fixed-address lane against this exact immutable
                          // topology page. Seal consumes this proof and retains
                          // the defensive scan for pages built by
                          // compatibility/test callers.
                          page->producerValidatedLaneCount = page->lanes.size();
                          page->producerValidatedPersistentGeneration =
                              persistentPage->generation;
                          if (!anyLaneChanged && previousPage != nullptr) {
                            preparedDynamicPages[pageIndex] = previousPage;
                          } else {
                            preparedDynamicPages[pageIndex] = std::move(page);
                            changedDynamicPages[pageIndex] = 1U;
                          }
                        }
                      });
                  dynamicRoot->pages = std::move(preparedDynamicPages);
                  publication->receipt.changedDynamicPages =
                      static_cast<std::uint32_t>(
                          std::count(changedDynamicPages.begin(),
                                     changedDynamicPages.end(), 1U));
                  retainedDynamicStateRoot = dynamicRoot;
                  retainedDynamicStateSourceSignature = dynamicSourceSignature;
				  retainedCpuDynamicPageIndices =
					  std::move(currentCpuDynamicPageIndices);
                  publication->dynamic = std::move(dynamicRoot);
                } else {
                  // The source signature includes topology, transforms,
                  // animation/resource generations, and VG residency.
                  // Camera-dependent selections are separate roots, so moving a
                  // camera does not invalidate scene-dynamic data.
                  publication->receipt.changedDynamicPages = 0U;
                  publication->dynamic = previousDynamicRoot;
                  retainedDynamicStateRoot = previousDynamicRoot;
				  retainedCpuDynamicPageIndices =
					  std::move(currentCpuDynamicPageIndices);
                }
                TerraEngine::Renderer::AnimationBackendWorkPlan animationBackendWork;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Animation Backend Work",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			animationBackendWork = hasDenseAnimationPublication
				? TerraEngine::Renderer::BuildDenseAnimationBackendWorkPlan(
					activeRenderFrameId, denseAnimationOwnerCount,
					immutableGpuAnimationPlan)
				: TerraEngine::Renderer::BuildAnimationBackendWorkPlan(
					activeRenderFrameId, *publication->dynamic,
					immutableGpuAnimationPlan);
		}
		if(cpuDeformationPlan != nullptr)
		{
			animationBackendWork.cpuOrFallbackAnimatorCount =
				cpuDeformationPlan->records.size();
			animationBackendWork.cpuDeformationPlan = std::move(cpuDeformationPlan);
		}
		const bool backendWorkValid = activeRenderFrameId != 0u
			&& animationBackendWork.frameId == activeRenderFrameId
			&& animationBackendWork.valid
			&& selectedDeformation.frameId == activeRenderFrameId
			&& selectedDeformation.valid
			&& (!selectedDeformation.immutablePlan
				|| selectedDeformation.immutablePlan->IsComplete());
		if(!backendWorkValid)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Renderer rejected selected animation or deformation work for frame "
					<< activeRenderFrameId << ".");
		}
		auto backendWork = std::make_shared<
			TerraEngine::Renderer::RenderBackendWorkPublication>();
		backendWork->frameId = activeRenderFrameId;
		backendWork->animationWork = std::move(animationBackendWork);
		backendWork->deformationWork = std::move(selectedDeformation.immutablePlan);
		backendWork->valid = backendWorkValid;
		publication->backendWork = std::move(backendWork);
		const auto dynamicPublicationEnd = std::chrono::steady_clock::now();

		for(PreparedRenderSubmission& prepared : preparedRenderSubmissions)
		{
			const std::uint32_t viewIndex = static_cast<std::uint32_t>(publication->views.size());
			std::vector<TerraEngine::Renderer::RenderSubmissionRange> ranges =
				TerraEngine::Renderer::BuildRenderSubmissionRanges(
					prepared.selection, viewIndex);
			publication->submissionRanges.insert(publication->submissionRanges.end(),
				std::make_move_iterator(ranges.begin()),
				std::make_move_iterator(ranges.end()));
			publication->views.push_back(std::move(prepared.selection));
			publication->presentationInputs.push_back(std::move(prepared.presentation));
		}
		// Resource readiness crosses the backend boundary once as compact identities.
		// Per-view ranges resolve these shared rows by stable resource UUID instead of
		// retaining the worker-only snapshot's mesh, material, or texture vectors.
		std::vector<TerraEngine::Renderer::RenderResourcePreparationRecord>
			resourcePreparationRecords;
		resourcePreparationRecords.reserve(
			renderFrameSnapshot.meshes.size()
			+ renderFrameSnapshot.materials.size()
			+ renderFrameSnapshot.textures.size());
		const auto appendResourcePreparation = [&resourcePreparationRecords,
			&renderFrameSnapshot](
			const std::vector<TerraEngine::Renderer::RenderFrameResourceRecord>& records)
		{
			for(const TerraEngine::Renderer::RenderFrameResourceRecord& record : records)
			{
				if(record.resourceId == TerraEngine::EMPTY_UUID)
				{
					continue;
				}
				resourcePreparationRecords.push_back({
					record.resourceId,
					std::max<std::uint64_t>(1u, renderFrameSnapshot.resourceRevision),
					0u,
					record.resident,
					record.usingFallback});
			}
		};
		appendResourcePreparation(renderFrameSnapshot.meshes);
		appendResourcePreparation(renderFrameSnapshot.materials);
		appendResourcePreparation(renderFrameSnapshot.textures);
		publication->resourcePreparation =
			TerraEngine::Renderer::BuildRenderResourcePreparationTable(
				resourcePreparationRecords,
				std::max<std::uint64_t>(1u, renderFrameSnapshot.resourceRevision));
		const auto viewPublicationEnd = std::chrono::steady_clock::now();
		// Read only the completed immutable roots. Each task writes its own light
		// list, and settlement precedes publication; no new scene flush is needed.
		std::vector<std::future<void>> shadowStreamFutures;
		TerraEngine::Tasks::ScopedTaskSettlement<void> shadowStreamSettlement(shadowStreamFutures);
		try
		{
			for (auto& presentation : publication->presentationInputs)
				for (auto& shadow : presentation.shadowDrawLists)
				{
					const auto seal = [&publication, &presentation, &shadow]() {
						shadow.instanceInputs = TerraEngine::Renderer::BuildShadowPreparedInstanceInputs(
							*publication, presentation, shadow);
					};
					if (presentation.efficiencyOptions.options.validationPreparation
						== TerraEngine::Renderer::RendererEfficiencyPreparation::Concurrent)
						shadowStreamSettlement.CreateAndEnqueue(ThreadPool::instance(), seal);
					else shadowStreamSettlement.RunInline(seal);
				}
			shadowStreamSettlement.GetAll();
		}
		catch (...)
		{
			shadowStreamSettlement.Wait();
			for (auto& presentation : publication->presentationInputs)
				for (auto& shadow : presentation.shadowDrawLists) shadow.instanceInputs.reset();
		}
		const TerraEngine::Renderer::SealedRenderFramePublication sealedPublication =
			TerraEngine::Renderer::SealRenderFramePublication(
				std::move(publication), publicationSessionGeneration);
		const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>&
			sealedRoot = sealedPublication.Publication();
		const TerraEngine::Renderer::RenderPublicationReceipt publicationReceipt =
			renderFramePublicationStore.TryPublish(sealedPublication,
				publicationSessionGeneration);
		if(sealedRoot != nullptr && publicationReceipt.IsSuccess())
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Prepare Backend Work Caches",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			rendererPreparationSummary = rendererPreparationCache.BuildFramePreparation(
				*sealedRoot,
				renderGraphPasses,
				backendCapabilities,
				TerraEngine::Renderer::RenderDiagnosticsRequested());
		}
		const auto protectedPublicationEnd = std::chrono::steady_clock::now();
		// Profiler evidence is copied only while recording is active. Missing
		// producers stay unavailable; successful and rejected publications both
		// retain their exact generation/outcome tuple for diagnosis.
		TerraEngine::Profiling::ProfilerFramePublicationEvidence evidence{};
		evidence.availabilityMask = 0x7fu;
		evidence.frameGeneration = sealedRoot != nullptr ? sealedRoot->generation : 0u;
		evidence.sceneGeneration = sealedRoot != nullptr ? sealedRoot->sceneGeneration : 0u;
		evidence.renderSessionGeneration = publicationSessionGeneration;
		evidence.topologyGeneration = sealedRoot != nullptr && sealedRoot->persistent != nullptr
			? sealedRoot->persistent->generation : 0u;
		evidence.dynamicGeneration = sealedRoot != nullptr && sealedRoot->dynamic != nullptr
			? sealedRoot->dynamic->generation : 0u;
		evidence.renderOutcome = static_cast<std::uint32_t>(
			publicationReceipt.outcome);
		evidence.rejectionReason = publicationReceipt.IsSuccess()
			? 0u : evidence.renderOutcome;
		evidence.persistentReuseCount = publicationReceipt.reusedPersistentPages;
		evidence.persistentRebuildCount = sealedRoot != nullptr && sealedRoot->persistent != nullptr
			? static_cast<std::uint32_t>(sealedRoot->persistent->pages.size()) - std::min<std::uint32_t>(
				publicationReceipt.reusedPersistentPages,
				static_cast<std::uint32_t>(sealedRoot->persistent->pages.size())) : 0u;
		evidence.dynamicRebuildCount = publicationReceipt.changedDynamicPages;
		evidence.dynamicReuseCount = sealedRoot != nullptr && sealedRoot->dynamic != nullptr
			? static_cast<std::uint32_t>(sealedRoot->dynamic->pages.size()) - std::min<std::uint32_t>(
				publicationReceipt.changedDynamicPages,
				static_cast<std::uint32_t>(sealedRoot->dynamic->pages.size())) : 0u;
		evidence.viewSelectionRebuildCount = sealedRoot != nullptr
			? static_cast<std::uint32_t>(sealedRoot->views.size()) : 0u;
		const auto elapsedNanoseconds = [](const auto begin, const auto end) {
			return static_cast<std::uint64_t>(std::max<std::int64_t>(0,
				std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
		};
		evidence.disjointTimingNanoseconds[0] = elapsedNanoseconds(
			publicationSealStart, persistentPublicationEnd);
		evidence.disjointTimingNanoseconds[1] = elapsedNanoseconds(
			persistentPublicationEnd, dynamicPublicationEnd);
		evidence.disjointTimingNanoseconds[2] = elapsedNanoseconds(
			dynamicPublicationEnd, viewPublicationEnd);
		evidence.disjointTimingNanoseconds[4] = elapsedNanoseconds(
			viewPublicationEnd, protectedPublicationEnd);
		evidence.disjointTimingNanoseconds[5] = elapsedNanoseconds(
			backendPreparationStart, backendPreparationEnd);
		if(animationPresentationFrame != nullptr
			&& animationPresentationFrame->authority != nullptr)
		{
			const auto& authority = *animationPresentationFrame->authority;
			evidence.authorityGeneration = authority.authorityGeneration;
			evidence.presentationGeneration =
				animationPresentationFrame->presentationGeneration;
			evidence.animationOutcome = static_cast<std::uint32_t>(
				authority.receipt.outcome);
			evidence.authorityReuseCount = authority.receipt.reusedPages;
			evidence.authorityRebuildCount = authority.receipt.changedPages;
		}
		evidence.viewCount = sealedRoot != nullptr ? static_cast<std::uint32_t>(std::min<std::size_t>(
			sealedRoot->presentationInputs.size(),
			TerraEngine::Profiling::ProfilerFramePublicationEvidence::kMaximumViews)) : 0u;
		for(std::size_t index = 0u; index < evidence.viewCount; ++index)
		{
			const RenderOptimizationSummary& source =
				sealedRoot->presentationInputs[index].renderOptimization;
			auto& destination = evidence.views[index];
			destination.viewType = static_cast<std::uint32_t>(
				sealedRoot->views[index].viewType);
			destination.frameGeneration = sealedRoot->generation;
			destination.topologyGeneration = source.topologyGeneration;
			destination.viewGeneration = sealedRoot->views[index].generation;
			destination.rendererGeneration = source.rendererGeneration;
			destination.topologyBuildNanoseconds = source.topologyBuildNanoseconds;
			destination.dynamicMaterializationNanoseconds =
				source.dynamicMaterializationNanoseconds;
			destination.instanceStreamUploadNanoseconds =
				source.instanceStreamUploadNanoseconds;
			destination.groupedCommandRecordingNanoseconds =
				source.groupedCommandRecordingNanoseconds;
			destination.visibleInstanceCount = source.visibleInstanceCount;
			destination.groupedInstanceCount = source.groupedInstanceCount;
			destination.fallbackInstanceCount = source.fallbackInstanceCount;
			destination.groupedSubmissionCount = source.groupedSubmissionCount;
			destination.fallbackSubmissionCount = source.fallbackSubmissionCount;
			destination.avoidedSubmissionCount = source.avoidedSubmissionCount;
			destination.instanceStreamBytes = source.instanceStreamBytes;
			destination.instanceStreamCapacityBytes = source.instanceStreamCapacityBytes;
			destination.topologyReuseHits = source.topologyReuseHits;
			destination.topologyInvalidations = source.topologyInvalidations;
			destination.viewEvidenceReuseHits = source.viewEvidenceReuseHits;
			destination.viewEvidenceInvalidations = source.viewEvidenceInvalidations;
			destination.rejectedIncompleteKeyCount =
				source.rejectedIncompleteKeyCount;
			destination.rejectedUnsupportedVariantCount =
				source.rejectedUnsupportedVariantCount;
			destination.rejectedOutOfRangeCount = source.rejectedOutOfRangeCount;
			destination.rejectedStaleGenerationCount =
				source.rejectedStaleGenerationCount;
			destination.fallbackAllocationFailureCount =
				source.fallbackAllocationFailureCount;
			destination.fallbackRecoveryCount = source.fallbackRecoveryCount;
			destination.valid = source.valid != 0u;
		}
		pendingProfilerPublicationEvidence = evidence;
		if(!publicationReceipt.IsSuccess())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Render frame publication was rejected for frame " << activeRenderFrameId
					<< ": " << publicationReceipt.DiagnosticText());
		}
	}

	// Boundary 3: every fallible publication and per-view plan is sealed. Only
	// protected backend ownership and final diagnostics remain below this point.
	co_await std::suspend_always{};
	if(pendingProfilerPublicationEvidence.has_value())
	{
		TerraEngine::Profiling::Profiler::Instance()
			.RecordFramePublicationEvidence(*pendingProfilerPublicationEvidence);
	}
	for(const auto& [viewType, touchSet] : pendingStreamingTouchSets)
	{
		const char* viewReason = viewType == RenderViewType::Scene
			? "visible scene view renderable" : "visible game view renderable";
		resourceManager.PublishVisibleStreamingTouches(
			touchSet, activeRenderFrameId, viewReason);
	}
	rhi->SetGlobalTaskShaderCullingEnabled(taskShaderCullingEnabled);
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI Begin Frame", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->BeginFrame();
	}
	// BeginFrame first retires and clears the reused backend frame slot. Publish
	// the sealed all-view plan only after that reset; publishing it earlier would
	// let Vulkan erase pending compute work before the first view records it.
	if(!rhi->ConsumeRenderFramePublication(renderFramePublicationStore.Load()))
	{
		TERRAENGINE_LOG(LogLevel::Error,
			"Render backend rejected frame " << activeRenderFrameId
				<< " immutable publication before command recording.");
	}
	const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>
		sealedPublication = renderFramePublicationStore.Load();
	for(std::uint32_t publicationViewIndex = 0u;
		sealedPublication != nullptr
		&& publicationViewIndex < sealedPublication->views.size(); ++publicationViewIndex)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI Render Prepared View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->RenderFrame(sealedPublication, publicationViewIndex);
	}
	// Capture target generations are backend-owned and become authoritative only
	// after the corresponding offscreen view has been rendered. Attach that exact
	// tuple before exposing the retained identity table to asynchronous picking.
	if(sceneCaptureProcessed)
	{
		const auto metadata = rhi->GetConventionalVisibilityIdentityTargetMetadata(
			RenderViewType::Scene, true);
		if(metadata && metadata->frameId == completedSceneCaptureIdentitySnapshot.frameId
			&& metadata->rendererGeneration
				== completedSceneCaptureIdentitySnapshot.rendererGeneration
			&& metadata->originGeneration
				== completedSceneCaptureIdentitySnapshot.originGeneration
			&& metadata->spatialGeneration
				== completedSceneCaptureIdentitySnapshot.spatialGeneration
			&& metadata->residencyGeneration
				== completedSceneCaptureIdentitySnapshot.residencyGeneration)
			completedSceneCaptureIdentitySnapshot.targetGeneration =
				metadata->targetGeneration;
	}
	if(gameCaptureProcessed)
	{
		const auto metadata = rhi->GetConventionalVisibilityIdentityTargetMetadata(
			RenderViewType::Game, true);
		if(metadata && metadata->frameId == completedGameCaptureIdentitySnapshot.frameId
			&& metadata->rendererGeneration
				== completedGameCaptureIdentitySnapshot.rendererGeneration
			&& metadata->originGeneration
				== completedGameCaptureIdentitySnapshot.originGeneration
			&& metadata->spatialGeneration
				== completedGameCaptureIdentitySnapshot.spatialGeneration
			&& metadata->residencyGeneration
				== completedGameCaptureIdentitySnapshot.residencyGeneration)
			completedGameCaptureIdentitySnapshot.targetGeneration =
				metadata->targetGeneration;
	}
	if (!captureRequestsForReadback.empty())
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for (TerraEngine::Renderer::RenderCaptureRequest& captureRequest : captureRequestsForReadback)
		{
			submittedCaptureReadbackRequests.push_back(std::move(captureRequest));
		}
	}
	if (TerraEngine::Renderer::RenderDiagnosticsRequested())
	{
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Visible draw command cache hits: "
			+ std::to_string(visibleCommandCache.hitCount)
			+ "/"
			+ std::to_string(visibleCommandCache.hitCount + visibleCommandCache.missCount));
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Shadow draw list cache hits: "
			+ std::to_string(shadowDrawListCache.hitCount)
			+ "/"
			+ std::to_string(shadowDrawListCache.hitCount + shadowDrawListCache.missCount));
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Persistent render packet cache hits: "
			+ std::to_string(rendererPreparationSummary.renderPacketCache.cacheHits)
			+ "/"
			+ std::to_string(rendererPreparationSummary.renderPacketCache.packetCount));
	}
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI End Frame", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->EndFrame();
	}
	TerraEngine::Renderer::ApplyVirtualGeometryCapabilityDiagnostics(
		renderGraphDiagnostics,
		CollectVirtualGeometryCapabilityDiagnostics(
			*rhi,
			backendCapabilities,
			{
				activeRenderFrameId,
				renderFrameSnapshot.viewRequests.front().outputId,
				snapshotRendererGeneration
			}));

	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Publish Render Stats", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> virtualGeometryViewSnapshots;
		const VirtualGeometryQualityProfile virtualGeometryQuality = renderFrameSnapshot.renderSettings.virtualGeometry;
		const bool detailedRenderDiagnosticsRequested = TerraEngine::Renderer::RenderDiagnosticsRequested();
		// The stats overlay only needs live frame-selection counters. Full asset
		// inventory diagnostics scan every cooked VG asset and allocate warning
		// strings, so keep that work behind the explicit diagnostics request.
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Compact Render Diagnostics",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			virtualGeometryViewSnapshots =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					renderFrameSnapshot,
					virtualGeometryQuality,
					GetVirtualGeometryInactiveReason(),
					// Ownership moved into the sealed backend publication above. Read the
					// retained immutable root instead of the moved-from selector; otherwise
					// diagnostics falsely report a missing deformation plan on every valid
					// animated frame even though Vulkan receives the completed work.
					sealedPublication && sealedPublication->backendWork
						? sealedPublication->backendWork->deformationWork.get()
						: nullptr,
					detailedRenderDiagnosticsRequested);
		}
		if (detailedRenderDiagnosticsRequested)
		{
			std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> detailedVirtualGeometryViewSnapshots;
			detailedVirtualGeometryViewSnapshots.push_back(TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, activeRenderFrameId, gameVirtualGeometryDiagnosticsAssets, virtualGeometryQuality));
			detailedVirtualGeometryViewSnapshots.push_back(TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Scene, activeRenderFrameId, sceneVirtualGeometryDiagnosticsAssets, virtualGeometryQuality));
			bool shadowDiagnosticsPublished = false;
			{
				TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot shadowSnapshot =
					TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Shadow, activeRenderFrameId, shadowVirtualGeometryDiagnosticsAssets, virtualGeometryQuality);
				shadowSnapshot.hiZActive = true;
				shadowSnapshot.hiZHistoryValid = true;
				shadowSnapshot.hiZInvalidReason.clear();
				shadowSnapshot.warnings.push_back("Shadow virtual geometry uses conservative visibility; camera Hi-Z ownership is diagnostic-only.");
				detailedVirtualGeometryViewSnapshots.push_back(std::move(shadowSnapshot));
				shadowDiagnosticsPublished = true;
			}
			const auto mergeSelectionDiagnostics = [](
				TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& target,
				const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& selection) {
				// The quality snapshot describes cooked assets and residency. The
				// selection snapshot describes what this frame actually dispatched.
				// Merge the live fields into the inventory view so editor overlays
				// do not stop on a stale zero-count Game/Scene snapshot.
				target.active = target.active || selection.active;
				target.inactiveReason = selection.inactiveReason;
				target.selectedMeshlets = selection.selectedMeshlets;
				target.dispatchedMeshlets = selection.dispatchedMeshlets;
				target.culledMeshlets = selection.culledMeshlets;
				target.culledByFrustum = selection.culledByFrustum;
				target.culledByCone = selection.culledByCone;
				target.culledByHiZ = selection.culledByHiZ;
				target.clusterPacketCount = selection.clusterPacketCount;
				target.candidateClusterCount = selection.candidateClusterCount;
				target.compactedClusterCount = selection.compactedClusterCount;
				target.rejectedClusterCount = selection.rejectedClusterCount;
				target.failOpenClusterCount = selection.failOpenClusterCount;
				target.submittedBatchCount = selection.submittedBatchCount;
				target.singlePacketFallbackBatchCount = selection.singlePacketFallbackBatchCount;
				target.hiZActive = selection.hiZActive;
				target.hiZHistoryValid = selection.hiZHistoryValid;
				target.hiZInvalidReason = selection.hiZInvalidReason;
				target.hiZState = selection.hiZState;
				target.hiZPyramidSize = selection.hiZPyramidSize;
				target.hiZRenderSize = selection.hiZRenderSize;
				target.hiZCameraSignature = selection.hiZCameraSignature;
				target.hiZDepthSceneSignature = selection.hiZDepthSceneSignature;
				target.hiZWarmupFrames = selection.hiZWarmupFrames;
				target.hiZBaselineMatched = target.hiZBaselineMatched && selection.hiZBaselineMatched;
				target.hiZFalseOcclusionRiskWarnings += selection.hiZFalseOcclusionRiskWarnings;
				target.clusterSelection = selection.clusterSelection;
				target.hiZValidation = selection.hiZValidation;
				// Animated-page facts come from the same live selection snapshot.
				// Preserve them when detailed cooked-asset inventory is enabled;
				// otherwise opening diagnostics would replace accepted compute work
				// with the inventory snapshot's default zero values.
				target.animatedPageDemandCount =
					selection.animatedPageDemandCount;
				target.animatedUniqueDispatchCount =
					selection.animatedUniqueDispatchCount;
				target.animatedReuseCount =
					selection.animatedReuseCount;
				target.animatedAnimationLods =
					selection.animatedAnimationLods;
				target.animatedSkippedWorkCount =
					selection.animatedSkippedWorkCount;
				target.animatedBoundsSource =
					selection.animatedBoundsSource;
				target.animatedFallbackCount =
					selection.animatedFallbackCount;
				target.deformationCurrentBytes =
					selection.deformationCurrentBytes;
				target.deformationHistoryBytes =
					selection.deformationHistoryBytes;
				target.deformationBoundsBytes =
					selection.deformationBoundsBytes;
				target.deformationHighWaterBytes =
					selection.deformationHighWaterBytes;
				target.animatedDiagnosticEmitted =
					selection.animatedDiagnosticEmitted;
				target.animatedDiagnosticCoalesced =
					selection.animatedDiagnosticCoalesced;
				target.animatedDiagnosticDropped =
					selection.animatedDiagnosticDropped;
				target.animatedShaderContractValid =
					selection.animatedShaderContractValid;
				target.animatedFallbackReason =
					selection.animatedFallbackReason;
				target.virtualGeometryPerformanceGates.insert(
					target.virtualGeometryPerformanceGates.end(),
					selection.virtualGeometryPerformanceGates.begin(),
					selection.virtualGeometryPerformanceGates.end());
				target.selectedLodHistogram = selection.selectedLodHistogram;
				target.occlusionDebugSamples = selection.occlusionDebugSamples;
				if (selection.sourceMeshlets > 0u)
				{
					target.sourceMeshlets = std::max(target.sourceMeshlets, selection.sourceMeshlets);
				}
				if (selection.requestedPages > 0u || selection.missingPages > 0u)
				{
					target.requestedPages = std::max(target.requestedPages, selection.requestedPages);
					target.missingPages = std::max(target.missingPages, selection.missingPages);
				}
				target.warnings.insert(target.warnings.end(), selection.warnings.begin(), selection.warnings.end());
			};
			for (TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& selectionSnapshot : virtualGeometryViewSnapshots)
			{
				auto existingSnapshot = std::find_if(
					detailedVirtualGeometryViewSnapshots.begin(),
					detailedVirtualGeometryViewSnapshots.end(),
					[viewType = selectionSnapshot.viewType](const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& snapshot) {
						return snapshot.viewType == viewType;
					});
				if (existingSnapshot != detailedVirtualGeometryViewSnapshots.end())
				{
					mergeSelectionDiagnostics(*existingSnapshot, selectionSnapshot);
				}
				else if (selectionSnapshot.viewType == RenderViewType::ExportRuntime
					|| (selectionSnapshot.viewType == RenderViewType::Shadow && !shadowDiagnosticsPublished))
				{
					detailedVirtualGeometryViewSnapshots.push_back(std::move(selectionSnapshot));
				}
			}
			virtualGeometryViewSnapshots = std::move(detailedVirtualGeometryViewSnapshots);
			for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& snapshot : virtualGeometryViewSnapshots)
			{
				for (const PerformanceGateResult& gate : snapshot.virtualGeometryPerformanceGates)
				{
					if (gate.status != VirtualGeometryPerformanceGateStatus::Fail)
					{
						continue;
					}
					renderGraphDiagnostics.backendFallbacks.push_back(
						"Virtual geometry gate failed for "
						+ gate.fixtureId
						+ " "
						+ ToString(gate.viewType)
						+ "#"
						+ std::to_string(gate.viewId)
						+ "/"
						+ gate.metricName
						+ ": expected "
						+ std::to_string(gate.threshold)
						+ ", actual "
						+ std::to_string(gate.actual));
				}
			}
			const TerraEngine::Scheduling::SystemDeclaration schedulerDeclaration = SchedulerDeclaration();
			std::vector<TerraEngine::Renderer::VirtualGeometrySchedulerDiagnostics> virtualGeometrySchedulerSnapshots;
			virtualGeometrySchedulerSnapshots.push_back(TerraEngine::Renderer::BuildVirtualGeometrySchedulerDiagnostics(
				schedulerDeclaration.name,
				schedulerDeclaration.phase,
				schedulerDeclaration.reads,
				schedulerDeclaration.writes,
				schedulerDeclaration.safeSubwork,
				schedulerDeclaration.protectedCommit,
				schedulerDeclaration.requiresFlushedSceneMutations
					? "SceneMutationQueue flush before render snapshot reads; virtual geometry diagnostics publish after protected RHI work."
					: "Virtual geometry diagnostics publish after protected RHI work."));
			TerraEngine::Renderer::ApplyVirtualGeometryDiagnostics(renderGraphDiagnostics, std::move(virtualGeometryViewSnapshots), std::move(virtualGeometrySchedulerSnapshots));
			TerraEngine::Renderer::PublishRenderDiagnostics(renderGraphDiagnostics);
		}
		else
		{
			// Keep latestRenderGraphDiagnostics useful for overlays and MCP stats,
			// but avoid publishing or enriching detailed diagnostics while closed.
			renderGraphDiagnostics.virtualGeometryViews = std::move(virtualGeometryViewSnapshots);
		}
		std::unique_lock<std::mutex> lock(statsMutex, std::defer_lock);
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Acquire Stats Publication Lock",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			lock.lock();
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Read Backend Render Stats",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			latestGameStats = rhi->GetLatestRenderStats(RenderViewType::Game);
			latestSceneStats = rhi->GetLatestRenderStats(RenderViewType::Scene);
		}
		const auto applyVirtualGeometryStats = [](RenderStatsSnapshot& stats, const TerraEngine::Renderer::RenderDiagnosticsSummary& diagnostics) {
			for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view : diagnostics.virtualGeometryViews)
			{
				if (view.viewType != stats.viewType)
				{
					continue;
				}
				stats.hiZState = stats.hiZActive
					? view.hiZState
					: (view.hiZState == VirtualGeometryHiZState::Active ? VirtualGeometryHiZState::MissingPyramid : view.hiZState);
				stats.hiZStatusReason = view.hiZInvalidReason;
				if (!stats.hiZActive && stats.hiZStatusReason.empty())
				{
					stats.hiZStatusReason = "backend pyramid unavailable for completed view stats";
				}
				stats.hiZWarmupFrames = view.hiZWarmupFrames;
				stats.hiZBaselineMatched = view.hiZBaselineMatched;
				stats.hiZFalseOcclusionRiskWarnings = view.hiZFalseOcclusionRiskWarnings;
				stats.virtualGeometryClusterPackets = view.clusterPacketCount;
				stats.virtualGeometrySubmittedBatches = view.submittedBatchCount;
				stats.virtualGeometrySinglePacketFallbackBatches = view.singlePacketFallbackBatchCount;
				stats.virtualGeometryCandidatePackets = view.candidateClusterCount;
				stats.virtualGeometryCompactedPackets = view.compactedClusterCount;
				stats.virtualGeometryRejectedPackets = view.rejectedClusterCount;
				stats.virtualGeometryFailOpenPackets = view.failOpenClusterCount;
				stats.virtualGeometryResidentPages = view.residentPages;
				stats.virtualGeometryRequestedPages = view.requestedPages;
				stats.virtualGeometryMissingPages = view.missingPages;
				stats.virtualGeometryFailedPages = view.failedPages;
				stats.virtualGeometryEvictedPages = view.evictedPages;
				stats.virtualGeometryPinnedFallbackPages = view.pinnedFallbackPages;
				// Copy the compact page-source summary into the stats snapshot so
				// UI, automation, and MCP readers can explain page-streamed geometry
				// without dereferencing the heavier render diagnostics graph.
				stats.virtualGeometryResidentPagePayloads = view.pageSources.residentPayloads;
				stats.virtualGeometryHighDetailPagePayloads = view.pageSources.highDetailPayloads;
				stats.virtualGeometryFallbackPagePayloads = view.pageSources.fallbackPayloads;
				stats.virtualGeometryEmbeddedFallbackPagePayloads = view.pageSources.embeddedFallbackPayloads;
				stats.virtualGeometryEmittedHighDetailPages = view.pageSources.emittedHighDetailSources;
				stats.virtualGeometryEmittedFallbackPages = view.pageSources.emittedFallbackSources;
				stats.virtualGeometryEmittedEmbeddedFallbackPages = view.pageSources.emittedEmbeddedFallbackSources;
				stats.virtualGeometryMissingPageSources = view.pageSources.missingSources;
				stats.virtualGeometryPageSourceRequests = view.pageSources.pageRequests;
				stats.virtualGeometryPageSourceRetirements = view.pageSources.pageRetirements;
				stats.virtualGeometryResidentPagePayloadBytes = view.pageSources.residentPayloadBytes;
				stats.virtualGeometryFallbackPagePayloadBytes = view.pageSources.fallbackPayloadBytes;
				stats.virtualGeometryEmbeddedFallbackBytes = view.pageSources.embeddedFallbackBytes;
				stats.virtualGeometryGpuPacketRecords = view.gpuPacketRecordCount;
				stats.virtualGeometryGpuBatchDescriptors = view.gpuBatchDescriptorCount;
				stats.virtualGeometryGpuPacketFrameSets = view.gpuPacketFrameSetCount;
				stats.virtualGeometryGpuValidFrameSets = view.gpuValidFrameSetCount;
				stats.virtualGeometryGpuInvalidFrameSets = view.gpuInvalidFrameSetCount;
				stats.virtualGeometryGpuCompactionDispatches = view.gpuCompactionDispatchCount;
				stats.virtualGeometryGeneratedSubmissions = view.generatedSubmissionCount;
				stats.virtualGeometryValidGeneratedSubmissions = view.validGeneratedSubmissionCount;
				stats.virtualGeometryInvalidGeneratedSubmissions = view.invalidGeneratedSubmissionCount;
				stats.virtualGeometryFallbackGeneratedSubmissions = view.fallbackGeneratedSubmissionCount;
				stats.virtualGeometryGpuCounterReadbacks = view.gpuCounterReadbackCount;
				stats.virtualGeometryGpuCounterLatencyFrames = view.gpuCounterReadbackLatencyFrames;
				stats.virtualGeometryGpuFallbackRecords = view.gpuFallbackRecordCount;
				stats.virtualGeometryGpuFallbackAffectedPackets = view.gpuFallbackAffectedPackets;
				stats.virtualGeometryGpuFallbackAffectedBatches = view.gpuFallbackAffectedBatches;
				stats.virtualGeometryGpuFallbackReason = view.gpuFallbackReason;
				stats.virtualGeometryPerformanceGateCount = static_cast<std::uint32_t>(view.virtualGeometryPerformanceGates.size());
				stats.virtualGeometryPerformanceGateFailures = 0u;
				stats.virtualGeometryPerformanceGateUnsupported = 0u;
				stats.virtualGeometryFirstFailedPerformanceGate.clear();
				for (const PerformanceGateResult& gate : view.virtualGeometryPerformanceGates)
				{
					if (gate.status == VirtualGeometryPerformanceGateStatus::Fail)
					{
						++stats.virtualGeometryPerformanceGateFailures;
						if (stats.virtualGeometryFirstFailedPerformanceGate.empty())
						{
							stats.virtualGeometryFirstFailedPerformanceGate =
								gate.fixtureId + "/" + gate.metricName;
						}
					}
					else if (gate.status == VirtualGeometryPerformanceGateStatus::Unsupported)
					{
						++stats.virtualGeometryPerformanceGateUnsupported;
					}
				}
				if (stats.meshletCounterReason.empty() && stats.meshletCounterSource == VirtualGeometryCounterSource::Prepared)
				{
					stats.meshletCounterSource = view.counterSource;
					stats.meshletCounterFrameId = view.countersAuthoritative
						? view.counterReadbackFrameId
						: view.counterPreparedFrameId;
					stats.meshletCounterLatencyFrames = view.counterReadbackLatencyFrames;
					stats.meshletCounterReason = view.counterUnavailableReason;
				}
				break;
			}
		};
		applyVirtualGeometryStats(latestGameStats, renderGraphDiagnostics);
		applyVirtualGeometryStats(latestSceneStats, renderGraphDiagnostics);
		// Full explanations exist only while diagnostics are requested. They are
		// not needed to count routes, but preserve the most useful fallback string
		// for the overlay when a diagnostic consumer explicitly asked for it.
		for(const TerraEngine::Renderer::GeometryRouteDecision& decision :
			renderFrameSnapshot.geometryRouteDecisions)
		{
			if(!decision.fallbackDiagnostic.empty())
			{
				geometryRouteStats.fallbackReason = decision.fallbackDiagnostic;
			}
		}
		latestGeometryRouteSelectionSignature = geometryRouteSelectionSignature;
		latestGeometryRouteSelectionCount =
			renderFrameSnapshot.renderables.size();
		const auto applyGeometryRouteStats = [&renderFrameSnapshot,
			&geometryRouteStats](RenderStatsSnapshot& stats) {
			stats.geometryRoutePolicyIdentity = geometryRouteStats.policyIdentity;
			stats.geometryRouteCalibrationIdentity = geometryRouteStats.calibrationIdentity;
			stats.geometryRouteHysteresisFrames = std::max(
				stats.geometryRouteHysteresisFrames,
				geometryRouteStats.hysteresisFrames);
			stats.geometryRouteFallbackReason = geometryRouteStats.fallbackReason;
			stats.geometryRouteSource = geometryRouteStats.source
				== TerraEngine::Renderer::GeometryRouteSelectionSource::AuthorOverride
				? "author_override" : geometryRouteStats.source
					== TerraEngine::Renderer::GeometryRouteSelectionSource::CapabilityFallback
				? "capability_fallback" : geometryRouteStats.source
					== TerraEngine::Renderer::GeometryRouteSelectionSource::CorrectnessFallback
				? "correctness_fallback" : "automatic";
			stats.geometryRouteDirectBatchCount = static_cast<std::uint32_t>(renderFrameSnapshot.directMeshletBatches.size());
			stats.geometryRouteSharedInstanceCount = geometryRouteStats.sharedInstances;
			stats.geometryRouteSuppressedPageRequests = geometryRouteStats.directCount;
			stats.geometryRouteSuppressedDecodeBlocks = geometryRouteStats.directCount;
			stats.geometryRouteSuppressedResidencyRecords = geometryRouteStats.directCount;
			// An empty route envelope means this frame contained no routable geometry;
			// do not label that absence as a successful direct-meshlet decision.
			stats.geometryRoute = renderFrameSnapshot.renderables.empty() ? ""
				: geometryRouteStats.virtualCount == 0U ? "direct_meshlet"
				: geometryRouteStats.directCount == 0U ? "virtual_geometry" : "mixed";
		};
		applyGeometryRouteStats(latestGameStats);
		applyGeometryRouteStats(latestSceneStats);
		latestGameStats.spatialIndexedObjectCount = gameSpatialDiagnostics.indexedObjectCount;
		latestGameStats.spatialVisibleCandidateCount = gameSpatialDiagnostics.visibleCandidateCount;
		latestGameStats.spatialRejectedObjectCount = gameSpatialDiagnostics.rejectedObjectCount;
		latestGameStats.spatialFallbackObjectCount = gameSpatialDiagnostics.fallbackObjectCount;
		latestGameStats.spatialRefreshMode = gameSpatialDiagnostics.refreshMode;
		latestGameStats.spatialAddedObjectCount = gameSpatialDiagnostics.addedObjectCount;
		latestGameStats.spatialUpdatedObjectCount = gameSpatialDiagnostics.updatedObjectCount;
		latestGameStats.spatialRemovedObjectCount = gameSpatialDiagnostics.removedObjectCount;
		latestGameStats.spatialUnchangedObjectCount = gameSpatialDiagnostics.unchangedObjectCount;
		latestGameStats.spatialFullRefreshObjectCount = gameSpatialDiagnostics.fullRefreshObjectCount;
		latestGameStats.transformDirtyRootCount = gameSpatialDiagnostics.transformDirtyRootCount;
		latestGameStats.transformProcessedObjectCount = gameSpatialDiagnostics.transformProcessedObjectCount;
		latestGameStats.boundsRecomputedObjectCount = gameSpatialDiagnostics.boundsRecomputedObjectCount;
		latestGameStats.visibleCommandCacheHit = gameVisibleCommandTelemetry.cacheHit;
		latestGameStats.visibleCommandConservativeRefresh = gameVisibleCommandTelemetry.conservativeRefresh;
		latestGameStats.detailedRenderDiagnosticsRequested = gameVisibleCommandTelemetry.detailedDiagnosticsRequested;
		latestGameStats.visibleCommandCount = gameVisibleCommandTelemetry.commandCount;
		latestGameStats.visibleCommandCopiedCount = gameVisibleCommandTelemetry.copiedCommandCount;
		latestGameStats.visibleStreamingUniqueMeshes = gameVisibleCommandTelemetry.uniqueMeshTouches;
		latestGameStats.visibleStreamingUniqueMaterials = gameVisibleCommandTelemetry.uniqueMaterialTouches;
		latestGameStats.visibleStreamingUniqueTextures = gameVisibleCommandTelemetry.uniqueTextureTouches;
		latestGameStats.visibleStreamingDuplicateTouchesEliminated = gameVisibleCommandTelemetry.duplicateResourceTouchesEliminated;
		latestGameStats.visibleCommandReuseFallbackReason = gameVisibleCommandTelemetry.fallbackReason;
		latestSceneStats.spatialIndexedObjectCount = sceneSpatialDiagnostics.indexedObjectCount;
		latestSceneStats.spatialVisibleCandidateCount = sceneSpatialDiagnostics.visibleCandidateCount;
		latestSceneStats.spatialRejectedObjectCount = sceneSpatialDiagnostics.rejectedObjectCount;
		latestSceneStats.spatialFallbackObjectCount = sceneSpatialDiagnostics.fallbackObjectCount;
		latestSceneStats.spatialRefreshMode = sceneSpatialDiagnostics.refreshMode;
		latestSceneStats.spatialAddedObjectCount = sceneSpatialDiagnostics.addedObjectCount;
		latestSceneStats.spatialUpdatedObjectCount = sceneSpatialDiagnostics.updatedObjectCount;
		latestSceneStats.spatialRemovedObjectCount = sceneSpatialDiagnostics.removedObjectCount;
		latestSceneStats.spatialUnchangedObjectCount = sceneSpatialDiagnostics.unchangedObjectCount;
		latestSceneStats.spatialFullRefreshObjectCount = sceneSpatialDiagnostics.fullRefreshObjectCount;
		latestSceneStats.transformDirtyRootCount = sceneSpatialDiagnostics.transformDirtyRootCount;
		latestSceneStats.transformProcessedObjectCount = sceneSpatialDiagnostics.transformProcessedObjectCount;
		latestSceneStats.boundsRecomputedObjectCount = sceneSpatialDiagnostics.boundsRecomputedObjectCount;
		latestSceneStats.visibleCommandCacheHit = sceneVisibleCommandTelemetry.cacheHit;
		latestSceneStats.visibleCommandConservativeRefresh = sceneVisibleCommandTelemetry.conservativeRefresh;
		latestSceneStats.detailedRenderDiagnosticsRequested = sceneVisibleCommandTelemetry.detailedDiagnosticsRequested;
		latestSceneStats.visibleCommandCount = sceneVisibleCommandTelemetry.commandCount;
		latestSceneStats.visibleCommandCopiedCount = sceneVisibleCommandTelemetry.copiedCommandCount;
		latestSceneStats.visibleStreamingUniqueMeshes = sceneVisibleCommandTelemetry.uniqueMeshTouches;
		latestSceneStats.visibleStreamingUniqueMaterials = sceneVisibleCommandTelemetry.uniqueMaterialTouches;
		latestSceneStats.visibleStreamingUniqueTextures = sceneVisibleCommandTelemetry.uniqueTextureTouches;
		latestSceneStats.visibleStreamingDuplicateTouchesEliminated = sceneVisibleCommandTelemetry.duplicateResourceTouchesEliminated;
		latestSceneStats.visibleCommandReuseFallbackReason = sceneVisibleCommandTelemetry.fallbackReason;
		latestGameSpatialDiagnostics = gameSpatialDiagnostics;
		latestSceneSpatialDiagnostics = sceneSpatialDiagnostics;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Read Backend Diagnostic Snapshots",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			latestGameBudgetDiagnostics = rhi->GetLatestFrameBudgetDiagnostics(RenderViewType::Game);
			latestSceneBudgetDiagnostics = rhi->GetLatestFrameBudgetDiagnostics(RenderViewType::Scene);
			latestGameLightingDiagnostics = rhi->GetLatestLightingDiagnostics(RenderViewType::Game);
			latestSceneLightingDiagnostics = rhi->GetLatestLightingDiagnostics(RenderViewType::Scene);
			latestRenderSnapshotDiagnostics = rhi->GetLatestRenderSnapshotDiagnostics();
			latestRendererRecoverySnapshot = rhi ? rhi->GetLatestRendererRecoverySnapshot() : recoverySnapshot;
			latestAnimationGpuBackendExecution = rhi
				? rhi->GetLatestAnimationGpuBackendExecution()
				: TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot{};
			latestRenderGraphDiagnostics = renderGraphDiagnostics;
		}
		// Capture identity without inventing backend measurements. Story-specific
		// receipts replace the explicit unavailable counters as stages are added.
		auto completedEfficiencyRows = rhi->PollRendererEfficiencyEvidence();
		std::erase_if(completedEfficiencyRows, [&](const auto& row) {
			return !TerraEngine::Renderer::IsCurrentRendererEfficiencyFixtureEvidence(row,
				renderFrameSnapshot.efficiencyOptions, renderFrameSnapshot.efficiencyFixtureIntent);
		});
		if(!completedEfficiencyRows.empty())
		{
			// One cursor owns the entire completed batch. View and face counts do
			// not advance fixture frame progression independently.
			++latestRendererEfficiencyEvidenceBatch.batchSequence;
			latestRendererEfficiencyEvidenceBatch.rows = std::move(completedEfficiencyRows);
			latestRendererEfficiencyEvidence = latestRendererEfficiencyEvidenceBatch.rows;
		}
		else if(latestRendererEfficiencyEvidence.empty())
		{
		for (const auto& view : renderFrameSnapshot.viewRequests)
		{
			TerraEngine::Renderer::RendererEfficiencyEvidence evidence;
			evidence.viewType = view.viewType;
			evidence.viewId = view.outputId;
			evidence.frameSerial = renderFrameSnapshot.frameId;
			evidence.resolution = view.targetSize;
			evidence.effectiveOptions = renderFrameSnapshot.efficiencyOptions;
			evidence.staticCacheBudgetRequestedBytes = evidence.effectiveOptions.options.staticShadowCacheBudgetBytes;
			evidence.staticCacheBudgetEffectiveBytes = evidence.staticCacheBudgetRequestedBytes;
			evidence.fallbackReason = "Optimized stage support has not been published by the backend.";
			latestRendererEfficiencyEvidence.push_back(std::move(evidence));
		}
		}
		// The renderer has finished every backend and statistics read of the local
		// snapshot. Transfer its owned vectors into the protected publication instead
		// of deep-copying thousands of renderables, route decisions, and batch members.
		// The feedback pass below only reads this completed publication.
		if (renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied && renderFrameSnapshot.efficiencyFixtureIntent.shadowSelection
			&& !fixtureShadowSelectionApplied.load(std::memory_order_relaxed))
		{
			renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied = false;
			renderFrameSnapshot.efficiencyFixtureIntent.controlReason = "Requested target did not appear in an emitted shadow owner-range command.";
		}
		latestRendererEfficiencyFixtureTargets = std::move(fixtureTargets);
		std::swap(latestRenderFrameSnapshot, renderFrameSnapshot);
		recycledRenderFrameSnapshot = std::move(renderFrameSnapshot);
		latestSceneVirtualGeometryVisibilityRecords =
			std::move(completedSceneVisibilityRecords);
		latestGameVirtualGeometryVisibilityRecords =
			std::move(completedGameVisibilityRecords);
		// An ordinary frame must not erase valid custom-capture proof before its
		// explicit identity query reaches the fence-owned Vulkan capture target.
		if(sceneCaptureProcessed)
			latestSceneCaptureVisibilityRecords = std::move(completedSceneCaptureVisibilityRecords);
		if(gameCaptureProcessed)
			latestGameCaptureVisibilityRecords = std::move(completedGameCaptureVisibilityRecords);
		if(sceneCaptureProcessed)
			latestSceneCaptureIdentitySnapshot = std::move(completedSceneCaptureIdentitySnapshot);
		if(gameCaptureProcessed)
			latestGameCaptureIdentitySnapshot = std::move(completedGameCaptureIdentitySnapshot);
		latestSkyRuntimeState = skyRuntimeState;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Record Profiler Render Evidence",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			TerraEngine::Profiling::Profiler::Instance().RecordRenderStats(
				latestGameStats, latestSceneStats);
		}
	}

	skyStateDirty = false;

	// Submit copied observations after this frame's immutable snapshot is built.
	// StreamingPrepare runs before the next render update, so these values are
	// naturally prior-frame feedback and cannot make a page resident mid-frame.
	TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Streaming Feedback and Cleanup",
		TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	std::size_t streamingFeedbackCount = 0U;
	for(const auto& view : latestRenderFrameSnapshot.virtualGeometryViewSelections) {
		streamingFeedbackCount += view.PageRequests().size();
	}
	std::vector<TerraEngine::Resource::VirtualGeometryStreamingFeedback> streamingFeedback;
	streamingFeedback.reserve(streamingFeedbackCount);
	std::set<std::pair<TerraEngine::UUID, std::uint32_t>> pagesWithCanonicalMetadata;
	for(const auto& view : latestRenderFrameSnapshot.virtualGeometryViewSelections) {
		for(const auto& request : view.PageRequests()) {
			TerraEngine::Resource::VirtualGeometryStreamingFeedback feedback{};
			feedback.assetId = request.meshId; feedback.pageId = request.pageId;
			feedback.fallbackPageId = request.fallbackPageId; feedback.clusterId = request.clusterId;
			feedback.priority = request.priority; feedback.observedFrame = activeRenderFrameId;
			// Build immutable source metadata only for the first occurrence of an exact
			// mesh/page key. StreamingPrepare already coalesces duplicate view/instance
			// feedback while retaining maximum priority and every fallback dependency;
			// repeating the page-table scan for a shared crowd would add no information.
			const bool resolveCanonicalPage = pagesWithCanonicalMetadata.emplace(
				request.meshId, request.pageId).second;
			if(resolveCanonicalPage) {
				feedback.canonicalPageReadDiagnostic =
					"No exact compact page range was resolved from the current mesh snapshot.";
			}
			// Resolve every canonical TEMFPRD1 block for this unique page while
			// RenderSystem owns the main-thread ResourceManager snapshot. Worker
			// preparation receives copied values only and never inspects live mesh state.
			const Mesh* mesh = resolveCanonicalPage
				? resourceManager.GetMesh(request.meshId) : nullptr;
			if(mesh != nullptr)
			{
				if(mesh->sourcePath.empty())
				{
					feedback.canonicalPageReadDiagnostic =
						"The validated TEMFPRD1 directory has no immutable cooked source path.";
				}
				else if(mesh->storage.size() < sizeof(TemfProductionHeader))
				{
					feedback.canonicalPageReadDiagnostic =
						"The loaded mesh does not retain a canonical TEMFPRD1 production header.";
				}
				else
				{
					TemfProductionHeader header{};
					std::memcpy(&header, mesh->storage.data(), sizeof(header));
					const auto rangeFitsStorage = [&storage = mesh->storage](std::uint64_t offset,
						std::uint64_t count, std::size_t stride) {
						return offset <= storage.size()
							&& count <= (storage.size() - static_cast<std::size_t>(offset)) / stride;
					};
					if(header.formatContractId != kTemfProductionContractId
						|| header.headerBytes != kTemfProductionHeaderBytes)
					{
						feedback.canonicalPageReadDiagnostic = "The mesh header is not canonical TEMFPRD1 data; recook-required.";
					}
					else if(!rangeFitsStorage(header.pageTableOffset, header.pageCount, sizeof(TemfPageDescriptor))
						|| !rangeFitsStorage(header.blockTableOffset, header.blockCount, sizeof(TemfBlockDescriptor))
						|| !rangeFitsStorage(header.quantizationTableOffset, header.quantizationDomainCount, sizeof(TemfQuantizationDomain)))
					{
						feedback.canonicalPageReadDiagnostic = "The canonical TEMFPRD1 directory range is unavailable in retained mesh storage.";
					}
					else
					{
						TemfPageDescriptor page{};
						bool foundPage = false;
						for(std::uint32_t index = 0u; index < header.pageCount; ++index)
						{
							std::memcpy(&page, mesh->storage.data() + header.pageTableOffset
								+ static_cast<std::uint64_t>(index) * sizeof(page), sizeof(page));
							if(page.logicalPageId == request.pageId) { foundPage = true; break; }
						}
						if(!foundPage)
						{
							feedback.canonicalPageReadDiagnostic = "The requested page ID is absent from the canonical TEMFPRD1 page table.";
						}
						else if(page.blockCount == 0u || page.firstBlockIndex > header.blockCount
							|| page.blockCount > header.blockCount - page.firstBlockIndex
							|| page.attributedStoredBytes == 0u || page.decodedBytes == 0u || page.pageIntegrity == 0u)
						{
							feedback.canonicalPageReadDiagnostic = "The requested TEMFPRD1 page has invalid block bounds, totals, or aggregate integrity.";
						}
						else
						{
							std::uint64_t storedTotal = 0u;
							std::uint64_t decodedTotal = 0u;
							std::uint64_t expectedNextPayloadOffset = 0u;
							bool pageRangesValid = true;
							feedback.exactBlockReads.reserve(page.blockCount);
							for(std::uint32_t blockOffset = 0u; blockOffset < page.blockCount; ++blockOffset)
							{
								TemfBlockDescriptor block{};
								const std::uint64_t blockIndex = static_cast<std::uint64_t>(page.firstBlockIndex) + blockOffset;
								std::memcpy(&block, mesh->storage.data() + header.blockTableOffset
									+ blockIndex * sizeof(block), sizeof(block));
								const bool totalsOverflow = block.encodedBytes > std::numeric_limits<std::uint64_t>::max() - storedTotal
									|| block.decodedBytes > std::numeric_limits<std::uint64_t>::max() - decodedTotal;
								const bool payloadOffsetOverflow = block.encodedBytes > std::numeric_limits<std::uint64_t>::max()
									- block.encodedPayloadOffset;
								const bool contiguousPayload = blockOffset == 0u
									|| block.encodedPayloadOffset == expectedNextPayloadOffset;
								if(block.logicalPageId != page.logicalPageId || block.blockOrdinal != blockOffset
									|| block.encodedBytes == 0u
									|| block.decodedBytes == 0u || totalsOverflow || payloadOffsetOverflow
									|| !contiguousPayload
									|| !rangeFitsStorage(block.encodedPayloadOffset, block.encodedBytes, 1u))
								{
									pageRangesValid = false;
									break;
								}
								storedTotal += block.encodedBytes;
								decodedTotal += block.decodedBytes;
								expectedNextPayloadOffset = block.encodedPayloadOffset + block.encodedBytes;
								TerraEngine::Resource::VirtualGeometryStaticPageReadRequest pageRead{};
								pageRead.path = mesh->sourcePath;
								pageRead.pageId = request.pageId;
								pageRead.byteOffset = block.encodedPayloadOffset;
								pageRead.byteSize = block.encodedBytes;
								pageRead.expectedHash = block.encodedIntegrity;
								pageRead.mountedLogicalPath = mesh->sourceLogicalPath;
								pageRead.mountedContentRequired =
									resourceManager.IsExportRuntime();
								feedback.exactBlockReads.push_back({ std::move(pageRead), block });
							}
							if(!pageRangesValid || storedTotal != page.attributedStoredBytes
								|| decodedTotal != page.decodedBytes)
							{
								feedback.exactBlockReads.clear();
								feedback.canonicalPageReadDiagnostic = "The TEMFPRD1 page block ranges, ordering, or byte totals are inconsistent.";
							}
							else
							{
								feedback.expectedStoredByteCount = page.attributedStoredBytes;
								feedback.expectedDecodedByteCount = page.decodedBytes;
								feedback.expectedIntegrityValue = page.pageIntegrity;
								feedback.quantizationDomains.resize(header.quantizationDomainCount);
								if(!feedback.quantizationDomains.empty())
								{
									std::memcpy(feedback.quantizationDomains.data(), mesh->storage.data()
										+ header.quantizationTableOffset,
										feedback.quantizationDomains.size() * sizeof(TemfQuantizationDomain));
								}
								// This is a CPU decoder capability copied to the worker. It is not
								// GPU execution evidence and permits the bundled Meshoptimizer path.
								feedback.availableDecoderCapabilities = kVirtualGeometryCodecCapabilityMeshoptimizer;
								feedback.canonicalPageReadDiagnostic = "Resolved ordered exact TEMFPRD1 block ranges and aggregate page integrity.";
							}
						}
					}
				}
			}
			else if(resolveCanonicalPage)
			{
				feedback.canonicalPageReadDiagnostic =
					"The requested mesh ID is not present in ResourceManager.";
			}
			if(request.fallbackPageId != request.pageId && request.fallbackPageId != 0xffffffffu)
				feedback.dependencyPageIds.push_back(request.fallbackPageId);
			streamingFeedback.push_back(std::move(feedback));
		}
	}
	TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance()
		.SubmitPriorFrameFeedback(std::move(streamingFeedback));

	ResourceManager::Instance().RunResidencyCleanup(activeRenderFrameId);
	ProcessQueuedCaptureRequests();
	DrainCaptureReadbacks();

	ClearOverrideCameras();
	scratchLease.MarkCompleted();
}

// RenderSystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Shutdown()
{
	RenderExecutionBoundary boundary(frameExecutionBoundary);
	RetireFrameExecutionAtProtectedBoundary();
	initialized = false;
	if(lifetimeState->frameScratch) lifetimeState->frameScratch->Retire();
	ObserveFrameScratchAtProtectedBoundary();
	{
		std::lock_guard lock(statsMutex);
		// The current lifetime still owns scratch. Drop the observer now so later
		// lifetime replacement destroys backing outside this lock, protected.
		observedFrameScratchOwner.reset();
	}
	// Restore tokens and pending requests belong to this renderer lifetime, not
	// just Vulkan's counter (which may restart with a replacement instance).
	{
		std::lock_guard lock(runtimeRenderSettingsMutex);
		rendererEfficiencyOptionsQueue.ResetForNewLifetime();
		pendingRendererEfficiencyFixtureIntent.reset();
		capturedRendererEfficiencyFixtureIntent = {};
		pendingRendererResourceInvalidations.clear();
		efficiencyObservedBackendSession = 0U;
	}
	{
		std::lock_guard lock(statsMutex);
		latestRendererEfficiencyEvidence.clear();
		latestRendererEfficiencyFixtureTargets.clear();
		TerraEngine::Renderer::ResetRendererEfficiencyFixtureEvidenceBatch(latestRendererEfficiencyEvidenceBatch);
	}
	// Drop copied scene data before backend teardown so a later editor restart
	// cannot reuse transforms or sky entities from the previous scene session.
	worldTransformCache.Clear();
	compatibleAnimationPoseCache.clear();
	renderableMembershipCache.clear();
	retainedRenderableEntries.clear();
	retainedRuntimeMeshResources.clear();
	retainedRuntimeMaterialResources.clear();
	retainedRuntimeMeshOwners.clear();
	retainedRuntimeMaterialOwners.clear();
	retainedMeshLocalBounds.clear();
	retainedRenderableCaptureSourceSignature = 0U;
	retainedRenderableEntriesReusable = false;
	renderableMembershipSceneGeneration = 0U;
	renderableMembershipComponentRevision = 0U;
	renderableMembershipHierarchyRevision = 0U;
	renderableMembershipActiveRevision = 0U;
	renderableMembershipPacketRevision = 0U;
	renderableMembershipTerrainProxyGeneration = 0U;
	TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance().Clear();
	renderableWorldPoseCache.clear();
	renderableModelMatrixSceneGeneration = 0U;
	renderableModelMatrixSpatialRevision = 0U;
	renderableModelMatrixComponentRevision = 0U;
	renderableModelMatrixHierarchyRevision = 0U;
	virtualGeometryViewPolicies.clear();
	virtualGeometryReplayReadbacks.Clear();
	latestVirtualGeometryReplayArtifacts.clear();
	// Detach the immutable hand-off before destroying backend resources so no
	// retained reader can mistake the shutdown generation for a live session.
	renderFramePublicationStore.ResetForSession(
		renderFramePublicationStore.SessionGeneration() + 1u);
	retainedPersistentTopologyRoot.reset();
	retainedDynamicStateRoot.reset();
	retainedCpuDynamicPageIndices.clear();
	dynamicPageRecyclePool.Clear();
	retainedDynamicStateSourceSignature = 0u;
	// Release every retained topology/view/backend cache with the renderer
	// session. A later Initialize starts from an empty, instance-owned state.
	lifetimeState = std::make_unique<RenderSystemLifetimeState>();
	hasCachedSkyObjectResolution = false;
	TerraEngine::Renderer::ClearAnimatedObjectVisibilitySnapshot();
	cachedSkyObject.reset();
	cachedSkyHierarchyRevision = 0U;
	cachedSkyCandidateSignature = 0U;
	if (rhi) {
		rhi->Shutdown();
		rhi.reset();
	}
}

// RenderSystem::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string RenderSystem::SystemName()
{
	return "RenderSystem";
}

TerraEngine::Scheduling::SystemDeclaration RenderSystem::SchedulerDeclaration()
{
	TerraEngine::Scheduling::SystemDeclaration declaration;
	declaration.name = "RenderSystem";
	declaration.phase = "Render";
	declaration.reads = {
		"RendererEfficiencyOptionsQueue", "RendererEfficiencyOptions",
		"LightingUploadMirrors", "LightingFrameSlotCompletion",
		"LightingDepthPreparedInputs", "LightingDepthQueryOwners",
		"Window",
		"ActivePresentationState",
		"SceneGraph",
		"ObjectManager",
		"UI",
		"Resources",
		TerraEngine::Scheduling::kResourceTerrainRenderProxies,
		TerraEngine::Scheduling::kResourceAnimatorComponents,
		TerraEngine::Scheduling::kResourceAnimationRuntimeState,
		TerraEngine::Scheduling::kResourceVirtualGeometryQualityProfile,
		TerraEngine::Scheduling::kResourceVirtualGeometryResidencySnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageResidencySnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryHiZValidationState,
		TerraEngine::Scheduling::kResourceVirtualGeometryGpuPacketFrameSets,
		TerraEngine::Scheduling::kResourceVirtualGeometryGpuCompactionDispatches,
		TerraEngine::Scheduling::kResourceVirtualGeometryGeneratedSubmissions,
		TerraEngine::Scheduling::kResourceVirtualGeometryCounterReadbacks,
		TerraEngine::Scheduling::kResourceVirtualGeometryPagePayloads,
		TerraEngine::Scheduling::kResourcePhysicalVirtualGeometryPagePayloads,
		TerraEngine::Scheduling::kResourceExactResidencySnapshots,
		TerraEngine::Scheduling::kResourceBackendGpuAllocationSnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryPagePublications,
		TerraEngine::Scheduling::kResourceVisibleStreamingTouchSets,
		TerraEngine::Scheduling::kResourceRendererRecoveryStateSnapshot,
		TerraEngine::Scheduling::kResourceRendererDeviceIncidentLog,
		TerraEngine::Scheduling::kResourceRuntimeRenderSettingsQueue,
		TerraEngine::Scheduling::kResourceAnimatedGeometryPageDemands,
		TerraEngine::Scheduling::kResourceAnimatedGeometryOutputs,
		TerraEngine::Scheduling::kResourceAnimatedGeometryDynamicBounds,
		TerraEngine::Scheduling::kResourceAnimatedGeometryPublications
	};
	declaration.writes = {
		"RendererEfficiencyOptions", "RendererEfficiencyEvidence",
		// Protected capture may admit a deduplicated async mesh/material request;
		// accepting a ready resource also changes its publication cohort.
		"Resources", "StreamingNeeds",
		"Renderer",
		TerraEngine::Scheduling::kResourcePersistentRenderTopology,
		TerraEngine::Scheduling::kResourceDynamicRenderInstanceCandidates,
		TerraEngine::Scheduling::kResourceRenderCompatibilityGroups,
		TerraEngine::Scheduling::kResourceRenderInstanceStreamPublication,
		TerraEngine::Scheduling::kResourceCachedRenderViewEvidence,
		TerraEngine::Scheduling::kResourceRenderOptimizationEvidence,
		"PresentationState",
		"PresentationGeneration",
		"EditorSubmissionCompletion",
		"RendererDiagnostics",
		TerraEngine::Scheduling::kResourceVirtualGeometryClusterPackets,
		TerraEngine::Scheduling::kResourceVirtualGeometryBatchPlans,
		TerraEngine::Scheduling::kResourceVirtualGeometryGpuPacketFrameSets,
		TerraEngine::Scheduling::kResourceVirtualGeometryGpuCompactionDispatches,
		TerraEngine::Scheduling::kResourceVirtualGeometryCompactionResults,
		TerraEngine::Scheduling::kResourceVirtualGeometryGeneratedSubmissions,
		TerraEngine::Scheduling::kResourceVirtualGeometryPagePayloads,
		TerraEngine::Scheduling::kResourcePhysicalVirtualGeometryPagePayloads,
		TerraEngine::Scheduling::kResourceExactResidencySnapshots,
		TerraEngine::Scheduling::kResourceBackendGpuAllocationSnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageRequests,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageNeedPackets,
		TerraEngine::Scheduling::kResourceVirtualGeometryPagePublications,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageRetirements,
		TerraEngine::Scheduling::kResourceVirtualGeometryPageDiagnostics,
		TerraEngine::Scheduling::kResourceVirtualGeometryVisibilitySnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryCounterSnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryCounterReadbacks,
		TerraEngine::Scheduling::kResourceVirtualGeometryCullingDiagnostics,
		TerraEngine::Scheduling::kResourceVirtualGeometryPerformanceCounters,
		TerraEngine::Scheduling::kResourceVirtualGeometryShadowVisibilitySnapshots,
		TerraEngine::Scheduling::kResourceVirtualGeometryDiagnostics,
		TerraEngine::Scheduling::kResourceVisibleCommandReuseDiagnostics,
		TerraEngine::Scheduling::kResourceVisibleStreamingTouchSets,
		TerraEngine::Scheduling::kResourceRendererRecoveryStateSnapshot,
		TerraEngine::Scheduling::kResourceRendererDeviceIncidentLog,
		TerraEngine::Scheduling::kResourceRuntimeRenderSettings,
		TerraEngine::Scheduling::kResourceAnimatedGeometryPageDemands,
		TerraEngine::Scheduling::kResourceAnimatedGeometryDeformationPlans,
		TerraEngine::Scheduling::kResourceAnimatedGeometryOutputs,
		TerraEngine::Scheduling::kResourceAnimatedGeometryDynamicBounds,
		TerraEngine::Scheduling::kResourceAnimatedGeometryPublications,
		TerraEngine::Scheduling::kResourceAnimatedGeometryRetirements,
		TerraEngine::Scheduling::kResourceAnimatedObjectVisibilitySnapshot
	};
	// Live ECS collection still runs protected inside RenderSystem, while draw,
	// light, upload, adaptive virtual geometry LOD/cluster/page intent, and
	// backend packet preparation consume immutable snapshots. The worker-safe
	// side may calculate selections and diagnostics only; the protected side
	// still owns RHI submission, resource publication, and the final diagnostic
	// swap visible to editor windows and automation.
	declaration.mainThread = false;
	declaration.requiresFlushedSceneMutations = true;
	declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;
	declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe;
	declaration.hardBoundary = true;
	declaration.safeSubwork = "Immutable render snapshots, exact shadowcaster membership preparation from copied input proofs, generation-validated animation pose bindings, frame-wide animated page demand union, checked morph-before-skin deformation prefix planning, history compatibility lookup, visible-command cache lookup, draw packets, culling and lighting preparation, generation-tagged submission plans, virtual geometry LOD/page preparation, page request planning, page payload source planning, resource readiness, secondary command buffers, and submit descriptions.";
	declaration.protectedCommit = "Runtime render-settings queue application, protected deduplicated async admission of missing mesh/material publications, live hierarchy/AnimatorComponent/deformable-resource and immutable shadowcaster lifetime/resource/deformation proof capture, immutable AnimationRuntimeState pose capture, Swapchain acquire, acquired-image generation validation, animated geometry allocation and atomic geometry/history/bounds publication with one cross-view generation, descriptor writes, deformation command recording, fence-delayed animated output retirement, command/fence ownership, queue submit/present, presentation recreation, completion publication, ResourceManager visible streaming touch publication, virtual geometry page payload publication, page retirement draining, virtual geometry GPU publication, virtual geometry diagnostics publication, renderer diagnostics, and device-loss containment.";
	declaration.safeSubwork += " Canonical lighting payload serialization from immutable copied final/analysis records and exact physical dirty-range planning.";
	declaration.protectedCommit += " Lighting allocation/frame/snapshot/purpose owner validation, fence-proven host map/write/atom-flush, success-only mirror publication and completion-qualified lighting evidence.";
	declaration.writes.push_back("LightingUploadMirrors");
	// BeginFrame and submission failure paths also update the fence-backed
	// physical reuse proof inside this same protected renderer boundary.
	declaration.writes.push_back("LightingFrameSlotCompletion");
	declaration.writes.push_back("LightingUploadEvidence");
	declaration.writes.push_back("LightingDepthPreparedInputs");
	declaration.writes.push_back("LightingDepthQueryOwners");
	declaration.writes.push_back("LightingDepthEvidence");
	declaration.reads.push_back("ShadowGroupedPreparedInputs");
	declaration.reads.push_back("ShadowGroupedPhysicalOwners");
	declaration.writes.push_back("ShadowGroupedPreparedInputs");
	declaration.writes.push_back("ShadowGroupedPhysicalOwners");
	declaration.writes.push_back("ShadowGroupedExecutionEvidence");
	declaration.reads.push_back("StaticShadowClassificationInputs");
	declaration.reads.push_back("StaticShadowPhysicalOwners");
	declaration.writes.push_back("StaticShadowClassificationInputs");
	declaration.writes.push_back("StaticShadowPhysicalOwners");
	declaration.writes.push_back("StaticShadowExecutionEvidence");
	declaration.safeSubwork += " Partitions copied complete light-selected caster proofs and plans deterministic queried byte admission in settled jobs.";
	declaration.protectedCommit += " Owns static allocation, CLEAR/copy/LOAD recording and separate static/final submit tokens; exact command completion retains retiring charges without adding an idle or scene flush.";
	declaration.safeSubwork += " Seals full light-selected instance streams from immutable roots and plans copied exact native compatibility in settled jobs.";
	declaration.protectedCommit += " Publishes separate frame/snapshot shadow instance ranges only after complete upload/descriptor proof and records actual grouped/singleton receipts; rejection restores the complete original draws.";
	declaration.safeSubwork += " Materializes exact-bit isolated depth recipes into immutable worker-owned CPU inputs.";
	declaration.protectedCommit += " Records physical depth query pairs and optional validation transfers; successful submit and existing fence completion qualify copied bounds, coverage, input and same-command color without query waits.";
	declaration.protectedReason = "Live GPU queue and presentation ownership remain protected after worker preparation.";
	declaration.reads.push_back(TerraEngine::Scheduling::kResourceRendererFrameScratch);
	declaration.writes.push_back(TerraEngine::Scheduling::kResourceRendererFrameScratch);
	declaration.safeSubwork += " Reads completed renderer-owned camera/light collections under one retained scratch episode; workers never allocate or reset that resource.";
	declaration.protectedCommit += " Retires prior continuation borrowers before opening renderer scratch, then publishes copied terminal episode/reclamation facts at existing protected boundaries.";
	declaration.profilingLabel = declaration.name;
	declaration.usesDefaultMetadata = false;
	return declaration;
}

std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> RenderSystem::DeclareFrameWorkloads(float deltaTime)
{
	using namespace TerraEngine::Scheduling;
	std::array<FrameWorkloadDeclaration, 5> workloads;
	std::size_t priorRouteDecisionCount = 0u;
	{
		// The last immutable snapshot is sufficient for dispatch granularity. A
		// first frame reports zero and therefore safely takes the inline path.
		std::lock_guard<std::mutex> lock(statsMutex);
		priorRouteDecisionCount = latestRenderFrameSnapshot.renderables.size();
	}

	// Protected capture publishes only immutable inputs. The route worker turns
	// those inputs into persistent topology and dynamic-state candidates; later
	// workers never reopen live ObjectManager or renderer-owned backend state.
	FrameWorkloadDeclaration& snapshot = workloads[0];
	snapshot.workloadName = "RenderPublicationCapture";
	snapshot.ownerSystem = "RenderSystem";
	snapshot.phase = "Render";
	snapshot.stableDescriptorId = "RenderSystem/Render/RenderPublicationCapture";
	snapshot.activationPredicateKey = "render-system-frame";
	snapshot.reads = {
		"Window", "SceneGraph", "Resources",
		kResourceRuntimeRenderSettingsQueue, "RendererEfficiencyOptionsQueue",
		kResourceSceneChangePublication,
		kResourceAnimationPresentationFrame,
		kResourceVirtualGeometryPagePublications,
		kResourceVirtualGeometryPageResidencySnapshots,
		kResourceVirtualGeometryCounterReadbacks,
		kResourceAnimatedGeometryPublications
	};
	snapshot.writes = {
		kResourceRuntimeRenderSettings, "RendererEfficiencyOptions",
		kResourceRendererFrameScratch,
		// Request admission stays at this protected boundary; workers consume
		// only the captured owners and never request or wait for a live load.
		"Resources", "StreamingNeeds",
		kResourceRenderCaptureInputs,
		kResourceAnimatedObjectVisibilitySnapshot
	};
	snapshot.explicitDependencies = { "VirtualGeometryStreamingSystem/Render/PagePublish" };
	snapshot.workerEligible = false;
	snapshot.mainThreadRequired = true;
	snapshot.mutationPolicy = FrameWorkloadMutationPolicy::DeferredOnly;
	snapshot.phaseKind = WorkerSystemPhaseKind::Snapshot;
	snapshot.boundaryClassification = HardBoundaryClassification::Protected;
	snapshot.hardBoundary = true;
	snapshot.protectedCommit = "Admits deduplicated async requests for missing mesh/material publications, then captures the already-published scene change packet, exact transforms, resources, immutable shadowcaster lifetime/resource/deformation inputs, animation presentation, page publication, readback, requested views, backend capabilities, and renderer generations without lowering the crowd.";
	snapshot.protectedCommit += " Retires the previous continuation before allocating/resetting the exclusive renderer-owned camera/light scratch episode.";
	snapshot.protectedReason = snapshot.protectedCommit;
	snapshot.profilingLabel = "RenderSystem.RenderPublicationCapture";

	FrameWorkloadDeclaration& routePrepare = workloads[1];
	routePrepare.workloadName = "GeometryRoutePrepare";
	routePrepare.ownerSystem = "RenderSystem";
	routePrepare.phase = "Render";
	routePrepare.stableDescriptorId = "RenderSystem/Render/GeometryRoutePrepare";
	routePrepare.activationPredicateKey = snapshot.activationPredicateKey;
	routePrepare.reads = {
		"RendererEfficiencyOptions",
		kResourceRendererFrameScratch,
		kResourceRenderCaptureInputs,
		kResourceVirtualGeometryQualityProfile
	};
	routePrepare.writes = { "GeometryRouteDecisions", "DirectMeshletBatches",
		kResourcePersistentRenderSceneState, kResourceDynamicRenderFrameState,
		kResourcePersistentRenderTopology, kResourceRenderCompatibilityGroups };
	routePrepare.explicitDependencies = { snapshot.stableDescriptorId };
	routePrepare.workerEligible = true;
	routePrepare.mainThreadRequired = false;
	routePrepare.estimatedWorkItemCount = priorRouteDecisionCount;
	routePrepare.inlineWorkItemThreshold = 16u;
	routePrepare.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
	routePrepare.boundaryClassification = HardBoundaryClassification::WorkerSafe;
	routePrepare.safeSubwork = "Assembles immutable render records, scores representation candidates, applies calibrated hysteresis, commits transactional topology pages, and groups direct instances without touching live resources.";
	routePrepare.profilingLabel = "RenderSystem.GeometryRoutePrepare";
	routePrepare.safeSubwork += " Borrows completed camera/light scratch collections read-only; no allocation/reset occurs on workers.";

	FrameWorkloadDeclaration& prepare = workloads[2];
	prepare.workloadName = "VirtualGeometryPrepare";
	prepare.ownerSystem = "RenderSystem";
	prepare.phase = "Render";
	prepare.stableDescriptorId = "RenderSystem/Render/VirtualGeometryPrepare";
	prepare.activationPredicateKey = snapshot.activationPredicateKey;
	prepare.reads = {
		"RendererEfficiencyOptions",
		kResourceRendererFrameScratch,
		kResourcePersistentRenderSceneState, kResourceDynamicRenderFrameState,
		kResourceVirtualGeometryQualityProfile,
		kResourceVirtualGeometryPageResidencySnapshots,
		kResourceVirtualGeometryPagePublications,
		kResourceAnimatedGeometryOutputs, "GeometryRouteDecisions", "DirectMeshletBatches"
	};
	prepare.writes = {
		kResourceRenderViewSelections, kResourceDynamicRenderInstanceCandidates,
		kResourceCachedRenderViewEvidence,
		"RenderGroupingEvidence",
		kResourceVirtualGeometryClusterPackets, kResourceVirtualGeometryBatchPlans,
		kResourceVirtualGeometryPageNeedPackets, kResourceAnimatedGeometryDeformationPlans
	};
	prepare.explicitDependencies = { routePrepare.stableDescriptorId };
	prepare.workerEligible = true;
	prepare.mainThreadRequired = false;
	prepare.estimatedWorkItemCount = priorRouteDecisionCount;
	prepare.inlineWorkItemThreshold = 16u;
	prepare.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
	prepare.boundaryClassification = HardBoundaryClassification::WorkerSafe;
	prepare.safeSubwork = "Consumes only immutable persistent scene state, dynamic lanes, compact view selections, and prior immutable publications to produce typed virtual geometry selection, packet, page-need, and deformation plans.";
	prepare.writes.push_back("LightingDepthPreparedInputs");
	prepare.writes.push_back("ShadowGroupedPreparedInputs");
	prepare.writes.push_back("StaticShadowClassificationInputs");
	prepare.safeSubwork += " Seals each light's immutable instance rows without using camera-visible membership.";
	prepare.safeSubwork += " Per-view preparation materializes admitted exact-bit depth recipes into immutable owned bytes.";
	prepare.profilingLabel = "RenderSystem.VirtualGeometryPrepare";
	prepare.safeSubwork += " Reads the retained camera/light scratch episode, with all child borrowers settled before continuation retirement.";

	FrameWorkloadDeclaration& commit = workloads[3];
	commit.workloadName = "BackendCommit";
	commit.ownerSystem = "RenderSystem";
	commit.phase = "Render";
	commit.stableDescriptorId = "RenderSystem/Render/BackendCommit";
	commit.activationPredicateKey = snapshot.activationPredicateKey;
	commit.reads = {
		"RendererEfficiencyOptions",
		kResourceRendererFrameScratch,
		"LightingDepthPreparedInputs", "LightingDepthQueryOwners",
		kResourcePersistentRenderSceneState, kResourceDynamicRenderFrameState,
		kResourceRenderViewSelections, kResourceVirtualGeometryClusterPackets,
		kResourceVirtualGeometryBatchPlans,
		kResourceVirtualGeometryPageNeedPackets, kResourceAnimatedGeometryDeformationPlans
	};
	commit.writes = {
		"LightingDepthQueryOwners", "LightingDepthEvidence",
		kResourceRenderFramePublication, kResourceRenderInstanceStreamPublication,
		kResourceVirtualGeometryGpuPacketFrameSets, kResourceVirtualGeometryGeneratedSubmissions,
		kResourceVirtualGeometryCounterReadbacks, kResourceVirtualGeometryVisibilitySnapshots
	};
	commit.reads.push_back("ShadowGroupedPreparedInputs");
	commit.reads.push_back("ShadowGroupedPhysicalOwners");
	commit.writes.push_back("ShadowGroupedPhysicalOwners");
	commit.writes.push_back("ShadowGroupedExecutionEvidence");
	commit.reads.push_back("StaticShadowClassificationInputs");
	commit.reads.push_back("StaticShadowPhysicalOwners");
	commit.writes.push_back("StaticShadowPhysicalOwners");
	commit.writes.push_back("StaticShadowExecutionEvidence");
	commit.explicitDependencies = { prepare.stableDescriptorId };
	commit.workerEligible = false;
	commit.mainThreadRequired = true;
	commit.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
	commit.boundaryClassification = HardBoundaryClassification::Protected;
	commit.hardBoundary = true;
	commit.protectedCommit = "Validates and seals one render-frame publication, then owns backend resource mutation, typed-range recording, queue submission, pending counters, and visibility targets.";
	commit.protectedCommit += " Depth query recording, actual submit/completion ownership and optional completed validation copies stay here; workers do not issue Vulkan commands or mutate the scene.";
	commit.protectedCommit += " Consumes captured camera/light scratch collections without reclaiming their resource while borrowed.";
	commit.protectedReason = commit.protectedCommit;
	commit.profilingLabel = "RenderSystem.BackendCommit";

	FrameWorkloadDeclaration& publication = workloads[4];
	publication.workloadName = "RenderPublication";
	publication.ownerSystem = "RenderSystem";
	publication.phase = "Render";
	publication.stableDescriptorId = "RenderSystem/Render/RenderPublication";
	publication.activationPredicateKey = snapshot.activationPredicateKey;
	publication.reads = {
		kResourceRendererFrameScratch,
		kResourceRenderFramePublication,
		kResourceVirtualGeometryGeneratedSubmissions,
		kResourceVirtualGeometryVisibilitySnapshots
	};
	publication.writes = {
		"RendererEfficiencyEvidence",
		"RendererDiagnostics", kResourceVirtualGeometryDiagnostics,
		kResourceProfilerFramePublicationEvidence
	};
	publication.explicitDependencies = { commit.stableDescriptorId };
	publication.workerEligible = false;
	publication.mainThreadRequired = true;
	publication.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
	publication.boundaryClassification = HardBoundaryClassification::Protected;
	publication.hardBoundary = true;
	publication.sameFrameVisibilityRequired = true;
	publication.protectedCommit = "Publishes complete render-frame receipt, diagnostics, profiler evidence, and visibility results only after protected backend work completes.";
	publication.protectedCommit += " Retires the completed continuation and copies terminal scratch diagnostics; reclamation stays at the next protected capture or lifecycle boundary.";
	publication.protectedReason = publication.protectedCommit;
	publication.profilingLabel = "RenderSystem.RenderPublication";

	// Each callback resumes one explicit section of the same frame continuation.
	// This preserves local immutable state without repeating scene capture or
	// reconstructing commands between scheduler phases.
	snapshot.work = [this, deltaTime](JobGraphContext&) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Protected Publication Capture",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		RetireFrameExecutionAtProtectedBoundary();
		RenderSystemLifetimeState::FramePhaseState captured;
		captured.deltaTime = deltaTime;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			captured.captureSequence = lifetimeState->framePhase.captureSequence + 1U;
			captured.execution = std::make_unique<RenderFrameExecution>(
				BuildFrameExecution(deltaTime));
			lifetimeState->framePhase = std::move(captured);
		}
		RenderFrameExecution* execution = nullptr;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			execution = lifetimeState->framePhase.execution.get();
		}
		const bool hasMore = execution != nullptr && execution->Resume();
		std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
		lifetimeState->framePhase.captureReady = hasMore;
	};
	routePrepare.work = [this](JobGraphContext&) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Geometry Route Preparation",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		RenderFrameExecution* execution = nullptr;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			if(!lifetimeState->framePhase.captureReady) return;
			execution = lifetimeState->framePhase.execution.get();
		}
		const bool hasMore = execution != nullptr && execution->Resume();
		std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
		lifetimeState->framePhase.routeReady = hasMore;
	};
	prepare.work = [this](JobGraphContext&) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Virtual Geometry Preparation",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		RenderFrameExecution* execution = nullptr;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			if(!lifetimeState->framePhase.routeReady) return;
			execution = lifetimeState->framePhase.execution.get();
		}
		const bool hasMore = execution != nullptr && execution->Resume();
		std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
		lifetimeState->framePhase.virtualGeometryReady = hasMore;
	};
	commit.work = [this](JobGraphContext&) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Protected Backend Commit",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		RenderFrameExecution* execution = nullptr;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			if(!lifetimeState->framePhase.virtualGeometryReady) return;
			execution = lifetimeState->framePhase.execution.get();
		}
		const bool hasMore = execution != nullptr && execution->Resume();
		std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
		lifetimeState->framePhase.backendComplete = execution != nullptr && !hasMore;
	};
	publication.work = [this](JobGraphContext&) {
		RenderExecutionBoundary boundary(frameExecutionBoundary);
		std::unique_ptr<RenderFrameExecution> retired;
		{
			std::lock_guard phaseLock(lifetimeState->framePhaseMutex);
			if(!lifetimeState->framePhase.backendComplete) return;
		// Update sealed the render root, submitted the backend frame, and swapped
		// complete diagnostics. This final receipt makes that completed boundary
		// explicit to scheduler diagnostics without rebuilding any scene data.
			lifetimeState->framePhase.publicationComplete = true;
			retired = std::move(lifetimeState->framePhase.execution);
		}
		retired.reset();
		ObserveFrameScratchAtProtectedBoundary();
	};
	return { workloads.begin(), workloads.end() };
}

TerraEngine::Scheduling::HardBoundary RenderSystem::GetLiveGpuSubmissionBoundary() const
{
	for (const TerraEngine::Scheduling::HardBoundary& boundary : TerraEngine::Scheduling::InitialWorkerHardBoundaries())
	{
		if (boundary.boundaryName == "LiveGpuSubmission")
		{
			return boundary;
		}
	}
	return {};
}

std::vector<TerraEngine::Scene::RenderRuntimeRecord> RenderSystem::BuildCompactRenderRuntimeRecords()
{
	std::vector<TerraEngine::Scene::RenderRuntimeRecord> records;
	objManager.forEachObjectWithComponents<Transform, CameraComponent>(
		[&](Object object, Transform&, CameraComponent&)
		{
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Camera;
			record.entity = object.EntityId();
			record.objectId = objManager.getUUID(object);
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			records.push_back(std::move(record));
		});

	objManager.forEachObjectWithComponents<Transform, MeshRenderComponent>(
		[&](Object object, Transform&, MeshRenderComponent& mesh)
		{
			// TerrainComponent has its own derived binding and is recorded below.
			// Skipping it here prevents a legacy proxy component from double-counting.
			if(objManager.getRegistry().any_of<TerrainComponent>(object.EntityId())) return;
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Renderable;
			record.entity = object.EntityId();
			record.objectId = mesh.spatialObjectId != TerraEngine::EMPTY_UUID ? mesh.spatialObjectId : objManager.getUUID(object);
			record.resourceIds = { mesh.meshId, mesh.materialId };
			record.renderFlags = mesh.taskShaderCullingEnabled ? 1u : 0u;
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			if (mesh.meshId == TerraEngine::EMPTY_UUID || mesh.materialId == TerraEngine::EMPTY_UUID)
			{
				record.rejectionReason = "missing-render-resource-id";
			}
			records.push_back(std::move(record));
		});

	objManager.forEachObjectWithComponents<Transform, TerrainComponent>(
		[&](Object object, Transform&, TerrainComponent& terrain)
		{
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Renderable;
			record.entity = object.EntityId();
			record.objectId = objManager.getUUID(object);
			record.renderFlags = 0U;
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			const auto proxy = terrain.visible && terrain.terrainAssetId != TerraEngine::EMPTY_UUID
				? TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
					.Resolve(terrain.terrainAssetId)
				: std::nullopt;
			if(proxy && proxy->IsComplete())
			{
				record.resourceIds = {proxy->meshId, proxy->materialId};
			}
			else
			{
				record.rejectionReason = "terrain-render-proxy-unavailable";
			}
			records.push_back(std::move(record));
		});

	objManager.forEachObjectWithComponents<Transform, LightComponent>(
		[&](Object object, Transform&, LightComponent& light)
		{
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Light;
			record.entity = object.EntityId();
			record.objectId = objManager.getUUID(object);
			record.renderFlags = light.castsShadows ? 1u : 0u;
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			records.push_back(std::move(record));
		});

	return records;
}

bool RenderSystem::CompactRenderRecordsMatchSnapshot(const std::vector<TerraEngine::Scene::RenderRuntimeRecord>& records, const TerraEngine::Renderer::RenderFrameSnapshot& snapshot)
{
	std::size_t renderableCount = 0u;
	std::size_t lightCount = 0u;
	for (const TerraEngine::Scene::RenderRuntimeRecord& record : records)
	{
		if (!record.rejectionReason.empty())
		{
			continue;
		}
		if (record.recordKind == TerraEngine::Scene::RuntimeLayoutRecordKind::Renderable)
		{
			++renderableCount;
		}
		else if (record.recordKind == TerraEngine::Scene::RuntimeLayoutRecordKind::Light)
		{
			++lightCount;
		}
	}
	return renderableCount == snapshot.renderables.size() && lightCount == snapshot.lights.size();
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectVirtualGeometryLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	TerraEngine::Renderer::VirtualGeometryDrawRange range{};
	if (asset.detailLevels.empty())
	{
		range.failOpen = true;
		range.fallbackReason = "missing virtual geometry detail metadata";
		return range;
	}

	const VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(quality);
	const auto ApplyDetail = [&asset, &range](std::size_t detailIndex) {
		const VirtualGeometryDetailRuntime& detail = asset.detailLevels[detailIndex];
		range.selectedLodIndex = detail.lodIndex;
		range.meshletOffset = detail.meshletOffset;
		range.meshletCount = detail.meshletCount;
	};

	ApplyDetail(0u);
	if (!taskShaderCullingEnabled)
	{
		range.failOpen = true;
		range.fallbackReason = "task shader culling disabled; using highest-detail fallback";
		return range;
	}
	if (!boundsValid)
	{
		range.failOpen = true;
		range.fallbackReason = "invalid meshlet bounds; using highest-detail fallback";
		return range;
	}

	range.virtualGeometryActive = true;
	range.hiZSelfOcclusionGuardPassed = true;
	range.hiZNearPlaneGuardPassed = true;
	range.hiZThinGeometryGuardPassed = true;
	std::size_t desiredIndex = 0u;
	const float finiteProjectedRadiusPixels = std::isfinite(projectedRadiusPixels)
		? std::max(projectedRadiusPixels, 0.0f)
		: 512.0f;
	// Mesh LOD Scale is documented as preserving detail for longer when raised.
	// Apply it to the whole-mesh screen-size bands as well as cluster error so
	// fallback and sparse-cluster paths obey the same project quality control.
	const float scaledProjectedRadiusPixels =
		finiteProjectedRadiusPixels * sanitized.lodErrorScale;
	constexpr float kFirstVirtualGeometryLodTransitionPixels = 480.0f;
	constexpr float kVirtualGeometryLodTransitionScale = 0.5f;
	// Walk every cooked detail level instead of using two fixed buckets. Each
	// lower LOD becomes valid when the mesh covers roughly half as much screen
	// radius as the previous transition, which makes LOD visualizer colors move
	// at ordinary editor camera distances while still keeping nearby meshes sharp.
	for (std::size_t detailIndex = 1u; detailIndex < asset.detailLevels.size(); ++detailIndex)
	{
		const float transitionPixels = kFirstVirtualGeometryLodTransitionPixels
			* std::pow(kVirtualGeometryLodTransitionScale, static_cast<float>(detailIndex - 1u));
		if (scaledProjectedRadiusPixels >= transitionPixels)
		{
			break;
		}
		desiredIndex = detailIndex;
	}

	if (sanitized.maxLodDrop > 0u)
	{
		desiredIndex = std::min<std::size_t>(desiredIndex, sanitized.maxLodDrop);
	}

	while (desiredIndex > 0u)
	{
		const VirtualGeometryDetailRuntime& detail = asset.detailLevels[desiredIndex];
		const float scaledError = detail.geometricError * sanitized.lodErrorScale;
		if (std::isfinite(scaledError) && scaledError <= 0.1f)
		{
			break;
		}
		--desiredIndex;
	}

	ApplyDetail(desiredIndex);
	return range;
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectDirectResidentLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels)
{
	// Reuse the same artist-facing screen-size and error policy as paged VG, then
	// remove every virtual-only state bit. Direct LODs are ordinary ranges inside
	// an already-resident owner mesh and must never create hierarchy or streaming
	// work merely because they share the detail-record structure.
	TerraEngine::Renderer::VirtualGeometryDrawRange range =
		SelectVirtualGeometryLodRange(
			asset,
			quality,
			projectedRadiusPixels,
			true,
			true);
	range.virtualGeometryActive = false;
	range.failOpen = false;
	range.fallbackReason.clear();
	range.hiZSelfOcclusionGuardPassed = false;
	range.hiZNearPlaneGuardPassed = false;
	range.hiZThinGeometryGuardPassed = false;
	range.hiZInactiveReason.clear();
	return range;
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectVirtualGeometryResidentLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryLodRange(
		asset,
		quality,
		projectedRadiusPixels,
		taskShaderCullingEnabled,
		boundsValid);
	const auto applyDetail = [&asset, &range](std::size_t detailIndex) {
		const VirtualGeometryDetailRuntime& detail = asset.detailLevels[detailIndex];
		range.selectedLodIndex = detail.lodIndex;
		range.meshletOffset = detail.meshletOffset;
		range.meshletCount = detail.meshletCount;
	};

	if (range.failOpen || !range.virtualGeometryActive || asset.pages.empty() || asset.detailLevels.empty())
	{
		return range;
	}

	const std::size_t desiredIndex = DetailIndexForLod(asset, range.selectedLodIndex);
	std::string fallbackReason;
	const std::size_t residentIndex = ResolveResidentVirtualGeometryDetailIndex(asset, desiredIndex, fallbackReason);
	if (residentIndex != desiredIndex)
	{
		applyDetail(residentIndex);
		range.failOpen = true;
		range.fallbackReason = fallbackReason;
	}
	return range;
}

TerraEngine::Renderer::VirtualGeometryClusterSelectionResult RenderSystem::SelectVirtualGeometryClusterCut(
	const VirtualGeometryAssetRuntime& asset,
	const TerraEngine::UUID& meshId,
	const TerraEngine::UUID& objectId,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	return SelectVirtualGeometryClusterCut(
		asset,
		meshId,
		objectId,
		quality,
		projectedRadiusPixels,
		glm::mat4(1.0f),
		glm::vec3(0.0f),
		glm::uvec2(0u),
		taskShaderCullingEnabled,
		boundsValid);
}

TerraEngine::Renderer::VirtualGeometryClusterSelectionResult RenderSystem::SelectVirtualGeometryClusterCut(
	const VirtualGeometryAssetRuntime& asset,
	const TerraEngine::UUID& meshId,
	const TerraEngine::UUID& objectId,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	const glm::mat4& modelMatrix,
	const glm::vec3& cameraPosition,
	const glm::uvec2& targetSize,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result;
	result.range = SelectVirtualGeometryResidentLodRange(
		asset,
		quality,
		projectedRadiusPixels,
		taskShaderCullingEnabled,
		boundsValid);
	if (!result.range.virtualGeometryActive || asset.clusters.empty())
	{
		return result;
	}
	if (!asset.clusterCoverageValidated)
	{
		if (!asset.detailLevels.empty())
		{
			const VirtualGeometryDetailRuntime& safestDetail = asset.detailLevels.front();
			result.range.selectedLodIndex = safestDetail.lodIndex;
			result.range.meshletOffset = safestDetail.meshletOffset;
			result.range.meshletCount = safestDetail.meshletCount;
		}
		result.range.failOpen = true;
		result.range.fallbackReason = "cluster coverage validation missing; using complete highest-detail range";
		result.range.selectedClusterOffset = 0u;
		result.range.selectedClusterCount = 0u;
		result.range.pageNeedOffset = 0u;
		result.range.pageNeedCount = 0u;
		result.invalidSelectionReasons |= 8u;
		return result;
	}

	// Prefer the least-error complete aggregate profile that can answer every
	// missing-page fallback through one existing cluster. This is a typed
	// coverage proof, not permission to pick an unrelated resident cluster.
	const VirtualGeometryAssetRuntime::AggregateProfileRuntime* aggregateFallback = nullptr;
	for (const VirtualGeometryAssetRuntime::AggregateProfileRuntime& profile : asset.aggregateProfiles)
	{
		if (!profile.Valid()
			|| FindVirtualGeometryClusterById(asset, profile.fallbackClusterId) == nullptr)
		{
			continue;
		}
		if (aggregateFallback == nullptr
			|| profile.representation.canonicalError
				< aggregateFallback->representation.canonicalError)
		{
			aggregateFallback = &profile;
		}
	}

	const VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(quality);
	const float finiteProjectedRadiusPixels = std::isfinite(projectedRadiusPixels)
		? std::max(projectedRadiusPixels, 0.0f)
		: 512.0f;

	struct ClusterPick
	{
		std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord> selectedClusters;
		bool substitutedAncestor = false;
		std::uint32_t substitutedAncestorId = kVirtualGeometryInvalidIndex;
	};

	const auto appendSelectedCluster = [&](const VirtualGeometryClusterRuntime& selectedCluster, const VirtualGeometryClusterRuntime& desiredCluster, bool usingFallback, ClusterPick& pick) {
		TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord record;
		record.meshId = meshId;
		record.objectId = objectId;
		record.clusterId = selectedCluster.clusterId;
		record.lodIndex = selectedCluster.lodIndex;
		record.pageId = selectedCluster.pageId;
		record.fallbackClusterId = desiredCluster.fallbackClusterId;
		record.usingFallback = usingFallback;
		pick.selectedClusters.push_back(record);
	};

	const auto appendPageNeed = [&](const VirtualGeometryClusterRuntime& desiredCluster) {
		TerraEngine::Renderer::VirtualGeometryPageNeedRecord need;
		need.meshId = meshId;
		need.objectId = objectId;
		need.pageId = desiredCluster.pageId;
		if (const VirtualGeometryClusterRuntime* fallbackCluster = FindVirtualGeometryClusterById(asset, desiredCluster.fallbackClusterId); fallbackCluster != nullptr)
		{
			need.fallbackPageId = fallbackCluster->pageId;
		}
		else if (aggregateFallback != nullptr)
		{
			need.fallbackPageId = FindVirtualGeometryClusterById(
				asset, aggregateFallback->fallbackClusterId)->pageId;
		}
		need.clusterId = desiredCluster.clusterId;
		need.desiredLodIndex = desiredCluster.lodIndex;
		need.screenImportance = EstimateProjectedClusterRadiusPixels(desiredCluster, modelMatrix, cameraPosition, targetSize, finiteProjectedRadiusPixels)
			* std::max(desiredCluster.geometricError, 0.001f);
		need.fallbackRequired = true;
		need.desiredPayloadSource = VirtualGeometryPagePayloadSource::Missing;
		need.fallbackPayloadSource = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		result.pageNeeds.push_back(need);
	};

	const auto resolveResidentCluster = [&](const VirtualGeometryClusterRuntime& desiredCluster) {
		struct ResolvedCluster
		{
			const VirtualGeometryClusterRuntime* cluster = nullptr;
			bool usingFallback = false;
		};

		if (VirtualGeometryClusterPageIsResident(asset, desiredCluster))
		{
			return ResolvedCluster{ &desiredCluster, false };
		}

		// Page streaming is intentionally fail-open. A missing high-detail page
		// records a need for ResourceStreaming, then walks resident ancestors so
		// the object remains visible instead of waiting on IO during render prep.
		appendPageNeed(desiredCluster);
		std::uint32_t fallbackId = desiredCluster.fallbackClusterId;
		for (std::size_t guard = 0u; guard < asset.clusters.size(); ++guard)
		{
			const VirtualGeometryClusterRuntime* fallbackCluster = FindVirtualGeometryClusterById(asset, fallbackId);
			if (fallbackCluster == nullptr)
			{
				break;
			}
			if (VirtualGeometryClusterPageIsResident(asset, *fallbackCluster))
			{
				return ResolvedCluster{ fallbackCluster, true };
			}
			if (fallbackCluster->fallbackClusterId == fallbackId)
			{
				break;
			}
			fallbackId = fallbackCluster->fallbackClusterId;
		}

		// The authored chain remains the first choice. Only after it cannot
		// produce a resident cluster may the independently validated aggregate
		// profile answer the request, and only when its canonical error bounds
		// the desired cluster's error.
		if (aggregateFallback != nullptr
			&& aggregateFallback->representation.canonicalError >= desiredCluster.geometricError)
		{
			const VirtualGeometryClusterRuntime* aggregateCluster =
				FindVirtualGeometryClusterById(asset, aggregateFallback->fallbackClusterId);
			if (aggregateCluster != nullptr
				&& VirtualGeometryClusterPageIsResident(asset, *aggregateCluster))
			{
				return ResolvedCluster{ aggregateCluster, true };
			}
		}

		// Never substitute an arbitrary resident cluster from another coverage
		// region. That keeps counters alive but renders the wrong island of the
		// mesh, which is worse than failing open to the owner draw range. Only the
		// authored fallback chain is allowed to answer a missing page.
		return ResolvedCluster{};
	};

	const auto shouldRefineCluster = [&](const VirtualGeometryClusterRuntime& cluster) {
		if (cluster.childCount == 0u)
		{
			return false;
		}
		const float clusterProjectedRadiusPixels = EstimateProjectedClusterRadiusPixels(cluster, modelMatrix, cameraPosition, targetSize, finiteProjectedRadiusPixels);
		const float errorPixels = clusterProjectedRadiusPixels
			* std::max(cluster.geometricError, 0.0f)
			* sanitized.lodErrorScale;
		// Cluster refinement is intentionally a little less eager than whole-mesh
		// LOD choice. A tiny error over one pixel can otherwise create noisy
		// mixed-detail cuts on flat or hard-surface assets without improving the
		// visible image. Nearby or high-error regions still refine independently.
		return std::isfinite(errorPixels) && errorPixels > kVirtualGeometryClusterRefinementErrorPixels;
	};

	const auto selectCluster = [&](auto&& self, const VirtualGeometryClusterRuntime& cluster) -> ClusterPick {
		if (shouldRefineCluster(cluster))
		{
			ClusterPick refinedPick;
			for (std::uint32_t childOffset = 0u; childOffset < cluster.childCount; ++childOffset)
			{
				const std::uint32_t childId = cluster.firstChildClusterId + childOffset;
				const VirtualGeometryClusterRuntime* childCluster = FindVirtualGeometryClusterById(asset, childId);
				if (childCluster == nullptr)
				{
					result.invalidSelectionReasons |= 1u;
					continue;
				}

				ClusterPick childPick = self(self, *childCluster);
				if (childPick.substitutedAncestor && childPick.substitutedAncestorId == cluster.clusterId)
				{
					// A desired child page is missing and resolved to this
					// parent. Selecting the parent for the whole subtree avoids
					// drawing a parent beside any of its refined children.
					ClusterPick parentPick;
					appendSelectedCluster(cluster, *childCluster, true, parentPick);
					parentPick.substitutedAncestor = true;
					parentPick.substitutedAncestorId = cluster.clusterId;
					return parentPick;
				}

				refinedPick.selectedClusters.insert(
					refinedPick.selectedClusters.end(),
					childPick.selectedClusters.begin(),
					childPick.selectedClusters.end());
			}

			if (!refinedPick.selectedClusters.empty())
			{
				return refinedPick;
			}
		}

		ClusterPick directPick;
		const auto resolved = resolveResidentCluster(cluster);
		if (resolved.cluster == nullptr)
		{
			result.invalidSelectionReasons |= 2u;
			return directPick;
		}

		appendSelectedCluster(*resolved.cluster, cluster, resolved.usingFallback, directPick);
		if (resolved.usingFallback && resolved.cluster->clusterId != cluster.clusterId)
		{
			directPick.substitutedAncestor = true;
			directPick.substitutedAncestorId = resolved.cluster->clusterId;
		}
		return directPick;
	};

	std::vector<const VirtualGeometryClusterRuntime*> rootClusters;
	rootClusters.reserve(asset.clusters.size());
	for (const VirtualGeometryClusterRuntime& cluster : asset.clusters)
	{
		if (cluster.parentClusterId == kVirtualGeometryInvalidIndex
			|| FindVirtualGeometryClusterById(asset, cluster.parentClusterId) == nullptr)
		{
			rootClusters.push_back(&cluster);
		}
	}
	if (rootClusters.empty())
	{
		rootClusters.push_back(&asset.clusters.front());
		result.invalidSelectionReasons |= 4u;
	}

	for (const VirtualGeometryClusterRuntime* rootCluster : rootClusters)
	{
		ClusterPick pick = selectCluster(selectCluster, *rootCluster);
		result.selectedClusters.insert(
			result.selectedClusters.end(),
			pick.selectedClusters.begin(),
			pick.selectedClusters.end());
	}

	// This selection runs during worker-safe render preparation on immutable
	// asset/page snapshots. Only the prepared draw range and page-need packet
	// cross into the protected backend submission and resource publication path.
	if (!result.selectedClusters.empty())
	{
		if (result.selectedClusters.size() == 1u)
		{
			result.range.selectedLodIndex = result.selectedClusters.front().lodIndex;
		}
	}

	result.range.selectedClusterOffset = 0u;
	result.range.selectedClusterCount = static_cast<std::uint32_t>(result.selectedClusters.size());
	result.range.pageNeedOffset = 0u;
	result.range.pageNeedCount = static_cast<std::uint32_t>(result.pageNeeds.size());
	return result;
}

TerraEngine::Renderer::VirtualGeometryClusterSelectionResult RenderSystem::SelectVirtualGeometryStableClusterCut(
	const VirtualGeometryAssetRuntime& asset,
	const TerraEngine::UUID& meshId,
	const TerraEngine::UUID& objectId,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	const glm::mat4& modelMatrix,
	const glm::vec3& cameraPosition,
	const glm::uvec2& targetSize,
	bool taskShaderCullingEnabled,
	bool boundsValid,
	const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult* previousStableSelection,
	RenderViewType viewType,
	std::uint32_t outputId)
{
	const VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(quality);
	TerraEngine::Renderer::VirtualGeometryClusterSelectionResult candidate = SelectVirtualGeometryClusterCut(
		asset,
		meshId,
		objectId,
		sanitized,
		projectedRadiusPixels,
		modelMatrix,
		cameraPosition,
		targetSize,
		taskShaderCullingEnabled,
		boundsValid);

	const std::uint64_t transitionKey = BuildVirtualGeometryTransitionKey(
		RenderOutputKind::GameView, viewType, outputId, targetSize,
		objectId, meshId);
	const std::uint64_t candidateSignature = BuildVirtualGeometrySelectionSignature(candidate);
	const VirtualGeometryTransitionMetrics metrics = EvaluateVirtualGeometryTransitionMetrics(
		asset,
		sanitized,
		projectedRadiusPixels,
		modelMatrix,
		cameraPosition,
		targetSize);

	const VirtualGeometryLODTransitionState* previousState = previousStableSelection != nullptr && previousStableSelection->transitionState.valid
		? &previousStableSelection->transitionState
		: nullptr;
	const bool resetForDifferentView =
		previousState == nullptr
		|| previousState->viewStateKey != transitionKey
		|| previousState->viewType != viewType
		|| previousState->outputId != outputId
		|| previousState->objectId != objectId
		|| previousState->meshId != meshId;
	const bool resetForModeChange = previousState != nullptr
		&& previousState->debugVisualizationMode != sanitized.debugVisualizationMode;
	const bool resetThisFrame = resetForDifferentView || resetForModeChange;
	const std::uint32_t stableFrameCount = (!resetThisFrame
		&& previousState != nullptr
		&& previousState->candidateSelectionSignature == candidateSignature)
		? std::min(previousState->stableFrameCount + 1u, sanitized.lodTransitionStableFrames)
		: 1u;

	VirtualGeometryLODTransitionState nextState;
	nextState.viewType = viewType;
	nextState.outputId = outputId;
	nextState.objectId = objectId;
	nextState.meshId = meshId;
	nextState.viewStateKey = transitionKey;
	nextState.candidateSelectionSignature = candidateSignature;
	nextState.stableFrameCount = stableFrameCount;
	nextState.hysteresisBandPixels = sanitized.lodTransitionHysteresisPixels;
	nextState.transitionMarginPixels = metrics.transitionMarginPixels;
	nextState.candidateMaxErrorPixels = metrics.candidateMaxErrorPixels;
	nextState.debugVisualizationMode = sanitized.debugVisualizationMode;
	nextState.resetThisFrame = resetThisFrame;
	nextState.valid = true;

	const bool hasPreviousStableCut = previousStableSelection != nullptr
		&& previousState != nullptr
		&& previousState->lastStableSelectionSignature != 0u;
	const bool candidateDiffersFromStable = hasPreviousStableCut
		&& previousState->lastStableSelectionSignature != candidateSignature;
	const bool candidateIsNearThreshold = metrics.transitionCandidateCount > 0u
		&& metrics.transitionMarginPixels <= sanitized.lodTransitionHysteresisPixels;
	const bool shouldHoldPrevious = candidateDiffersFromStable
		&& candidateIsNearThreshold
		&& !resetThisFrame
		&& stableFrameCount < sanitized.lodTransitionStableFrames;

	if (shouldHoldPrevious)
	{
		TerraEngine::Renderer::VirtualGeometryClusterSelectionResult held = *previousStableSelection;
		nextState.lastStableSelectionSignature = previousState->lastStableSelectionSignature;
		nextState.heldPreviousSelection = true;
		nextState.heldFrameCount = previousState->heldFrameCount + 1u;
		nextState.acceptedFrameCount = previousState->acceptedFrameCount;
		held.transitionState = nextState;
		return held;
	}

	nextState.lastStableSelectionSignature = candidateSignature;
	nextState.heldPreviousSelection = false;
	nextState.heldFrameCount = 0u;
	nextState.acceptedFrameCount = hasPreviousStableCut
		? previousState->acceptedFrameCount + (candidateDiffersFromStable ? 1u : 0u)
		: 1u;
	candidate.transitionState = nextState;
	return candidate;
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectVirtualGeometryShadowLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	bool boundsValid)
{
	VirtualGeometryQualityProfile shadowQuality = SanitizeVirtualGeometryQualityProfile(quality);
	shadowQuality.maxLodDrop = 0u;

	TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryResidentLodRange(
		asset,
		shadowQuality,
		1024.0f,
		true,
		boundsValid);
	range.hiZSelfOcclusionGuardPassed = true;
	range.hiZNearPlaneGuardPassed = true;
	range.hiZThinGeometryGuardPassed = true;
	range.hiZInactiveReason.clear();
	if (range.virtualGeometryActive)
	{
		range.selectedLodIndex = asset.detailLevels.empty() ? range.selectedLodIndex : asset.detailLevels.front().lodIndex;
		range.meshletOffset = asset.detailLevels.empty() ? range.meshletOffset : asset.detailLevels.front().meshletOffset;
		range.meshletCount = asset.detailLevels.empty() ? range.meshletCount : asset.detailLevels.front().meshletCount;
	}
	if (!range.failOpen)
	{
		range.fallbackReason = "shadow pass uses conservative virtual geometry visibility";
	}
	return range;
}

// RenderSystem::ReloadMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::ReloadMaterial(const TerraEngine::UUID& materialId)
{
	ResourceManager::Instance().ReloadMaterial(materialId);
	// Surface runtime-contract fallbacks during hot reload so artists can see
	// when a material is rendering with authored defaults instead of textures.
	if (const Material* material = ResourceManager::Instance().GetMaterial(materialId))
	{
		for (const std::string& warning : material->fallbackWarnings)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Material " << TerraEngine::ToHexString(materialId) << ": " << warning);
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (rhi)
	{
		rhi->InvalidateMaterial(materialId);
	}
}

bool RenderSystem::ReloadEditedMaterial(
	const TerraEngine::UUID& materialId,
	const std::filesystem::path& authoredPath)
{
	// The Inspector has already atomically validated and published authoredPath.
	// Replace the live CPU value first, then retire the GPU slot so no frame can
	// rebuild from the previous material generation.
	if(!ResourceManager::Instance().ReloadMaterialFromAuthoredPath(
		materialId, authoredPath))
	{
		return false;
	}
	if(rhi)
	{
		rhi->InvalidateMaterial(materialId);
	}
	return true;
}

// RenderSystem::ReloadMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::ReloadMesh(const TerraEngine::UUID& meshId)
{
	ResourceManager::Instance().ReloadMesh(meshId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (rhi)
	{
		rhi->InvalidateMesh(meshId);
	}
}

void RenderSystem::InvalidateGeneratedMesh(const TerraEngine::UUID& meshId)
{
	if(rhi && meshId != TerraEngine::EMPTY_UUID)
	{
		rhi->InvalidateMesh(meshId);
	}
}

void RenderSystem::InvalidateGeneratedTexture(const TerraEngine::UUID& textureId)
{
	if(rhi && textureId != TerraEngine::EMPTY_UUID)
	{
		rhi->InvalidateTexture(textureId);
	}
}

void RenderSystem::InvalidateGeneratedMaterial(const TerraEngine::UUID& materialId)
{
	// Generated terrain materials have no disk path for ReloadMaterial. Dropping
	// their backend slot makes the next visible draw rebuild MaterialData from the
	// freshly published CPU material and terrain runtime-layer binding.
	if(rhi && materialId != TerraEngine::EMPTY_UUID)
	{
		rhi->InvalidateMaterial(materialId);
	}
}

// RenderSystem::ReloadTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::ReloadTexture(const TerraEngine::UUID& textureId)
{
	auto future = ResourceManager::Instance().ReloadTexture(textureId);
	// Wait for the background transcode to finish before we swap the GPU resource
	future.get();

	if (rhi)
	{
		rhi->InvalidateTexture(textureId);
	}
}

// RenderSystem::DeleteResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::DeleteResource(const TerraEngine::UUID& id)
{
	if (rhi)
	{
		rhi->WaitIdle();
		rhi->InvalidateResource(id);
	}
}

void RenderSystem::RetireLoadedResource(const TerraEngine::UUID& id, std::uint64_t frameId, std::string_view debugName)
{
	if (rhi)
	{
		// Residency cleanup is a cache-pressure path, not an editor delete.
		// Backends can defer physical destruction until submitted frames no
		// longer reference the resource instead of forcing a device-wide idle.
		rhi->RetireLoadedResource(id, frameId, debugName);
	}
}

// RenderSystem::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Clear()
{
	RenderExecutionBoundary boundary(frameExecutionBoundary);
	RetireFrameExecutionAtProtectedBoundary();
	if(lifetimeState->frameScratch) lifetimeState->frameScratch->Retire();
	ObserveFrameScratchAtProtectedBoundary();
	{
		std::lock_guard lock(statsMutex);
		observedFrameScratchOwner.reset();
	}
	if (rhi)
	{
		rhi->ClearResources();
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestGameStats = {};
		latestSceneStats = {};
		latestGameBudgetDiagnostics = {};
		latestSceneBudgetDiagnostics = {};
		latestGameLightingDiagnostics = {};
		latestSceneLightingDiagnostics = {};
		latestRenderSnapshotDiagnostics = {};
		latestRendererRecoverySnapshot = TerraEngine::Renderer::MakeActiveRecoverySnapshot();
		latestAnimationGpuBackendExecution = {};
		latestRenderGraphDiagnostics = {};
		latestRenderFrameSnapshot = {};
		recycledRenderFrameSnapshot = {};
		latestGameVirtualGeometryVisibilityRecords.clear();
		latestSceneVirtualGeometryVisibilityRecords.clear();
		latestGameCaptureVisibilityRecords.clear();
		latestSceneCaptureVisibilityRecords.clear();
		latestGameCaptureIdentitySnapshot = {};
		latestSceneCaptureIdentitySnapshot = {};
		pendingGameIdentitySnapshot = {};
		pendingSceneIdentitySnapshot = {};
		pendingGameIdentityVisibilityRecords.clear();
		pendingSceneIdentityVisibilityRecords.clear();
		latestSceneAuthoritativeIdentityPick.reset();
		latestGameAuthoritativeIdentityPick.reset();
		pendingSceneAuthoritativeIdentityPixel.reset();
		pendingGameAuthoritativeIdentityPixel.reset();
		latestSceneAuthoritativeIdentityFromCapture = false;
		latestGameAuthoritativeIdentityFromCapture = false;
		pendingSceneAuthoritativeIdentityFromCapture = false;
		pendingGameAuthoritativeIdentityFromCapture = false;
		latestGameStats.viewType = RenderViewType::Game;
		latestSceneStats.viewType = RenderViewType::Scene;
		latestGameBudgetDiagnostics.viewType = RenderViewType::Game;
		latestSceneBudgetDiagnostics.viewType = RenderViewType::Scene;
		latestGameLightingDiagnostics.viewType = RenderViewType::Game;
		latestSceneLightingDiagnostics.viewType = RenderViewType::Scene;
		latestSkyRuntimeState = {};
		captureQueueState = {};
		pendingCaptureRequests.clear();
		submittedCaptureReadbackRequests.clear();
		completedCaptureResults.clear();
		virtualGeometryDebugModes.clear();
	}
	hasLatestGameViewCamera = false;
	latestGameViewCamera = {};
	latestGameViewTargetSize = glm::uvec2(0u);
	latestGameViewFrameId = 0u;
	virtualGeometryTransitionCache.clear();
	geometryRoutePolicyStates.clear();
	latestGeometryRouteFramePlans.clear();
	latestGeometryRouteFramePlanSignature = 0U;
	latestGeometryRouteSelectionSignature = 0U;
	latestGeometryRouteSelectionCount = 0U;
	virtualGeometryViewPolicies.clear();
	virtualGeometryReplayReadbacks.Clear();
	latestVirtualGeometryReplayArtifacts.clear();
	// Scene clearing invalidates every stable slot lifetime.  The next render
	// update reopens the store under the backend's current recovery generation.
	renderFramePublicationStore.ResetForSession(
		renderFramePublicationStore.SessionGeneration() + 1u);
	retainedPersistentTopologyRoot.reset();
	retainedDynamicStateRoot.reset();
	retainedCpuDynamicPageIndices.clear();
	dynamicPageRecyclePool.Clear();
	retainedDynamicStateSourceSignature = 0u;
	lifetimeState = std::make_unique<RenderSystemLifetimeState>();
	compatibleAnimationPoseCache.clear();
	renderableMembershipCache.clear();
	retainedRenderableEntries.clear();
	retainedRuntimeMeshResources.clear();
	retainedRuntimeMaterialResources.clear();
	retainedRuntimeMeshOwners.clear();
	retainedRuntimeMaterialOwners.clear();
	retainedMeshLocalBounds.clear();
	retainedRenderableCaptureSourceSignature = 0U;
	retainedRenderableEntriesReusable = false;
	renderableMembershipSceneGeneration = 0U;
	renderableMembershipComponentRevision = 0U;
	renderableMembershipHierarchyRevision = 0U;
	renderableMembershipActiveRevision = 0U;
	renderableMembershipPacketRevision = 0U;
	renderableMembershipTerrainProxyGeneration = 0U;
	TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance().Clear();
	renderableWorldPoseCache.clear();
	renderableModelMatrixSceneGeneration = 0U;
	renderableModelMatrixSpatialRevision = 0U;
	renderableModelMatrixComponentRevision = 0U;
	renderableModelMatrixHierarchyRevision = 0U;
	worldTransformCache.Clear();
	latestTransformCacheDiagnostics = {};
	skyStateDirty = true;
	hasCachedSkyObjectResolution = false;
	cachedSkyObject.reset();
	cachedSkyHierarchyRevision = 0U;
	cachedSkyCandidateSignature = 0U;
	lastLoggedSkyIssueKey.clear();
	ClearOverrideCameras();
}

// RenderSystem::SetTaskShaderCullingEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SetTaskShaderCullingEnabled(bool enabled)
{
	taskShaderCullingEnabled = enabled;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (rhi)
	{
		rhi->SetGlobalTaskShaderCullingEnabled(enabled);
	}
}

// RenderSystem::GetLatestRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderStatsSnapshot RenderSystem::GetLatestRenderStats(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneStats;
	}
	return latestGameStats;
}

// RenderSystem::GetLatestSpatialDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::Scene::SpatialDiagnosticsSnapshot RenderSystem::GetLatestSpatialDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneSpatialDiagnostics;
	}
	return latestGameSpatialDiagnostics;
}

// RenderSystem::GetLatestFrameBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
FrameBudgetDiagnosticsSnapshot RenderSystem::GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneBudgetDiagnostics;
	}
	return latestGameBudgetDiagnostics;
}

// RenderSystem::GetLatestLightingDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
LightingDiagnosticsSnapshot RenderSystem::GetLatestLightingDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneLightingDiagnostics;
	}
	return latestGameLightingDiagnostics;
}

// RenderSystem::GetLatestRenderSnapshotDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderSnapshotDiagnosticsSnapshot RenderSystem::GetLatestRenderSnapshotDiagnostics() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRenderSnapshotDiagnostics;
}

TerraEngine::Renderer::RendererRecoverySnapshot RenderSystem::GetLatestRendererRecoverySnapshot() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRendererRecoverySnapshot;
}

TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot
RenderSystem::GetLatestAnimationGpuBackendExecution() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestAnimationGpuBackendExecution;
}

std::vector<TerraEngine::Renderer::RenderFrameRenderableRecord>
RenderSystem::GetLatestRenderablesSnapshot() const
{
	// The protected renderer owns the source snapshot. Copying while holding the
	// statistics mutex gives editor/MCP readers one internally consistent frame
	// and prevents them from retaining references into the next render update.
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRenderFrameSnapshot.renderables;
}

std::size_t RenderSystem::GetLatestAnimatedGeometryPageDemandCount() const
{
	// Read the same immutable frame publication as renderable diagnostics so the
	// count cannot be paired with a newer or partially prepared demand list.
	std::lock_guard<std::mutex> lock(statsMutex);
	return std::max(latestRenderFrameSnapshot.animatedGeometryPageDemandCount,
		latestRenderFrameSnapshot.animatedGeometryPageDemands.size());
}

std::vector<GpuVirtualGeometryPacketRecord>
RenderSystem::GetLatestVirtualGeometryVisibilityRecords(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	if(viewType == RenderViewType::Scene)
	{
		return latestSceneVirtualGeometryVisibilityRecords;
	}
	return latestGameVirtualGeometryVisibilityRecords;
}

std::vector<TerraEngine::Renderer::OcclusionDebugSample> RenderSystem::GetLatestVirtualGeometryOcclusionDebugSamples(RenderViewType viewType, std::size_t maxCount) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	std::vector<TerraEngine::Renderer::OcclusionDebugSample> samples;
	const std::size_t cappedCount = std::min<std::size_t>(maxCount, TerraEngine::Renderer::kVirtualGeometryOcclusionDebugSampleCapacity);
	for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view : latestRenderGraphDiagnostics.virtualGeometryViews)
	{
		if (view.viewType != viewType)
		{
			continue;
		}
		for (const TerraEngine::Renderer::OcclusionDebugSample& sample : view.occlusionDebugSamples)
		{
			if (samples.size() >= cappedCount)
			{
				return samples;
			}
			samples.push_back(sample);
		}
	}
	return samples;
}

TerraEngine::Renderer::RenderDiagnosticsSummary RenderSystem::GetLatestRenderGraphDiagnostics() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	auto result = latestRenderGraphDiagnostics;
	TerraEngine::Renderer::ApplyFrameScratchDiagnostics(result,
		latestFrameScratch, previousFrameScratch, latestFrameScratchReclamation);
	return result;
}

void RenderSystem::SetFrameScratchValidationCallbacks(
	std::shared_ptr<const TerraEngine::Renderer::RenderFrameScratchValidationCallbacks> callbacks)
{
	RenderExecutionBoundary boundary(frameExecutionBoundary);
#if !defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	if(callbacks) throw std::logic_error("Native renderer scratch validation is unavailable in this build");
#endif
	frameScratchValidationCallbacks = std::move(callbacks);
}

TerraEngine::Renderer::RenderFrameScratchValidationState RenderSystem::GetFrameScratchValidationState() const
{
	std::lock_guard lock(statsMutex);
	TerraEngine::Renderer::RenderFrameScratchValidationState result;
	result.active = observedFrameScratchOwner && observedFrameScratchOwner->IsActive();
	result.latest = latestFrameScratch;
	result.previous = previousFrameScratch;
	result.reclamation = latestFrameScratchReclamation;
	return result;
}

std::vector<TerraEngine::Renderer::BackendStageTiming> RenderSystem::GetLatestBackendStageTimings() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRenderGraphDiagnostics.backendFrameBreakdown.stageTimings;
}

TerraEngine::Renderer::RenderCaptureLimits RenderSystem::GetCaptureLimits() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return captureLimits;
}

TerraEngine::Renderer::RenderCaptureQueueState RenderSystem::GetCaptureQueueState() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return captureQueueState;
}

TerraEngine::Renderer::RenderCaptureAvailability RenderSystem::GetCaptureAvailability() const
{
	TerraEngine::Renderer::RenderCaptureAvailability availability;
	availability.rendererInitialized = initialized && rhi != nullptr;
	if (rhi)
	{
		const TerraEngine::Renderer::BackendCapabilityProfile profile = rhi->GetBackendCapabilityProfile();
		availability.backendId = profile.backendId;
		availability.backendReadbackAvailable = profile.supportsImageReadback;
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for (const TerraEngine::Renderer::RenderFrameViewRequest& request : latestRenderFrameSnapshot.viewRequests)
		{
			if (request.viewType == RenderViewType::Game)
			{
				availability.gameViewAvailable = true;
				break;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		availability.limits = captureLimits;
		availability.queueState = captureQueueState;
		availability.queueFull = captureQueueState.pendingCaptureCount >= captureLimits.maxQueuedCaptures;
		availability.overBudget = false;
		if (hasLatestGameViewCamera)
		{
			availability.gameViewAvailable = true;
		}
	}
	if (availability.queueFull)
	{
		availability.available = false;
		availability.state = "queueFull";
		availability.message = "Capture queue is full.";
		availability.retryable = true;
	}
	else if (!availability.rendererInitialized && !availability.deterministicFallbackAvailable)
	{
		availability.available = false;
		availability.state = "rendererUnavailable";
		availability.message = "Renderer is not initialized and no deterministic capture fallback is available.";
		availability.retryable = true;
	}
	if (!availability.backendReadbackAvailable)
	{
		availability.notes.push_back("Backend image readback is unavailable; captures use the deterministic PNG fallback.");
	}
	if (!availability.gameViewAvailable)
	{
		availability.notes.push_back("No completed Game View frame is currently published.");
	}
	return availability;
}

std::string RenderSystem::QueueCaptureRequest(TerraEngine::Renderer::RenderCaptureRequest request, std::string& errorMessage)
{
	if (request.targetSize.x == 0u || request.targetSize.y == 0u)
	{
		errorMessage = "Capture target size must be greater than zero.";
		return {};
	}
	std::lock_guard<std::mutex> lock(statsMutex);
	if ((captureLimits.maxWidth && request.targetSize.x > *captureLimits.maxWidth)
		|| (captureLimits.maxHeight && request.targetSize.y > *captureLimits.maxHeight))
	{
		errorMessage = "Capture target size exceeds configured limits.";
		return {};
	}
	if (pendingCaptureRequests.size() >= captureLimits.maxQueuedCaptures)
	{
		errorMessage = "Capture queue is full.";
		return {};
	}
	if (request.requestId.empty())
	{
		request.requestId = "render-capture-" + std::to_string(nextCaptureRequestSequence++);
	}
	if (request.timeoutMs == 0u)
	{
		request.timeoutMs = captureLimits.defaultTimeoutMs;
	}
	pendingCaptureRequests.push_back(std::move(request));
	captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
	errorMessage.clear();
	return pendingCaptureRequests.back().requestId;
}

std::optional<TerraEngine::Renderer::RenderCaptureResult> RenderSystem::PollCaptureResult(std::string_view requestId)
{
	std::lock_guard<std::mutex> lock(statsMutex);
	for (auto it = completedCaptureResults.begin(); it != completedCaptureResults.end(); ++it)
	{
		if (it->requestId == requestId)
		{
			TerraEngine::Renderer::RenderCaptureResult result = std::move(*it);
			completedCaptureResults.erase(it);
			captureQueueState.completedResultCount = static_cast<std::uint32_t>(completedCaptureResults.size());
			return result;
		}
	}
	return std::nullopt;
}

void RenderSystem::ProcessQueuedCaptureRequests()
{
	std::deque<TerraEngine::Renderer::RenderCaptureRequest> requests;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		while (!submittedCaptureReadbackRequests.empty())
		{
			requests.push_back(std::move(submittedCaptureReadbackRequests.front()));
			submittedCaptureReadbackRequests.pop_front();
		}
		captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
	}

	if (requests.empty())
	{
		const bool dedicatedBackendAvailable = initialized && rhi
			&& rhi->GetBackendCapabilityProfile().supportsImageReadback;
		std::lock_guard<std::mutex> lock(statsMutex);
		// Only an existing viewport readback may bypass render preparation. Custom,
		// camera-object, and dedicated next-frame Game captures must remain pending
		// until RenderSystem builds their view, seals its immutable publication, and
		// moves the exact request to submittedCaptureReadbackRequests. Draining those
		// requests here raced the renderer under heavy workloads and copied a cleared
		// capture target before its commands had even been submitted.
		for (auto iterator = pendingCaptureRequests.begin();
			iterator != pendingCaptureRequests.end()
			&& captureQueueState.activeCaptureCount < captureLimits.maxConcurrentCaptures;)
		{
			const bool requiresDedicatedRender =
				iterator->sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView
				|| iterator->dedicatedRenderTarget;
			if (requiresDedicatedRender && dedicatedBackendAvailable)
			{
				++iterator;
				continue;
			}
			requests.push_back(std::move(*iterator));
			iterator = pendingCaptureRequests.erase(iterator);
			++captureQueueState.activeCaptureCount;
		}
		captureQueueState.pendingCaptureCount = static_cast<std::uint32_t>(pendingCaptureRequests.size());
	}

	for (const TerraEngine::Renderer::RenderCaptureRequest& request : requests)
	{
		TerraEngine::Renderer::RenderCaptureResult result;
		result.requestId = request.requestId;
		result.sourceKind = request.sourceKind;
		result.sourceName = request.sourceName;
		result.size = request.targetSize;
		// Game View captures report the last frame that actually submitted a
		// Game/runtime view; custom offscreen captures report the frame boundary
		// that drained the queue. Both values are read-only diagnostics.
		const std::uint64_t captureFrameId = request.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
			&& !request.dedicatedRenderTarget
			&& latestGameViewFrameId != 0u
			? latestGameViewFrameId
			: activeRenderFrameId;
		result.frameId = captureFrameId;
		result.freshness = request.freshness;
		result.overlaysIncluded = request.includeOverlays;

		// A normal Game View readback needs the last frame published by the dock.
		// A dedicated request already contains an immutable authored-camera
		// snapshot, so it remains valid even when the dock has never rendered.
		if (request.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
			&& !request.dedicatedRenderTarget
			&& !hasLatestGameViewCamera)
		{
			result.message = "No active Game View camera is available.";
			result.errorCategory = "missing_target";
		}
		else if (request.format != "image/png")
		{
			result.message = "Only image/png capture output is currently supported.";
			result.errorCategory = "unsupported_readback";
		}
		else if (rhi && rhi->GetBackendCapabilityProfile().supportsImageReadback)
		{
			// Stamp the immutable request copy with the exact frame whose output
			// the backend is about to copy. The caller-owned descriptor remains
			// unchanged and can be reused safely.
			TerraEngine::Renderer::RenderCaptureRequest backendRequest = request;
			backendRequest.submittedFrameId = captureFrameId;
			result = rhi->ReadbackCapture(backendRequest);
			result.requestId = request.requestId;
			result.sourceKind = request.sourceKind;
			result.sourceName = request.sourceName;
			if (result.size.x == 0u || result.size.y == 0u)
			{
				result.size = request.targetSize;
			}
			if (result.frameId == 0u)
			{
				result.frameId = captureFrameId;
			}
			result.freshness = request.freshness;
		}

		const bool backendFallbackEligible = result.errorCategory == "internal"
			|| result.errorCategory == "backend_readback_unavailable"
			|| result.errorCategory == "backend_error";
		if (!result.ok && backendFallbackEligible && request.allowBackendFallback)
		{
			result.ok = true;
			result.message = "Captured deterministic fallback image because backend readback is not available.";
			result.errorCategory.clear();
			result.mimeType = "image/png";
			result.base64Data = kDeterministicCapturePngBase64;
			result.size = request.targetSize;
			result.requestId = request.requestId;
			result.sourceKind = request.sourceKind;
			result.sourceName = request.sourceName;
			result.frameId = captureFrameId;
			result.freshness = request.freshness;
			result.overlaysIncluded = request.includeOverlays;
			result.backendReadback = false;
			result.deterministicFallback = true;
			result.warnings.push_back("Deterministic fallback image does not contain live GPU pixels.");
		}

		if (!result.pendingAsync)
		{
			CompleteCapture(result);
		}
	}
}

void RenderSystem::CompleteCapture(TerraEngine::Renderer::RenderCaptureResult result)
{
	std::lock_guard<std::mutex> lock(statsMutex);
	completedCaptureResults.push_back(result);
	while (completedCaptureResults.size() > captureLimits.maxQueuedCaptures)
	{
		completedCaptureResults.pop_front();
	}
	if (captureQueueState.activeCaptureCount > 0u)
	{
		--captureQueueState.activeCaptureCount;
	}
	captureQueueState.completedResultCount = static_cast<std::uint32_t>(completedCaptureResults.size());
	latestRenderGraphDiagnostics.captureSchedulers.push_back(
		TerraEngine::Renderer::BuildRenderCaptureSchedulerDiagnostics(
			"RenderSystem",
			"Render",
			TerraEngine::Renderer::ToString(result.sourceKind),
			result.backendReadback,
			result.deterministicFallback));
	if (latestRenderGraphDiagnostics.captureSchedulers.size() > 16u)
	{
		latestRenderGraphDiagnostics.captureSchedulers.erase(latestRenderGraphDiagnostics.captureSchedulers.begin());
	}
}

void RenderSystem::DrainCaptureReadbacks()
{
	if (!rhi)
	{
		return;
	}
	// PollCaptureReadbacks never waits for the GPU. Each completed item is
	// committed through the same bounded queue path as synchronous fallbacks.
	for (TerraEngine::Renderer::RenderCaptureResult& result : rhi->PollCaptureReadbacks())
	{
		CompleteCapture(std::move(result));
	}
	DrainConventionalVisibilityIdentityReadbacks();
}

void RenderSystem::DrainConventionalVisibilityIdentityReadbacks()
{
	if (!rhi) return;
	for (const TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult& result :
		rhi->PollConventionalVisibilityIdentityReadbacks()) {
		std::lock_guard<std::mutex> lock(statsMutex);
		std::optional<glm::uvec2>& pendingPixel = result.viewType == RenderViewType::Scene
			? pendingSceneAuthoritativeIdentityPixel : pendingGameAuthoritativeIdentityPixel;
		bool& pendingFromCapture = result.viewType == RenderViewType::Scene
			? pendingSceneAuthoritativeIdentityFromCapture : pendingGameAuthoritativeIdentityFromCapture;
		CaptureConventionalVisibilityIdentitySnapshot& pendingSnapshot =
			result.viewType == RenderViewType::Scene
				? pendingSceneIdentitySnapshot : pendingGameIdentitySnapshot;
		std::vector<GpuVirtualGeometryPacketRecord>& pendingVisibilityRecords =
			result.viewType == RenderViewType::Scene
				? pendingSceneIdentityVisibilityRecords
				: pendingGameIdentityVisibilityRecords;
		std::string& failureReason = result.viewType == RenderViewType::Scene
			? latestSceneAuthoritativeIdentityFailureReason : latestGameAuthoritativeIdentityFailureReason;
		// A later request may replace this pixel on the other target. Ignore that
		// stale completion instead of letting it satisfy the newer target selector.
		if (!pendingPixel || *pendingPixel != result.pixel
			|| pendingFromCapture != result.dedicatedCaptureTarget) continue;
		pendingPixel.reset();
		CaptureConventionalVisibilityIdentitySnapshot resolvedSnapshot =
			std::move(pendingSnapshot);
		std::vector<GpuVirtualGeometryPacketRecord> resolvedVisibilityRecords =
			std::move(pendingVisibilityRecords);
		pendingSnapshot = {};
		pendingVisibilityRecords.clear();
		if (!result.completed || !result.authoritative
			|| result.frameId == 0u || result.identity == glm::uvec4(0u)) {
			failureReason = "The Vulkan identity copy completed without an authoritative nonzero uint4 result.";
			continue;
		}
		if (!resolvedSnapshot.IsUsable()
			|| result.rendererGeneration != resolvedSnapshot.rendererGeneration) {
			failureReason = "The Vulkan identity copy belonged to a stale renderer generation.";
			continue;
		}
		if(result.targetGeneration != resolvedSnapshot.targetGeneration)
		{
			failureReason = "The Vulkan identity copy belonged to a resized or recreated target.";
			continue;
		}
		if (result.originGeneration != resolvedSnapshot.originGeneration
			|| result.spatialGeneration != resolvedSnapshot.spatialGeneration
			|| result.residencyGeneration != resolvedSnapshot.residencyGeneration)
		{
			failureReason = "The Vulkan identity copy belonged to a stale origin, spatial, or residency generation.";
			continue;
		}
		const std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityRecord>& identityTable =
			resolvedSnapshot.table;
		if (resolvedSnapshot.frameId != result.frameId || identityTable.empty()) {
			failureReason = "The completed Vulkan identity belonged to an expired immutable frame snapshot.";
			continue;
		}
		const auto identityRecord = std::find_if(
			identityTable.begin(),
			identityTable.end(),
			[&result](const TerraEngine::Renderer::ConventionalVisibilityIdentityRecord& candidate) {
				return candidate.index == result.identity.y
					&& candidate.objectId != TerraEngine::EMPTY_UUID
					&& candidate.meshId != TerraEngine::EMPTY_UUID;
			});
		if (identityRecord == identityTable.end()) {
			// Include the failed key so editor diagnostics can distinguish a missing
			// producer key from a nonzero key belonging to a different object table.
			failureReason = "The completed Vulkan identity key " + std::to_string(result.identity.y)
				+ " did not match the immutable frame object table ("
				+ std::to_string(identityTable.size()) + " entries).";
			continue;
		}
		VirtualGeometryVisibilityPick pick{};
		pick.objectId = identityRecord->objectId;
		pick.meshId = identityRecord->meshId;
		pick.materialId = identityRecord->materialId;
		pick.frameId = result.frameId;
		pick.originGeneration = result.originGeneration;
		pick.spatialGeneration = result.spatialGeneration;
		pick.residencyGeneration = result.residencyGeneration;
		pick.meshletId = result.identity.x;
		// VG packet metadata remains useful when it agrees with the same snapshot
		// object. Direct meshes intentionally leave these fields at their neutral
		// values rather than being rejected for lacking a packet.
		const std::vector<GpuVirtualGeometryPacketRecord>& records =
			resolvedVisibilityRecords;
		const auto packet = std::find_if(records.begin(), records.end(),
			[&result, &pick](const GpuVirtualGeometryPacketRecord& candidate) {
				return candidate.frameId == result.frameId
					&& candidate.payloadDescriptorIndex == result.identity.z
					&& candidate.objectId == pick.objectId
					&& candidate.meshId == pick.meshId;
			});
		if (packet != records.end()) {
			pick.packetId = packet->packetId;
			pick.clusterId = packet->clusterId;
			pick.selectedLod = packet->selectedLod;
			pick.debugIdentity = packet->debugIdentity;
			pick.fallback = (packet->flags & 0x1u) != 0u;
		}
		if (result.viewType == RenderViewType::Scene) {
			latestSceneAuthoritativeIdentityPick = pick;
			latestSceneAuthoritativeIdentityPixel = result.pixel;
			latestSceneAuthoritativeIdentityFromCapture = result.dedicatedCaptureTarget;
			latestSceneAuthoritativeIdentityFailureReason.clear();
		} else {
			latestGameAuthoritativeIdentityPick = pick;
			latestGameAuthoritativeIdentityPixel = result.pixel;
			latestGameAuthoritativeIdentityFromCapture = result.dedicatedCaptureTarget;
			latestGameAuthoritativeIdentityFailureReason.clear();
		}
	}
}

TerraEngine::Renderer::RenderCaptureResult RenderSystem::CaptureImage(const TerraEngine::Renderer::RenderCaptureRequest& request)
{
	// Admission precedes QueueCaptureRequest: rejected recursive capture must not
	// leave an orphan request. The intentional immediate update shares this gate.
	std::optional<RenderExecutionBoundary> boundary;
	boundary.emplace(frameExecutionBoundary);
	TerraEngine::Renderer::RenderCaptureRequest preparedRequest = request;
	if (preparedRequest.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
		&& preparedRequest.freshness == "nextFrame"
		&& !preparedRequest.dedicatedRenderTarget
		&& hasLatestGameViewCamera)
	{
		// Synchronous MCP calls cannot wait for the normal editor loop without
		// blocking that same loop. Snapshot the latest published Game View camera
		// and render one dedicated frame so nextFrame has real, bounded semantics
		// even when the Game View dock tab is occluded.
		preparedRequest.viewMatrix = latestGameViewCamera.view;
		preparedRequest.projectionMatrix = latestGameViewCamera.proj;
		preparedRequest.cameraCanonicalPosition =
			latestGameViewCamera.canonicalPosition;
		preparedRequest.dedicatedRenderTarget = true;
	}
	std::string errorMessage;
	std::string requestId = QueueCaptureRequest(preparedRequest, errorMessage);
	if (requestId.empty())
	{
		TerraEngine::Renderer::RenderCaptureResult result;
		result.requestId = preparedRequest.requestId;
		result.sourceKind = preparedRequest.sourceKind;
		result.sourceName = preparedRequest.sourceName;
		result.size = preparedRequest.targetSize;
		result.message = errorMessage;
		result.errorCategory = errorMessage.find("queue") != std::string::npos ? "queue_full" : "over_budget";
		return result;
	}
	const bool shouldRenderDedicatedCaptureImmediately = (preparedRequest.sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView
			|| preparedRequest.dedicatedRenderTarget)
		&& initialized
		&& rhi
		&& rhi->GetBackendCapabilityProfile().supportsImageReadback
		&& preparedRequest.format == "image/png";
	if (shouldRenderDedicatedCaptureImmediately)
	{
		// MCP tool calls are synchronous. One render update submits the dedicated
		// object/custom offscreen pass; subsequent iterations poll only its fence
		// instead of recursively rendering many frames.
		UpdateAtExecutionBoundary(0.0f);
	}
	else
	{
		ProcessQueuedCaptureRequests();
	}
	boundary.reset();

	const std::chrono::milliseconds timeout(preparedRequest.timeoutMs > 0u ? preparedRequest.timeoutMs : captureLimits.defaultTimeoutMs);
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	do
	{
		DrainCaptureReadbacks();
		if (std::optional<TerraEngine::Renderer::RenderCaptureResult> result = PollCaptureResult(requestId))
		{
			return *result;
		}
		std::this_thread::yield();
	} while (std::chrono::steady_clock::now() < deadline);

	// Give the backend one final poll at the exact deadline so its own timeout
	// result wins over a synthetic facade timeout and queue accounting remains
	// consistent.
	DrainCaptureReadbacks();
	if (std::optional<TerraEngine::Renderer::RenderCaptureResult> completed = PollCaptureResult(requestId))
	{
		return *completed;
	}

	TerraEngine::Renderer::RenderCaptureResult result;
	result.requestId = requestId;
	result.sourceKind = preparedRequest.sourceKind;
	result.sourceName = preparedRequest.sourceName;
	result.size = preparedRequest.targetSize;
	result.message = "Capture request did not complete before polling.";
	result.errorCategory = "timeout";
	return result;
}

void RenderSystem::SetRenderGraphDiagnosticsRequested(bool requested)
{
	renderGraphDiagnosticsRequestedByUi = requested;
	ApplyRenderDiagnosticsRequest();
}

bool RenderSystem::ResolveRenderDebugDiagnosticsEnabled() const
{
	// Editor Preferences are per-user state, so they override project-authored
	// debug diagnostics while the editor is running without changing the saved
	// RenderSettings.json value.
	return transientRenderDebugDiagnosticsOverride.value_or(currentSettings.debugEnabled > 0u);
}

void RenderSystem::ApplyRenderDiagnosticsRequest()
{
	// Diagnostics can come from a user preference override, project render
	// settings, or a temporary diagnostics window. Resolve the saved/project
	// side first, then OR in windows that only need capture data while open.
	const bool requested = renderGraphDiagnosticsRequestedByUi || ResolveRenderDebugDiagnosticsEnabled();
	TerraEngine::Renderer::SetRenderDiagnosticsRequested(requested);
	if (!requested)
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRenderGraphDiagnostics = {};
	}
}

// RenderSystem::GetLatestSkyRuntimeState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
SkyRuntimeState RenderSystem::GetLatestSkyRuntimeState() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestSkyRuntimeState;
}

// RenderSystem::MarkSkyStateDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::MarkSkyStateDirty()
{
	skyStateDirty = true;
	lastLoggedSkyIssueKey.clear();
}

// RenderSystem::SetQualityPreset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SetQualityPreset(QualityPreset preset)
{
	ApplyQualityPreset(preset, true);
}

void RenderSystem::ApplyQualityPreset(QualityPreset preset, bool persistProjectSettings)
{
	if (preset == QualityPreset::Custom)
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		currentPreset = QualityPreset::Custom;
		return;
	}

	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	const std::uint32_t priorTerrainAnisotropy = currentSettings.anisotropicFiltering;
	currentPreset = preset;
	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (preset)
	{
	case QualityPreset::Low:
		currentSettings.shadowResolution = 512u;
		currentSettings.shadowFilteringSize = 1u;
		currentSettings.maxUploadedLights = 64u;
		currentSettings.maxDynamicLights = 32u;
		currentSettings.maxShadowLights = 1u;
		currentSettings.maxAreaLights = 0u;
		currentSettings.enableLTC = 0u;
		currentSettings.enableIBL = 0u;
		currentSettings.indirectStrength = 0.35f;
		currentSettings.ambientOcclusionStrength = 0.5f;
		currentSettings.ambientOcclusionQuality = 0u;
		currentSettings.anisotropicFiltering = 1u;
		currentSettings.variableRateShading = 0u;
		break;
	case QualityPreset::Medium:
		currentSettings.shadowResolution = 1024u;
		currentSettings.shadowFilteringSize = 3u;
		currentSettings.maxUploadedLights = 128u;
		currentSettings.maxDynamicLights = 64u;
		currentSettings.maxShadowLights = 2u;
		currentSettings.maxAreaLights = 2u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 0.75f;
		currentSettings.ambientOcclusionStrength = 0.75f;
		currentSettings.ambientOcclusionQuality = 1u;
		currentSettings.anisotropicFiltering = 1u;
		currentSettings.variableRateShading = 0u;
		break;
	case QualityPreset::High:
		currentSettings.shadowResolution = 2048u;
		currentSettings.shadowFilteringSize = 5u;
		currentSettings.maxUploadedLights = 256u;
		currentSettings.maxDynamicLights = 128u;
		currentSettings.maxShadowLights = 3u;
		currentSettings.maxAreaLights = 4u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 1.0f;
		currentSettings.ambientOcclusionStrength = 1.0f;
		currentSettings.ambientOcclusionQuality = 2u;
		currentSettings.anisotropicFiltering = 4u;
		currentSettings.variableRateShading = 0u;
		break;
	case QualityPreset::Ultra:
		currentSettings.shadowResolution = 4096u;
		currentSettings.shadowFilteringSize = 7u;
		currentSettings.maxUploadedLights = 512u;
		currentSettings.maxDynamicLights = 256u;
		currentSettings.maxShadowLights = 8u;
		currentSettings.maxAreaLights = 8u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 1.25f;
		currentSettings.ambientOcclusionStrength = 1.25f;
		currentSettings.ambientOcclusionQuality = 3u;
		currentSettings.anisotropicFiltering = 16u;
		currentSettings.variableRateShading = 0u;
		break;
	}

	currentSettings = SanitizeRenderSettings(currentSettings);
	if(priorTerrainAnisotropy != currentSettings.anisotropicFiltering)
	{
		for(const TerraEngine::UUID& materialId :
			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().RepublishSamplerQuality(
				currentSettings.anisotropicFiltering))
		{
			InvalidateGeneratedMaterial(materialId);
		}
	}
	if (persistProjectSettings)
	{
		SaveSettings();
	}
}

// RenderSystem::SetRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SetRenderSettings(const RenderSettings& settings)
{
	const std::uint32_t priorTerrainAnisotropy = currentSettings.anisotropicFiltering;
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		currentSettings = SanitizeRenderSettings(settings);
		currentPreset = QualityPreset::Custom;
	}
	if(priorTerrainAnisotropy != currentSettings.anisotropicFiltering)
	{
		for(const TerraEngine::UUID& materialId :
			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().RepublishSamplerQuality(
				currentSettings.anisotropicFiltering))
		{
			InvalidateGeneratedMaterial(materialId);
		}
	}
	ResourceManager::Instance().SetRenderDebugEnabled(ResolveRenderDebugDiagnosticsEnabled());
	ApplyRenderDiagnosticsRequest();
	if (auto* vulkanContext = dynamic_cast<VulkanContext*>(rhi.get()))
	{
		vulkanContext->SetVariableRateShadingEnabled(currentSettings.variableRateShading > 0u);
	}
	SaveSettings();
}

bool RenderSystem::QueueRuntimeQualityPreset(QualityPreset preset)
{
	if (preset > QualityPreset::Ultra)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	pendingRuntimeRenderSettings = RuntimeRenderSettingsRequest{ {}, preset, true };
	return true;
}

bool RenderSystem::QueueRuntimeRenderSettings(const RenderSettings& settings)
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	pendingRuntimeRenderSettings = RuntimeRenderSettingsRequest{
		SanitizeRenderSettings(settings), QualityPreset::Custom, false };
	return true;
}

RenderSettings RenderSystem::GetRuntimeRenderSettingsSnapshot() const
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	return currentSettings;
}

QualityPreset RenderSystem::GetRuntimeQualityPresetSnapshot() const
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	return currentPreset;
}

std::uint64_t RenderSystem::GetRuntimeRenderSettingsGeneration() const
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	return runtimeRenderSettingsGeneration;
}

void RenderSystem::ApplyQueuedRuntimeRenderSettings()
{
	// The existing capture boundary is the sole consumer of transient options.
	// Recovery changes token ownership before any prior-session ingress can apply.
	const std::uint64_t backendSession = rhi
		? rhi->GetLatestRendererRecoverySnapshot().renderSessionGeneration : 0U;
	std::optional<RuntimeRenderSettingsRequest> request;
	std::vector<RendererResourcePublicationInvalidation> resourceInvalidations;
	std::uint64_t currentOptionsEpoch = 0;
	bool resetEfficiencyEvidence = false;
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		if (efficiencyObservedBackendSession != 0U && backendSession != efficiencyObservedBackendSession)
		{
			rendererEfficiencyOptionsQueue.InvalidateSession();
			resetEfficiencyEvidence = true;
		}
		efficiencyObservedBackendSession = backendSession;
		rendererEfficiencyOptionsQueue.BindRendererSession(backendSession);
		rendererEfficiencyOptionsQueue.CaptureAtProtectedBoundary();
		const auto options = rendererEfficiencyOptionsQueue.Snapshot();
		currentOptionsEpoch = options.optionsSessionGeneration;
		resourceInvalidations.swap(pendingRendererResourceInvalidations);
		if (pendingRendererEfficiencyFixtureIntent)
		{
			const auto& correlation = pendingRendererEfficiencyFixtureIntent->correlation;
			if (correlation.settingsGeneration == options.settingsGeneration
				&& correlation.rendererSessionGeneration == options.rendererSessionGeneration)
				capturedRendererEfficiencyFixtureIntent = std::move(*pendingRendererEfficiencyFixtureIntent);
			pendingRendererEfficiencyFixtureIntent.reset();
		}
		if (capturedRendererEfficiencyFixtureIntent.correlation.settingsGeneration != options.settingsGeneration
			|| capturedRendererEfficiencyFixtureIntent.correlation.rendererSessionGeneration != options.rendererSessionGeneration)
			capturedRendererEfficiencyFixtureIntent = {};
		request = std::move(pendingRuntimeRenderSettings);
		pendingRuntimeRenderSettings.reset();
	}
	if (resetEfficiencyEvidence)
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRendererEfficiencyEvidence.clear(); latestRendererEfficiencyFixtureTargets.clear();
		latestRenderFrameSnapshot.efficiencyFixtureIntent = {};
		TerraEngine::Renderer::ResetRendererEfficiencyFixtureEvidenceBatch(latestRendererEfficiencyEvidenceBatch);
	}
	// Actual asynchronous loader publication happens before ingress. Only this
	// existing protected boundary invalidates GPU payloads; it neither reloads CPU
	// resources nor introduces a scene mutation flush or a new GPU wait.
	const auto publicationGeneration = ResourceManager::Instance().GetRendererResourcePublicationGeneration();
	for (const auto& invalidation : resourceInvalidations)
	{
		if (!rhi || invalidation.optionsSessionGeneration != currentOptionsEpoch || !invalidation.publicationGeneration
			|| invalidation.publicationGeneration > publicationGeneration) continue;
		switch (invalidation.type)
		{
		case AssetType::Mesh: rhi->InvalidateMesh(invalidation.assetId); break;
		case AssetType::Material: rhi->InvalidateMaterial(invalidation.assetId); break;
		case AssetType::Texture: rhi->InvalidateTexture(invalidation.assetId); break;
		default: break;
		}
	}
	if (!request)
	{
		return;
	}
	const std::uint32_t priorTerrainAnisotropy = currentSettings.anisotropicFiltering;

	if (request->usePreset)
	{
		ApplyQualityPreset(request->preset, false);
	}
	else
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		currentSettings = SanitizeRenderSettings(request->settings);
		currentPreset = QualityPreset::Custom;
	}
	if(priorTerrainAnisotropy != currentSettings.anisotropicFiltering)
	{
		for(const TerraEngine::UUID& materialId :
			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().RepublishSamplerQuality(
				currentSettings.anisotropicFiltering))
		{
			InvalidateGeneratedMaterial(materialId);
		}
	}

	// Backend-affecting toggles are now applied by RenderSystem itself rather
	// than by the worker that submitted the managed request.
	ResourceManager::Instance().SetRenderDebugEnabled(ResolveRenderDebugDiagnosticsEnabled());
	ApplyRenderDiagnosticsRequest();
	if (auto* vulkanContext = dynamic_cast<VulkanContext*>(rhi.get()))
	{
		vulkanContext->SetVariableRateShadingEnabled(currentSettings.variableRateShading > 0U);
	}
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		++runtimeRenderSettingsGeneration;
	}
}

TerraEngine::Renderer::RendererEfficiencyOptionsReceipt RenderSystem::QueueRendererEfficiencyOptions(
	const TerraEngine::Renderer::RendererEfficiencyOptions& options)
{
	std::lock_guard lock(runtimeRenderSettingsMutex);
	return rendererEfficiencyOptionsQueue.Queue(options);
}

TerraEngine::Renderer::RendererEfficiencyOptionsReceipt RenderSystem::QueueRendererEfficiencyRestore(
	const TerraEngine::Renderer::RendererEfficiencyOptionsToken& token)
{
	std::lock_guard lock(runtimeRenderSettingsMutex);
	return rendererEfficiencyOptionsQueue.Restore(token);
}

TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot RenderSystem::GetRendererEfficiencyOptionsSnapshot() const
{
	std::lock_guard lock(runtimeRenderSettingsMutex);
	return rendererEfficiencyOptionsQueue.Snapshot();
}

TerraEngine::Renderer::RendererEfficiencyEvidence RenderSystem::GetLatestRendererEfficiencyEvidence(
	RenderViewType viewType, std::uint32_t viewId) const
{
	// Do not nest the ingress and diagnostic locks: requests may arrive while
	// protected publication replaces the copied per-view evidence collection.
	const auto options = GetRendererEfficiencyOptionsSnapshot();
	TerraEngine::Renderer::RendererEfficiencyFixtureIntent activeIntent;
	{ std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex); activeIntent = capturedRendererEfficiencyFixtureIntent; }
	std::lock_guard lock(statsMutex);
	for (const auto& evidence : latestRendererEfficiencyEvidence)
		if (evidence.viewType == viewType && evidence.viewId == viewId
			&& TerraEngine::Renderer::IsCurrentRendererEfficiencyFixtureEvidence(evidence, options, activeIntent)) return evidence;
	TerraEngine::Renderer::RendererEfficiencyEvidence unavailable;
	unavailable.viewType = viewType;
	unavailable.viewId = viewId;
	unavailable.effectiveOptions = options;
	unavailable.staticCacheBudgetRequestedBytes = options.options.staticShadowCacheBudgetBytes;
	unavailable.staticCacheBudgetEffectiveBytes = unavailable.staticCacheBudgetRequestedBytes;
	unavailable.unavailableReason = "No completed renderer efficiency publication exists for this view.";
	return unavailable;
}

TerraEngine::Renderer::RendererEfficiencyFixtureIntentReceipt RenderSystem::QueueRendererEfficiencyFixtureIntent(
	const TerraEngine::Renderer::RendererEfficiencyFixtureIntent& intent)
{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	TerraEngine::Renderer::RendererEfficiencyFixtureIntentReceipt receipt;
	receipt.correlation = intent.correlation;
	const auto options = rendererEfficiencyOptionsQueue.Snapshot();
	const auto& correlation = intent.correlation;
	if (correlation.operationId.empty() || correlation.fixtureStepId.empty()
		|| correlation.rendererSessionGeneration == 0U
		|| correlation.rendererSessionGeneration != options.rendererSessionGeneration
		|| correlation.settingsGeneration != options.settingsGeneration)
	{ receipt.reason = "Fixture correlation belongs to an unavailable settings/renderer generation."; return receipt; }
	if (intent.viewOrder.size() > 2 || intent.viewOrigins.size() > 2 || intent.cameras.size() > 2 || intent.casterSelections.size() > 2 || (intent.referenceOriginDelta && !intent.viewOrigins.empty()))
	{ receipt.reason = "Fixture view controls exceed bounded outputs or specify ambiguous origins."; return receipt; }
	if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureCameras(intent, receipt.reason)) return receipt;
	if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureCasterSelections(intent, receipt.reason)) return receipt;
	if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureLightingControls(intent, receipt.reason)) return receipt;
	if (!ValidateDepthFixtureIntent(intent,receipt.reason)) return receipt;
#if !defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
	if (intent.injection != TerraEngine::Renderer::RendererEfficiencyFixtureInjection::None)
	{ receipt.reason = "Recorded-submit injection is unavailable until its protected backend control is installed."; return receipt; }
#else
	if (intent.injection > TerraEngine::Renderer::RendererEfficiencyFixtureInjection::ShadowResourceRecreation)
	{ receipt.reason = "Unknown validation-only submit injection."; return receipt; }
#endif
	const auto& activeOperation = capturedRendererEfficiencyFixtureIntent.correlation.operationId;
	if ((!activeOperation.empty() && activeOperation != correlation.operationId)
		|| (pendingRendererEfficiencyFixtureIntent && pendingRendererEfficiencyFixtureIntent->correlation.operationId != correlation.operationId))
	{ receipt.reason = "Another fixture operation already owns the renderer intent."; return receipt; }
	// Ingress copies the whole intent. Only the existing settings/capture boundary
	// publishes it, preserving frame identity while jobs prepare immutable work.
	auto copied = intent;
	const auto& prior = pendingRendererEfficiencyFixtureIntent ? *pendingRendererEfficiencyFixtureIntent : capturedRendererEfficiencyFixtureIntent;
	const bool sameControl = prior.correlation.operationId == correlation.operationId && prior.correlation.fixtureStepId == correlation.fixtureStepId
		&& prior.referenceOriginDelta == intent.referenceOriginDelta && prior.shadowSelection == intent.shadowSelection && prior.injection == intent.injection
		&& prior.viewOrder == intent.viewOrder && prior.viewOrigins == intent.viewOrigins && prior.cameras == intent.cameras
		&& prior.casterSelections == intent.casterSelections && prior.lightingControls == intent.lightingControls
		&& prior.depthControls == intent.depthControls
		&& prior.shadowBatchControls == intent.shadowBatchControls
		&& prior.staticShadowControls == intent.staticShadowControls;
	copied.controlGeneration = sameControl && prior.controlGeneration ? prior.controlGeneration : ++nextRendererEfficiencyFixtureControlGeneration;
	copied.controlsApplied = false; copied.controlReason.clear();
	receipt.controlGeneration = copied.controlGeneration;
	pendingRendererEfficiencyFixtureIntent = std::move(copied);
	receipt.accepted = true;
	return receipt;
}

void RenderSystem::ClearRendererEfficiencyFixtureIntent(std::string_view operationId)
{
	if (operationId.empty()) return;
	bool cleared = false;
	{
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	if (pendingRendererEfficiencyFixtureIntent && pendingRendererEfficiencyFixtureIntent->correlation.operationId == operationId)
	{ pendingRendererEfficiencyFixtureIntent.reset(); cleared = true; }
	if (capturedRendererEfficiencyFixtureIntent.correlation.operationId == operationId)
	{ capturedRendererEfficiencyFixtureIntent = {}; cleared = true; }
	}
	std::lock_guard<std::mutex> lock(statsMutex);
	if (latestRenderFrameSnapshot.efficiencyFixtureIntent.correlation.operationId == operationId)
	{ latestRenderFrameSnapshot.efficiencyFixtureIntent = {}; cleared = true; }
	if (cleared)
	{
		latestRendererEfficiencyFixtureTargets.clear(); latestRendererEfficiencyEvidence.clear();
		TerraEngine::Renderer::ResetRendererEfficiencyFixtureEvidenceBatch(latestRendererEfficiencyEvidenceBatch);
	}
}

TerraEngine::Renderer::RendererEfficiencyFixtureIntent RenderSystem::GetRendererEfficiencyFixtureControlSnapshot() const
{
	TerraEngine::Renderer::RendererEfficiencyFixtureIntent activeIntent;
	{ std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex); activeIntent = capturedRendererEfficiencyFixtureIntent; }
	std::lock_guard<std::mutex> lock(statsMutex);
	auto result = latestRenderFrameSnapshot.efficiencyFixtureIntent;
	if (result.correlation.operationId != activeIntent.correlation.operationId || result.controlGeneration != activeIntent.controlGeneration) return {};
	if (result.controlsApplied && !TerraEngine::Renderer::HasRendererEfficiencyFixtureControlAcknowledgment(latestRendererEfficiencyEvidenceBatch.rows, result))
	{
		result.controlsApplied = false;
		result.controlReason = "Awaiting an actual recorded-command completion receipt for these fixture controls.";
	}
	return result;
}
std::optional<TerraEngine::Renderer::RendererEfficiencyFixtureTargetProof> RenderSystem::GetRendererEfficiencyFixtureTarget(const TerraEngine::UUID& objectId) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	const auto found = std::find_if(latestRendererEfficiencyFixtureTargets.begin(), latestRendererEfficiencyFixtureTargets.end(), [&](const auto& row) { return row.objectId == objectId; });
	return found == latestRendererEfficiencyFixtureTargets.end() ? std::nullopt : std::optional(*found);
}
bool RenderSystem::QueueRendererResourcePublicationInvalidation(const TerraEngine::UUID& assetId, AssetType type,
	std::uint64_t generation, std::string* reason)
{
	if (reason) reason->clear();
	const auto current = ResourceManager::Instance().GetRendererResourcePublicationGeneration();
	if (assetId == TerraEngine::EMPTY_UUID || !generation || generation > current
		|| (type != AssetType::Mesh && type != AssetType::Material && type != AssetType::Texture))
	{ if (reason) *reason = "Actual renderer resource publication identity is unavailable."; return false; }
	std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
	const auto epoch = rendererEfficiencyOptionsQueue.Snapshot().optionsSessionGeneration;
	const auto found = std::find_if(pendingRendererResourceInvalidations.begin(), pendingRendererResourceInvalidations.end(), [&](const auto& row) { return row.assetId == assetId && row.type == type; });
	if (found == pendingRendererResourceInvalidations.end()) pendingRendererResourceInvalidations.push_back({assetId, type, generation, epoch});
	else *found = {assetId, type, generation, epoch};
	return true;
}

TerraEngine::Renderer::RendererEfficiencyEvidenceBatch RenderSystem::GetRendererEfficiencyEvidenceBatch(std::uint64_t afterBatchSequence) const
{
	const auto options = GetRendererEfficiencyOptionsSnapshot();
	TerraEngine::Renderer::RendererEfficiencyFixtureIntent activeIntent;
	{ std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex); activeIntent = capturedRendererEfficiencyFixtureIntent; }
	std::lock_guard<std::mutex> lock(statsMutex);
	if (latestRendererEfficiencyEvidenceBatch.batchSequence <= afterBatchSequence) return {};
	auto result = latestRendererEfficiencyEvidenceBatch;
	std::erase_if(result.rows, [&](const auto& row) { return !TerraEngine::Renderer::IsCurrentRendererEfficiencyFixtureEvidence(row, options, activeIntent); });
	return result;
}

void RenderSystem::ApplyTransientRenderDiagnostics(bool validationEnabled, bool debugEnabled)
{
	const bool validationChanged = !transientRenderValidationOverride.has_value()
		|| transientRenderValidationOverride.value() != validationEnabled;
	const bool debugChanged = !transientRenderDebugDiagnosticsOverride.has_value()
		|| transientRenderDebugDiagnosticsOverride.value() != debugEnabled;
	if (!validationChanged && !debugChanged)
	{
		return;
	}

	// Preferences are user/editor state, so this path deliberately updates only
	// live runtime flags. It does not call SetRenderSettings or SaveSettings,
	// because those APIs own project-authored RenderSettings.json data.
	transientRenderValidationOverride = validationEnabled;
	transientRenderDebugDiagnosticsOverride = debugEnabled;
	ResourceManager::Instance().SetRenderValidationEnabled(validationEnabled);
	ResourceManager::Instance().SetRenderDebugEnabled(debugEnabled);
	ApplyRenderDiagnosticsRequest();

	if (auto* vulkanContext = dynamic_cast<VulkanContext*>(rhi.get()))
	{
		// Existing Vulkan instances cannot gain layers after creation, but
		// retaining the requested value keeps diagnostics accurate without
		// modifying project settings. A restart applies the new layer request.
		vulkanContext->SetValidationLayersRequested(validationEnabled);
	}
}

void RenderSystem::ClearTransientRenderDiagnosticsOverride()
{
	if (!transientRenderValidationOverride.has_value() && !transientRenderDebugDiagnosticsOverride.has_value())
	{
		return;
	}

	transientRenderValidationOverride.reset();
	transientRenderDebugDiagnosticsOverride.reset();
	ResourceManager::Instance().SetRenderValidationEnabled(currentSettings.validationEnabled > 0u);
	ResourceManager::Instance().SetRenderDebugEnabled(currentSettings.debugEnabled > 0u);
	ApplyRenderDiagnosticsRequest();

	if (auto* vulkanContext = dynamic_cast<VulkanContext*>(rhi.get()))
	{
		vulkanContext->SetValidationLayersRequested(currentSettings.validationEnabled > 0u);
	}
}

void RenderSystem::SetVirtualGeometryQualityProfile(const VirtualGeometryQualityProfile& profile)
{
	currentSettings.virtualGeometry = SanitizeVirtualGeometryQualityProfile(profile);
	currentPreset = QualityPreset::Custom;
	SaveSettings();
}

VirtualGeometryQualityProfile RenderSystem::GetVirtualGeometryQualityProfile() const
{
	return SanitizeVirtualGeometryQualityProfile(currentSettings.virtualGeometry);
}

void RenderSystem::SetVirtualGeometryDebugVisualizationMode(RenderViewType viewType, std::uint32_t outputId, VirtualGeometryDebugVisualizationMode mode)
{
	// View-local toolbar state is runtime-only. Normalize here so editor UI,
	// automation, or stale project values cannot publish removed debug modes
	// into the visible-command cache or shader push constants.
	const VirtualGeometryDebugVisualizationMode normalized = NormalizeVirtualGeometryDebugVisualizationMode(mode);
	std::lock_guard<std::mutex> lock(statsMutex);
	virtualGeometryDebugModes[BuildVirtualGeometryDebugModeKey(viewType, outputId)] = normalized;
}

VirtualGeometryDebugVisualizationMode RenderSystem::GetVirtualGeometryDebugVisualizationMode(RenderViewType viewType, std::uint32_t outputId) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	const auto mode = virtualGeometryDebugModes.find(BuildVirtualGeometryDebugModeKey(viewType, outputId));
	if (mode != virtualGeometryDebugModes.end())
	{
		return NormalizeVirtualGeometryDebugVisualizationMode(mode->second);
	}
	return NormalizeVirtualGeometryDebugVisualizationMode(currentSettings.virtualGeometry.debugVisualizationMode);
}

bool RenderSystem::SubmitSceneViewPhysicsDebugPackets(
	const std::vector<std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>>& packets,
	const TerraEngine::Physics::PhysicsDebugDisplaySelection& selection)
{
	// Renderer recovery and headless/editor shutdown paths must not force an RHI
	// allocation merely because physics published a safe diagnostic packet.
	if (rhi == nullptr)
	{
		return false;
	}

	SceneViewPhysicsDebugSubmissionSummary summary;
	summary.rendererAvailable = true;
	summary.packetCount = packets.size();
	for (const std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>& packet : packets)
	{
		if (!packet)
		{
			continue;
		}
		if (selection.showLines && packet->lines) { summary.primitiveCount += packet->lines->size(); }
		if (selection.showTriangles && packet->triangles) { summary.primitiveCount += packet->triangles->size(); }
		if (selection.showContacts && packet->contacts) { summary.primitiveCount += packet->contacts->size(); }
		if (selection.showConstraints && packet->constraints) { summary.primitiveCount += packet->constraints->size(); }
		if (selection.showCharacterSupport && packet->characters) { summary.primitiveCount += packet->characters->size(); }
		if (selection.showRecentQueries && packet->queries) { summary.primitiveCount += packet->queries->size(); }
	}

	std::lock_guard<std::mutex> lock(statsMutex);
	latestSceneViewPhysicsDebugSubmission = summary;
	return true;
}

RenderSystem::SceneViewPhysicsDebugSubmissionSummary RenderSystem::GetLatestSceneViewPhysicsDebugSubmission() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestSceneViewPhysicsDebugSubmission;
}

void RenderSystem::ClearVirtualGeometryDebugVisualizationModes()
{
	std::lock_guard<std::mutex> lock(statsMutex);
	virtualGeometryDebugModes.clear();
}

VirtualGeometryInactiveReason RenderSystem::GetVirtualGeometryInactiveReason() const
{
	const bool meshShaderRouteSupported = rhi != nullptr && rhi->SupportsVirtualGeometryMeshShaderRoute();
	return ResolveVirtualGeometryInactiveReason(currentSettings.virtualGeometry, meshShaderRouteSupported);
}

// RenderSystem::LoadSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::LoadSettings()
{
	namespace fs = std::filesystem;
	fs::path settingsPath = ResolveRuntimeSettingsDirectory() / "RenderSettings.json";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(settingsPath))
	{
		const fs::path legacySettingsPath = ResolveLegacyAssetSettingsPath();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!legacySettingsPath.empty() && fs::exists(legacySettingsPath))
		{
			settingsPath = legacySettingsPath;
		}
		else
		{
			SetQualityPreset(QualityPreset::High);
			return;
		}
	}

	try
	{
		std::ifstream file(settingsPath);
		nlohmann::json j;
		file >> j;

		currentPreset = static_cast<QualityPreset>(j.value("preset", 2u));
		
		auto s = j["settings"];
		currentSettings.shadowResolution = s.value("shadowResolution", 2048u);
		currentSettings.shadowFilteringSize = s.value("shadowFilteringSize", 5u);
		currentSettings.maxUploadedLights = s.value("maxUploadedLights", 256u);
		currentSettings.maxDynamicLights = s.value("maxDynamicLights", 128u);
		currentSettings.maxShadowLights = s.value("maxShadowLights", 8u);
		currentSettings.maxAreaLights = s.value("maxAreaLights", 4u);
		currentSettings.enableLTC = s.value("enableLTC", 1u);
		currentSettings.enableIBL = s.value("enableIBL", 1u);
		currentSettings.indirectStrength = s.value("indirectStrength", 1.0f);
		currentSettings.ambientOcclusionStrength = s.value("ambientOcclusionStrength", 1.0f);
		currentSettings.ambientOcclusionQuality = s.value("ambientOcclusionQuality", 2u);
		currentSettings.anisotropicFiltering = s.value("anisotropicFiltering", 4u);
		currentSettings.variableRateShading = s.value("variableRateShading", 0u);
		// Vulkan validation and render-debug diagnostics are startup-owned
		// profile flags. Keep them beside the render settings file so the editor
		// can expose them without requiring manual JSON edits.
		currentSettings.validationEnabled = j.value("validationEnabled", s.value("validationEnabled", false)) ? 1u : 0u;
		currentSettings.debugEnabled = j.value("debugEnabled", s.value("debugEnabled", false)) ? 1u : 0u;
		if (s.contains("virtualGeometry") && s["virtualGeometry"].is_object())
		{
			const auto& virtualGeometry = s["virtualGeometry"];
			currentSettings.virtualGeometry.lodErrorScale = virtualGeometry.value("lodErrorScale", 1.0f);
			currentSettings.virtualGeometry.maxLodDrop = virtualGeometry.value("maxLodDrop", 0u);
			// Older settings files may omit this field entirely. Missing means
			// "use the guarded engine default"; an explicit false still remains
			// an opt-out for debugging risky scenes.
			currentSettings.virtualGeometry.hiZOcclusionEnabled = virtualGeometry.value("hiZOcclusionEnabled", MakeDefaultVirtualGeometryQualityProfile().hiZOcclusionEnabled);
			currentSettings.virtualGeometry.hiZSelfOcclusionGuardEnabled = virtualGeometry.value("hiZSelfOcclusionGuardEnabled", true);
			currentSettings.virtualGeometry.hiZNearPlaneGuardEnabled = virtualGeometry.value("hiZNearPlaneGuardEnabled", true);
			currentSettings.virtualGeometry.hiZThinGeometryGuardEnabled = virtualGeometry.value("hiZThinGeometryGuardEnabled", true);
			currentSettings.virtualGeometry.debugVisualizationMode = static_cast<VirtualGeometryDebugVisualizationMode>(
				virtualGeometry.value("debugVisualizationMode", static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::None)));
			currentSettings.virtualGeometry.geometryBudgetBytes = virtualGeometry.value("geometryBudgetBytes", static_cast<std::uint64_t>(0u));
			currentSettings.virtualGeometry.deformationCurrentBudgetBytes =
				virtualGeometry.value(
					"deformationCurrentBudgetBytes",
					static_cast<std::uint64_t>(0u));
			currentSettings.virtualGeometry.deformationHistoryBudgetBytes =
				virtualGeometry.value(
					"deformationHistoryBudgetBytes",
					static_cast<std::uint64_t>(0u));
			currentSettings.virtualGeometry.deformationBoundsBudgetBytes =
				virtualGeometry.value(
					"deformationBoundsBudgetBytes",
					static_cast<std::uint64_t>(0u));
			currentSettings.virtualGeometry.preloadBias = virtualGeometry.value("preloadBias", 1.0f);
			currentSettings.virtualGeometry.lodTransitionHysteresisPixels = virtualGeometry.value("lodTransitionHysteresisPixels", 0.35f);
			currentSettings.virtualGeometry.lodTransitionStableFrames = virtualGeometry.value("lodTransitionStableFrames", 3u);
		}
		currentSettings = SanitizeRenderSettings(currentSettings);
		if (!transientRenderValidationOverride.has_value())
		{
			ResourceManager::Instance().SetRenderValidationEnabled(currentSettings.validationEnabled > 0u);
		}
		ResourceManager::Instance().SetRenderDebugEnabled(ResolveRenderDebugDiagnosticsEnabled());
		ApplyRenderDiagnosticsRequest();
	}
	catch (const std::exception& e)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to load RenderSettings: " << e.what());
		SetQualityPreset(QualityPreset::High);
	}
}

// RenderSystem::SaveSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SaveSettings()
{
	namespace fs = std::filesystem;
	const fs::path settingsDir = ResolveRuntimeSettingsDirectory();
	const fs::path settingsPath = settingsDir / "RenderSettings.json";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(settingsDir))
	{
		fs::create_directories(settingsDir);
	}

	try
	{
		nlohmann::json j;
		j["preset"] = static_cast<uint32_t>(currentPreset);
		j["validationEnabled"] = currentSettings.validationEnabled > 0u;
		j["debugEnabled"] = currentSettings.debugEnabled > 0u;
		
		nlohmann::json s;
		s["shadowResolution"] = currentSettings.shadowResolution;
		s["shadowFilteringSize"] = currentSettings.shadowFilteringSize;
		s["maxUploadedLights"] = currentSettings.maxUploadedLights;
		s["maxDynamicLights"] = currentSettings.maxDynamicLights;
		s["maxShadowLights"] = currentSettings.maxShadowLights;
		s["maxAreaLights"] = currentSettings.maxAreaLights;
		s["enableLTC"] = currentSettings.enableLTC;
		s["enableIBL"] = currentSettings.enableIBL;
		s["indirectStrength"] = currentSettings.indirectStrength;
		s["ambientOcclusionStrength"] = currentSettings.ambientOcclusionStrength;
		s["ambientOcclusionQuality"] = currentSettings.ambientOcclusionQuality;
		s["anisotropicFiltering"] = currentSettings.anisotropicFiltering;
		s["variableRateShading"] = currentSettings.variableRateShading;
		nlohmann::json virtualGeometry;
		const VirtualGeometryQualityProfile virtualGeometryProfile = SanitizeVirtualGeometryQualityProfile(currentSettings.virtualGeometry);
		virtualGeometry["lodErrorScale"] = virtualGeometryProfile.lodErrorScale;
		virtualGeometry["maxLodDrop"] = virtualGeometryProfile.maxLodDrop;
		virtualGeometry["hiZOcclusionEnabled"] = virtualGeometryProfile.hiZOcclusionEnabled;
		virtualGeometry["hiZSelfOcclusionGuardEnabled"] = virtualGeometryProfile.hiZSelfOcclusionGuardEnabled;
		virtualGeometry["hiZNearPlaneGuardEnabled"] = virtualGeometryProfile.hiZNearPlaneGuardEnabled;
		virtualGeometry["hiZThinGeometryGuardEnabled"] = virtualGeometryProfile.hiZThinGeometryGuardEnabled;
		virtualGeometry["debugVisualizationMode"] = static_cast<std::uint32_t>(virtualGeometryProfile.debugVisualizationMode);
		virtualGeometry["geometryBudgetBytes"] = virtualGeometryProfile.geometryBudgetBytes;
		virtualGeometry["deformationCurrentBudgetBytes"] =
			virtualGeometryProfile.deformationCurrentBudgetBytes;
		virtualGeometry["deformationHistoryBudgetBytes"] =
			virtualGeometryProfile.deformationHistoryBudgetBytes;
		virtualGeometry["deformationBoundsBudgetBytes"] =
			virtualGeometryProfile.deformationBoundsBudgetBytes;
		virtualGeometry["preloadBias"] = virtualGeometryProfile.preloadBias;
		virtualGeometry["lodTransitionHysteresisPixels"] = virtualGeometryProfile.lodTransitionHysteresisPixels;
		virtualGeometry["lodTransitionStableFrames"] = virtualGeometryProfile.lodTransitionStableFrames;
		s["virtualGeometry"] = virtualGeometry;
		
		j["settings"] = s;

		std::ofstream file(settingsPath);
		file << j.dump(4);
	}
	catch (const std::exception& e)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to save RenderSettings: " << e.what());
	}
}
