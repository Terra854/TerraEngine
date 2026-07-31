// VirtualGeometryTypes.h - Backend-neutral virtual geometry identities.
//
// These compact value records travel between cooker, resource publication,
// renderer workers, and backend submission without holding live API handles.
// Keeping every relevant generation in one copied identity prevents an older
// worker result from being mistaken for the current asset or page publication.

#pragma once

#include "Utilities/UUID.h"

#include <compare>
#include <cstdint>

namespace TerraEngine::Renderer
{
	struct VirtualGeometryAssetIdentity
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t sourceHash = 0u;
		std::uint64_t buildRecipeHash = 0u;

		[[nodiscard]] bool IsComplete() const noexcept { return assetId != TerraEngine::EMPTY_UUID && sourceHash != 0u && buildRecipeHash != 0u; }

		auto operator<=>(const VirtualGeometryAssetIdentity &) const = default;
	};

	// Each field names an independently published resource family. Resource
	// publication compares all five values before exposing a page to rendering.
	struct VirtualGeometryGenerationIdentity
	{
		std::uint64_t geometryFormatGeneration = 0u;
		std::uint64_t hierarchyGeneration = 0u;
		std::uint64_t pageDirectoryGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		std::uint64_t descriptorGeneration = 0u;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return geometryFormatGeneration != 0u && hierarchyGeneration != 0u && pageDirectoryGeneration != 0u && residencyGeneration != 0u &&
				   descriptorGeneration != 0u;
		}

		auto operator<=>(const VirtualGeometryGenerationIdentity &) const = default;
	};

	// Page zero is a valid fallback page, so completeness intentionally depends
	// on its generation rather than treating a numeric page ID of zero as empty.
	struct VirtualGeometryPageIdentity
	{
		VirtualGeometryAssetIdentity asset{};
		VirtualGeometryGenerationIdentity generations{};
		std::uint32_t pageId = 0u;
		std::uint64_t pageContentGeneration = 0u;

		[[nodiscard]] bool IsComplete() const noexcept { return asset.IsComplete() && generations.IsComplete() && pageContentGeneration != 0u; }

		auto operator<=>(const VirtualGeometryPageIdentity &) const = default;
	};

	// This identity pins a worker or backend observation to the frame and view
	// that produced it. A diagnostic with a different generation is stale data.
	struct VirtualGeometryFrameViewIdentity
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		std::uint64_t rendererGeneration = 0u;

		[[nodiscard]] bool IsComplete() const noexcept { return frameId != 0u && rendererGeneration != 0u; }

		auto operator<=>(const VirtualGeometryFrameViewIdentity &) const = default;
	};

	// Packet IDs are only unique within the immutable frame/view and descriptor
	// generation that built them, so all of those values stay together.
	struct VirtualGeometryPacketIdentity
	{
		VirtualGeometryFrameViewIdentity frameView{};
		std::uint32_t packetId = 0u;
		std::uint64_t descriptorGeneration = 0u;

		[[nodiscard]] bool IsComplete() const noexcept { return frameView.IsComplete() && descriptorGeneration != 0u; }

		auto operator<=>(const VirtualGeometryPacketIdentity &) const = default;
	};

	enum class VirtualGeometryVisibilityResult : std::uint8_t
	{
		Visible = 0u,
		CulledByFrustum = 1u,
		CulledByOcclusion = 2u,
		Fallback = 3u,
		Invalid = 4u
	};

	// A visibility decision references typed packet/page identities rather than
	// raw buffer offsets, allowing every backend to validate the same outcome.
	struct VirtualGeometryVisibilityDecision
	{
		VirtualGeometryPacketIdentity packet{};
		VirtualGeometryPageIdentity page{};
		VirtualGeometryVisibilityResult result = VirtualGeometryVisibilityResult::Invalid;
		bool failOpen = false;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return packet.IsComplete() && page.IsComplete() && result != VirtualGeometryVisibilityResult::Invalid;
		}
	};
} // namespace TerraEngine::Renderer
