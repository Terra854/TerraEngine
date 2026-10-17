// Exact typed signatures shared by captured-input and preparation domains.
#pragma once
#include "Renderer/RenderTypes.h"
#include "Renderer/VirtualGeometry/Diagnostics/VirtualGeometryReplay.h"
class RHIContext;
#include <cstdint>
namespace TerraEngine::Renderer::Frame {
	inline constexpr std::uint64_t kRenderHashOffset = 1469598103934665603ull;
	inline constexpr std::uint64_t kRenderHashPrime = 1099511628211ull;
	std::uint64_t HashFloat(float value);
	void HashCombine(std::uint64_t& seed, std::uint64_t value);
	void HashVec3(std::uint64_t& seed, const glm::vec3& value);
	void HashVec4(std::uint64_t& seed, const glm::vec4& value);
	void HashMat4(std::uint64_t& seed, const glm::mat4& value);
	void HashUVec2(std::uint64_t& seed, const glm::uvec2& value);
	void HashQuat(std::uint64_t& seed, const glm::quat& value);
	std::uint64_t BuildRenderSettingsSignature(const RenderSettings& settings, QualityPreset preset, bool taskShaderCullingEnabled);
	RenderSettings SanitizeRenderSettings(const RenderSettings& input);
	// Poll actual completed backend evidence at the same protected boundary.
	VirtualGeometryCapabilityDiagnosticsSnapshot CollectVirtualGeometryCapabilityDiagnostics(
		RHIContext& context, const BackendCapabilityProfile& profile,
		const VirtualGeometryFrameViewIdentity& currentPublication);

	glm::mat4 MakeFrameModelMatrix(const TerraEngine::Scene::WorldPose& pose,
		const TerraEngine::Scene::WorldPosition& referenceOrigin);

}
