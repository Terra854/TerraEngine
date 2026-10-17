#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureCameras.h"
// Narrow borrowed animation identity inputs, owned by protected coroutine roots.
#pragma once
#include "Renderer/RenderTypes.h"
#include "Renderer/RenderSystem.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/Gpu/GpuAnimationPlan.h"
#include <memory>
#include <optional>
#include "Scene/SceneChangePacket.h"
#include "Scene/SpatialBounds.h"
#include "Components/SkyComponent.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Resource/VirtualGeometryStreamingPolicy.h"
#include <unordered_map>
#include <unordered_set>

struct Mesh;
struct Material;
class ResourceManager;

namespace TerraEngine::Renderer::Frame {
	// These strong-pointer references borrow the caller's captured owners. This
	// view is consumed synchronously during protected renderable collection only.
	struct RenderAnimationCaptureView {
		const std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan>& immutableGpuAnimationPlan;
		const std::shared_ptr<const TerraEngine::Animation::PublishedGpuAnimationPresentationPlan>& gpuPresentation;
		std::uint64_t protectedRendererGeneration;
		std::uint64_t protectedSceneGeneration;
	};
	std::optional<AnimationGpuRenderBinding> FindUniqueGpuAnimationIdentity(
		const RenderAnimationCaptureView& inputs, const AnimationRenderBindingRequest& request,
		std::size_t& ownerMappingHint);
	std::uint64_t BuildStableAnimationInstanceId(
		const TerraEngine::Renderer::AnimationRenderBinding& binding,
		const TerraEngine::UUID& objectId);
	// Immutable animation/mesh compatibility checks shared by protected collection.
	TerraEngine::Renderer::AnimationRenderBindingStatus ValidateAnimatorPosePublication(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request,
		const TerraEngine::Animation::AnimationAuthorityRow& authorityRow) noexcept;
	bool HeldAnimationBindingMatchesRequest(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request) noexcept;
	bool GpuVisualIdentityMatchesRequest(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request,
		const TerraEngine::Renderer::AnimationGpuRenderBinding& identity) noexcept;
	bool IsMorphOnlyIdentityDeformableMesh(const Mesh& mesh) noexcept;
	TerraEngine::Animation::AnimationCompatibilityFingerprint
	ResolveDeformableSkeletonFingerprint(
		const Mesh& mesh,
		const TerraEngine::Animation::AnimationAuthorityRow* authorityRow) noexcept;
	bool IsValidatedAnimationDeformableMesh(const Mesh& mesh) noexcept;
	// Synchronous protected collectors borrow these narrow tables/status values;
	// retained renderer maps own their immutable mesh and material payloads.
	struct RenderResourceCaptureView {
		std::unordered_map<TerraEngine::UUID, bool>& frameResourceAvailability;
		std::unordered_map<TerraEngine::UUID, const Mesh*>& frameMeshResources;
		std::unordered_map<TerraEngine::UUID, const Material*>& frameMaterialResources;
		std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::SpatialAABB>& frameMeshLocalBounds;
		ResourceManager& resourceManager;
		bool& renderableResourcePublicationsComplete;
		std::size_t& runtimeLayoutRejectedRecordCount;
		std::vector<std::string>& runtimeLayoutRejectionReasons;
	};
	struct RenderAnimationCollectionView {
		const RenderAnimationCaptureView& animationCapture;
		const std::shared_ptr<const TerraEngine::Animation::AnimationPresentationFrame>& animationPresentationFrame;
		std::optional<std::uint64_t>& animationAuthorityGeneration;
		std::unordered_set<TerraEngine::UUID>& aliveCompatibleAnimationObjects;
		bool& hasCpuDeformationCandidates;
	};
	struct RenderMembershipCaptureView {
		const std::shared_ptr<const TerraEngine::Scene::SceneChangePacket>& protectedSceneChanges;
		std::uint64_t protectedSceneGeneration, protectedTerrainProxyGeneration;
		bool reuseProtectedRenderableCapture, renderableMetadataCaptureCurrent, hasQueuedDedicatedCapture;
	};

}

// Private domain owner: all aliases exposed by capture remain backed by these
// strong immutable roots until the coroutine finishes or is safely retired.
struct RenderSystem::FrameCaptureRoots {
	std::uint64_t protectedRendererGeneration = 0U;
	std::uint64_t protectedBackendResourceGeneration = 0U;
	std::shared_ptr<const TerraEngine::Scene::SceneChangePacket> protectedSceneChanges;
	std::uint64_t protectedSceneGeneration = 0U;
	std::shared_ptr<const TerraEngine::Animation::AnimationPresentationFrame> animationPresentationFrame;
	std::shared_ptr<const TerraEngine::Animation::PublishedGpuAnimationPresentationPlan> gpuPresentation;
	std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutableGpuAnimationPlan;
	std::size_t denseAnimationOwnerCount = 0U;
	bool hasDenseAnimationTopologyCandidate = false;
};

// Scalar scene revisions are captured once; worker helpers never reopen ECS.
struct RenderSystem::FrameSceneRevisions {
	std::uint64_t capturedSceneGeneration = 0U;
	std::uint64_t capturedSpatialRevision = 0U;
	std::uint64_t capturedComponentMutationRevision = 0U;
	std::uint64_t capturedMeshComponentRevision = 0U;
	std::uint64_t capturedTerrainComponentRevision = 0U;
	std::uint64_t capturedTerrainProxyGeneration = 0U;
	std::uint64_t capturedRenderBindingRevision = 0U;
	std::uint64_t capturedHierarchyRevision = 0U;
	std::uint64_t capturedActiveMembershipRevision = 0U;
	std::uint64_t capturedAnimatorComponentRevision = 0U;
};

// Shader owners close the same loader cohort as retained mesh/material tables.
struct ShaderCollection; // Match the resource definition tag used in MSVC ABI names.
struct RenderSystem::FrameResourceCohort {
	TerraEngine::UUID capturedDefaultShaderCollectionId{}, capturedDefaultTaskShaderCollectionId{};
	std::shared_ptr<const ShaderCollection> capturedShadowMeshCollection, capturedShadowTaskCollection;
	std::uint64_t capturedRendererResourceRevision = 0U;
	bool rendererResourceOwnerCohortStable = false;
	std::uint64_t capturedAnimationBindingResourceRevision = 0U;
	std::uint64_t capturedVirtualGeometryResidencyGeneration = 0U;
	RenderSettings capturedRenderSettings;
	TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot capturedEfficiencyOptions;
};

// Immutable backend capabilities and dedicated-output intent captured before workers.
struct RenderSystem::FrameBackendCapture {
	TerraEngine::Renderer::BackendCapabilityProfile backendCapabilities;
	bool hardwareVirtualGeometryRouteAvailable = false;
	bool computeVirtualGeometryRouteAvailable = false;
	bool deviceAuthoredVirtualGeometryAvailable = false;
	std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute> validationRouteOverride;
	bool hybridValidationSceneRequested = false;
	bool hasPendingDedicatedCaptureRender = false;
	TerraEngine::Renderer::RenderCaptureViewIdentity pendingDedicatedCaptureIdentity;
	TerraEngine::Scene::WorldPosition pendingDedicatedCaptureCanonicalOrigin;
	glm::uvec2 pendingDedicatedCaptureTargetSize{};
};

// Owned sky component copy; selection and asset lookup remain protected work.
struct RenderSystem::FrameSkyCapture {
	std::optional<SkyComponent> capturedSkyComponent;
	TerraEngine::UUID capturedSkyObjectId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID capturedSkyTextureId = TerraEngine::EMPTY_UUID;
};
// Completed backend observations are immutable values throughout preparation.
struct RenderSystem::FrameBackendObservations {
	std::vector<TerraEngine::Renderer::VirtualGeometryAuthoritativeCounterReadback> completedVirtualGeometryCounters;
	TerraEngine::Renderer::BackendFrameBreakdown capturedBackendBreakdown;
	TerraEngine::Renderer::RendererRecoverySnapshot capturedRecoverySnapshot;
	UIRenderBatch capturedUiBatch;
	TerraEngine::Resource::VirtualGeometryStreamingPlan capturedVirtualGeometryStreamingPlan;
};

// Owned per-frame terrain values consumed by worker visibility and streaming work.
struct RenderSystem::FrameTerrainCapture {
	std::unordered_map<TerraEngine::UUID, TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeBinding> frameTerrainMaterialBindings;
	std::unordered_map<TerraEngine::UUID, std::pair<TerraEngine::UUID, TerraEngine::UUID>> frameTerrainObjects;
};

// Borrowed renderer cache values remain stable under the existing execution gate.
// Lifecycle retirement destroys the continuation before replacing this cache owner.
struct RenderSystem::FrameWorldTransforms {
	const std::unordered_map<entt::entity, TerraEngine::Scene::WorldPose>* worldTransforms = nullptr;
	const TerraEngine::Scene::SceneTransformPropagationResult* transformPropagation = nullptr;
	TerraEngine::Scene::WorldPose fallbackWorldTransform;
	const TerraEngine::Scene::WorldPose& Get(entt::entity entity) const;
};

// Collection status owns only copied validation/status values and sparse CPU-pose retirement IDs.
struct RenderSystem::FrameRenderableCaptureState {
	std::uint64_t protectedRendererResourceOwnerRevision, protectedTerrainProxyGeneration, renderableCaptureSourceSignature;
	bool hasQueuedDedicatedCapture, hasCpuDeformationCandidates, renderableResourcePublicationsComplete;
	std::size_t runtimeLayoutRejectedRecordCount;
	std::vector<std::string> runtimeLayoutRejectionReasons;
	TerraEngine::Renderer::RenderFrameScratchDiagnostics capturedScratchDiagnostics;
	std::unordered_set<TerraEngine::UUID> aliveCompatibleAnimationObjects;
};

// Snapshot reuse owns one frame table and only a tiny copy of prior qualified view origins.
struct RenderSystem::FrameSnapshotCapture {
	TerraEngine::Renderer::RenderFrameSnapshot renderFrameSnapshot;
	std::vector<TerraEngine::Renderer::RenderFrameViewRequest> previousViewRequests;
	bool reuseRenderableRecords;
};

// Optional validation output owns immutable ranges/camera descriptors without reopening ECS on workers.
struct RenderSystem::FrameFixtureCapture {
	std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureTargetProof> fixtureTargets;
	std::vector<TerraEngine::Renderer::RendererEfficiencyResolvedFixtureCamera> sealedFixtureCameras;
};
