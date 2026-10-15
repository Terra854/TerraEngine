// Validation-only native renderer scratch contract.
#pragma once

#include "Renderer/Graph/RenderFrameScratch.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Utilities/UUID.h"
#include <functional>
#include <bit>
#include <memory>
#include <vector>

namespace TerraEngine::Renderer {
	// Canonical test-only digest: append low byte first, avoiding padding, host
	// endianness, implementation-defined hash functions, and floating conversion.
	inline void AppendScratchValidationWord(std::uint64_t& digest, std::uint64_t word, std::size_t bytes) noexcept {
		for(std::size_t byte = 0U; byte < bytes; ++byte) {
			digest ^= (word >> (byte * 8U)) & 0xffU;
			digest *= 1099511628211ULL;
		}
	}
	inline void AppendScratchLightValidationDigest(std::uint64_t& digest,
		const TerraEngine::UUID& id, const LightComponent& light) noexcept {
		for(const auto byte : id) AppendScratchValidationWord(digest, byte, 1U);
		AppendScratchValidationWord(digest, static_cast<std::uint32_t>(light.type), 4U);
		AppendScratchValidationWord(digest, static_cast<std::uint32_t>(light.unit), 4U);
		AppendScratchValidationWord(digest, static_cast<std::uint32_t>(light.appearance), 4U);
		const auto appendFloat = [&](float value) { AppendScratchValidationWord(digest, std::bit_cast<std::uint32_t>(value), 4U); };
		appendFloat(light.color.x); appendFloat(light.color.y); appendFloat(light.color.z);
		appendFloat(light.temperature); appendFloat(light.intensity); appendFloat(light.range);
		appendFloat(light.innerConeAngle); appendFloat(light.outerConeAngle);
		appendFloat(light.width); appendFloat(light.height);
		AppendScratchValidationWord(digest, light.castsShadows ? 1U : 0U, 1U);
		appendFloat(light.shadowStrength); appendFloat(light.shadowBias);
		appendFloat(light.indirectMultiplier); appendFloat(light.authoredPriority);
		appendFloat(light.stabilityPreference);
	}
	// These are owned copies of the actual protected collection, in its order.
	// They contain no PMR allocator, arena pointer, or live component reference.
	struct RenderFrameScratchCameraInput {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		CameraComponent camera;
	};
	struct RenderFrameScratchLightInput {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		LightComponent light;
	};
	struct RenderFrameScratchCapturedInputs {
		std::vector<RenderFrameScratchCameraInput> cameras;
		std::vector<RenderFrameScratchLightInput> lights;
	};
	enum class RenderFrameScratchValidationCheckpoint {
		Captured, LightReadBegin, LightReadEnd
	};
	struct RenderFrameScratchValidationEvent {
		RenderFrameScratchValidationCheckpoint checkpoint{};
		RenderFrameScratchDiagnostics episode;
		std::size_t start = 0U;
		std::size_t end = 0U;
		std::size_t workerLimit = 0U;
		// Both events hash the actual source range with the same algorithm. End
		// also folds the production loop's computed signatures separately.
		std::uint64_t rangeDigest = 0U;
		std::uint64_t computedSignatureDigest = 0U;
		std::shared_ptr<const RenderFrameScratchCapturedInputs> capturedInputs;
	};
	struct RenderFrameScratchValidationCallbacks {
		// Configure only while quiescent. Implementations must synchronize their
		// own observations: disjoint real worker chunks can call concurrently.
		std::function<void(const RenderFrameScratchValidationEvent&)> onEvent;
	};
}
