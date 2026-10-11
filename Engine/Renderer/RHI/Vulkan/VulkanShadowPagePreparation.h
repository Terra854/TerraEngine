// VulkanShadowPagePreparation.h — complete authored shadow-page coverage.
#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Mesh;

namespace TerraEngine::Renderer { struct ShadowCasterStamp; }

namespace TerraEngine::Renderer::Vulkan
{
	struct ShadowPageCutRequest
	{
		UUID meshId = EMPTY_UUID;
		std::shared_ptr<const Mesh> mesh;
		std::uint64_t cpuPublicationGeneration = 0U;
		std::uint32_t ownerMeshletOffset = 0U, ownerMeshletCount = 0U, selectedLod = 0U;
		bool deformationUnsupported = false, materialUnsupported = false;
	};

	struct ShadowAuthoredPageSegment
	{
		std::uint32_t pageId = 0U, fallbackPageId = 0U, clusterId = 0U;
		std::uint32_t ownerMeshletOffset = 0U, ownerMeshletCount = 0U, selectedLod = 0U;
		bool operator==(const ShadowAuthoredPageSegment&) const = default;
	};

	struct ShadowPageCut
	{
		bool complete = false;
		std::vector<ShadowAuthoredPageSegment> segments;
		std::string fallbackReason;
	};

	// This source names the actual copied authored stream consumed by task/mesh
	// shaders. It never claims a persistent streamed-page residency receipt.
	enum class ShadowPagePayloadSource : std::uint8_t { AuthoredShadowLocal };
	struct PreparedShadowPageBinding
	{
		ShadowAuthoredPageSegment segment;
		std::uint32_t descriptorIndex = 0xffffffffU, payloadBufferIndex = 0xffffffffU;
		std::uint64_t cpuPublicationGeneration = 0U, payloadAllocationGeneration = 0U;
		// All segments share the complete native word stream, including its ABI
		// header and descriptors. Offset zero and actual buffer size bound those reads.
		std::uint64_t payloadByteOffset = 0U, payloadByteSize = 0U;
		ShadowPagePayloadSource source = ShadowPagePayloadSource::AuthoredShadowLocal;
	};
	struct PreparedShadowPageSet
	{
		bool complete = false;
		std::vector<PreparedShadowPageBinding> bindings;
		std::string fallbackReason;
	};

	// Validate each binding on a copied owner stamp before suppressing the owner
	// draw. Failure leaves the input unchanged so the caller keeps whole coverage.
	TERRAENGINE_API bool ApplyShadowAuthoredPageBinding(const PreparedShadowPageBinding& binding, ShadowCasterStamp& stamp) noexcept;

	// Worker-safe: the request retains all immutable metadata. No camera packet,
	// scene object, ResourceManager, or Vulkan state can influence this cut.
	TERRAENGINE_API ShadowPageCut BuildShadowAuthoredPageCut(const ShadowPageCutRequest& request);
}
