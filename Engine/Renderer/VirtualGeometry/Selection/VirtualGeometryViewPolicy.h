#pragma once
#include <cstdint>
namespace TerraEngine::Renderer
{
	enum class VirtualGeometryViewKind : std::uint8_t
	{
		Camera,
		Shadow,
		Capture,
		Reflection,
		Editor
	};
	enum class VirtualGeometryOcclusionPass : std::uint8_t
	{
		Main,
		Post
	};
	struct VirtualGeometryViewPolicyState
	{
		VirtualGeometryViewKind kind = VirtualGeometryViewKind::Camera;
		std::uint64_t viewId = 0, targetGeneration = 0, projectionGeneration = 0, geometryGeneration = 0, residencyGeneration = 0;
		std::uint64_t historyOwnerViewId = 0;
		bool cameraCut = false, resized = false, teleported = false;
	};
	enum class VirtualGeometryHistoryInvalidation : std::uint8_t { None, CameraCut, Resize, Teleport, ProjectionChanged, OwnerChanged, GenerationChanged };
	struct VirtualGeometryViewPolicyDecision
	{
		bool sharesGeometry = false, historyValid = false;
		// A compatible view may reuse immutable geometry while still owning a
		// separate depth-history lifetime. This avoids letting a shadow, capture,
		// or reflection borrow a camera pyramid whose visibility assumptions differ.
		bool ownsHistory = false;
		VirtualGeometryOcclusionPass pass = VirtualGeometryOcclusionPass::Main;
		VirtualGeometryHistoryInvalidation invalidation = VirtualGeometryHistoryInvalidation::None;
		std::uint64_t historyOwnerViewId = 0;
	};
	[[nodiscard]] VirtualGeometryViewPolicyDecision EvaluateVirtualGeometryViewPolicy(
		const VirtualGeometryViewPolicyState &, const VirtualGeometryViewPolicyState &);
} // namespace TerraEngine::Renderer
