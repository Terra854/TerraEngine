// Pure worker-safe preparation from retained, immutable motion/history inputs.
#pragma once
#include "Core/Export.h"
#include "Renderer/RHI/Vulkan/VulkanPostProcessHistory.h"
#include "Utilities/UUID.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct Mesh;
namespace TerraEngine::Renderer { struct CpuAnimationDeformationData; }
namespace TerraEngine::Renderer::Vulkan
{
	// Match MotionVectors.hlsli exactly. CPU readiness never qualifies native
	// descriptors: selectors, shader owner flags and view flags stay invalid until
	// the protected uploader proves its own retained mesh/palette allocations.
	struct alignas(16) PostProcessMotionViewData
	{
		glm::mat4 currentRasterViewProjection{1.0f},previousRasterViewProjection{1.0f};
		glm::vec4 jitterUV{0.0f};
		glm::uvec4 extents{0u},recordingTokens{0u},state{0u};
	};
	struct alignas(16) PostProcessMotionDrawData
	{
		glm::mat4 currentModel{1.0f},previousModel{1.0f};
		glm::uvec4 deformationSelectors{0xffffffffu},deformationState{0u};
	};
	struct alignas(16) PostProcessMotionCameraOnlyData { glm::mat4 currentModelInPreviousReference{1.0f}; };
	enum class PostProcessMotionReason : std::uint8_t
	{
		None,MissingPrevious,IncompatibleHistory,UnresolvedDraw,DuplicateIdentity,
		EntityChanged,MeshChanged,TopologyChanged,MissingCanonicalPose,UnsafeReference,
		NonFiniteMatrix,UnsupportedGeometry,UnsupportedDeformation,MissingCpuPose,ChangedCpuPoseLayout
	};
	struct PostProcessMotionDrawIdentity
	{
		std::uint32_t ordinal=0xffffffffu;
		std::uint64_t lifetimeGeneration=0;
		UUID objectId=EMPTY_UUID,meshId=EMPTY_UUID;
		std::uint64_t meshGeneration=0,entityLifetimeGeneration=0;
		std::uint32_t selectedLod=0,firstMeshlet=0,meshletCount=0;
		bool operator==(const PostProcessMotionDrawIdentity&) const = default;
	};
	struct PostProcessMotionCpuPoseOwner
	{
		std::shared_ptr<const CpuAnimationDeformationData> data;
		std::uint64_t poseGeneration=0,payloadGeneration=0,skeletonFingerprint=0,authorityGeneration=0;
		std::uint64_t frameId=0,rendererGeneration=0,stableInstanceId=0;
		std::uint32_t recordIndex=0xffffffffu;
	};
	struct PreparedPostProcessMotionDraw
	{
		std::uint32_t selectionIndex=0;
		PostProcessMotionDrawIdentity identity;
		PostProcessMotionReason reason=PostProcessMotionReason::MissingPrevious;
		PostProcessMotionDrawData shader;
		PostProcessMotionCameraOnlyData cameraOnly;
		PostProcessMotionCpuPoseOwner currentPose,previousPose;
		bool currentGeometryValid=false,cpuPairValid=false,cameraOnlyAvailable=false;
	};
	using PostProcessMotionMeshOwners = std::unordered_map<UUID, std::shared_ptr<const Mesh>>;
	struct PreparedPostProcessMotion
	{
		std::shared_ptr<const PostProcessHistoryState> current,previous;
		std::shared_ptr<const PostProcessMotionMeshOwners> meshes;
		PostProcessMotionViewData shader;
		PostProcessMotionReason historyReason=PostProcessMotionReason::MissingPrevious;
		bool cpuHistoryCompatible=false;
		std::vector<PreparedPostProcessMotionDraw> draws;
	};
	// Inputs come from PostProcessHistory::Record, whose previous root represents
	// the actual prior accepted view. Preparation runs on copied roots in a worker;
	// native admission/upload remains at the existing protected renderer boundary.
	// Return false for invalid current provenance or allocation failure, preserving
	// output. A valid first frame returns rows marked MissingPrevious, never motion
	// inferred from RenderDynamicLane::previousModelMatrix or producer tick order.
	[[nodiscard]] TERRAENGINE_API bool TryPreparePostProcessMotion(
		const PostProcessHistoryRecording& recording,glm::vec2 currentJitterUV,glm::vec2 previousJitterUV,
		bool includeCameraOnly,PreparedPostProcessMotion& output,std::string& reason) noexcept;
}
