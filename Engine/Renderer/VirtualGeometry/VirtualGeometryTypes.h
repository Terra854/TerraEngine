// VirtualGeometryTypes.h - Backend-neutral virtual geometry identities.
//
// These compact value records travel between cooker, resource publication,
// renderer workers, and backend submission without holding live API handles.
// Keeping every relevant generation in one copied identity prevents an older
// worker result from being mistaken for the current asset or page publication.

#pragma once

#include "Utilities/UUID.h"

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

	// GeometryRoute identifies the representation family selected for an
	// immutable frame snapshot. It deliberately stays separate from the Vulkan
	// raster route because a direct meshlet is not an indexed VG fallback.
	enum class GeometryRoute : std::uint8_t
	{
		DirectMeshlet = 0u,
		VirtualGeometryHardware = 1u,
		VirtualGeometryCompute = 2u,
		VirtualGeometryFallback = 3u
	};

	enum class GeometryRouteSelectionSource : std::uint8_t
	{
		Automatic = 0u,
		AuthorOverride = 1u,
		CapabilityFallback = 2u,
		CorrectnessFallback = 3u,
		PerformanceBypass = 4u
	};

	enum class GeometryRouteFallbackReason : std::uint8_t
	{
		None = 0u,
		CapabilityUnavailable = 1u,
		AuthorOverrideUnavailable = 2u,
		CorrectnessUnavailable = 3u,
		RepresentationUnavailable = 4u,
		StaleGeneration = 5u
	};

	enum class GeometryViewKind : std::uint8_t
	{
		Camera = 0u,
		Shadow = 1u,
		Capture = 2u,
		Picking = 3u,
		Editor = 4u
	};

	enum class GeometryClass : std::uint8_t
	{
		Static = 0u,
		Skeletal = 1u,
		Morph = 2u,
		Spline = 3u,
		Curve = 4u,
		Displacement = 5u,
		Tessellation = 6u,
		Foliage = 7u,
		Terrain = 8u,
		Assembly = 9u,
		DistantRepresentation = 10u,
		Distant = DistantRepresentation
	};

	enum class GeometryClassSupportState : std::uint8_t
	{
		Prototype = 0u,
		ValidatedCandidate = 1u,
		ProductionSupported = 2u,
		Unavailable = 3u
	};

	enum class GeometryGenerationState : std::uint8_t
	{
		Prepared = 0u,
		Validated = 1u,
		Published = 2u,
		Retiring = 3u,
		Retired = 4u,
		Rejected = 5u
	};

	struct GeometryRouteInputSummary
	{
		std::uint32_t sourceTriangleCount = 0u;
		std::uint32_t packedMeshletCount = 0u;
		std::uint64_t baseStoredBytes = 0u;
		std::uint64_t baseDecodedBytes = 0u;
		std::uint64_t materialCapabilityMask = 0u;
		std::uint64_t deformationCapabilityMask = 0u;
		std::uint64_t viewCapabilityMask = 0u;
		std::uint64_t pickingCapabilityMask = 0u;
		std::uint64_t availableRepresentationMask = 0u;
		std::uint32_t directBatchLimit = 0u;
		bool repeatedInstanceStorageCompatible = false;
		bool directBatchEligible = false;
		std::optional<double> predictedHierarchyReduction;
		std::optional<double> predictedVirtualGeometryStorageBenefit;
		std::uint64_t calibrationClassIdentity = 0u;
		std::uint32_t calibrationVersion = 0u;
		std::uint64_t deterministicSummaryHash = 0u;
	};

	struct GeometryRouteCandidate
	{
		GeometryRoute route = GeometryRoute::DirectMeshlet;
		bool eligible = false;
		std::optional<double> estimatedCost;
		std::vector<std::string> rejectionReasons;
	};

	struct GeometryRoutePolicyState
	{
		GeometryRoute current = GeometryRoute::DirectMeshlet;
		GeometryRoute challenger = GeometryRoute::DirectMeshlet;
		std::uint32_t stableFrames = 0u;
		std::uint32_t challengerFrames = 0u;
	};

	struct GeometryRouteDecision
	{
		std::uint64_t decisionIdentity = 0u;
		std::uint64_t frameId = 0u;
		std::uint64_t viewFamilyId = 0u;
		std::uint64_t objectIdentity = 0u;
		std::uint64_t meshIdentity = 0u;
		std::uint64_t assetGeneration = 0u;
		GeometryRoute selected = GeometryRoute::DirectMeshlet;
		std::vector<GeometryRouteCandidate> candidates;
		std::uint64_t policyIdentity = 0u;
		std::uint64_t calibrationIdentity = 0u;
		GeometryRouteSelectionSource source = GeometryRouteSelectionSource::Automatic;
		std::optional<double> estimatedCost;
		std::optional<double> measuredCost;
		double confidenceMargin = 0.0;
		std::uint32_t stableFrameCount = 0u;
		GeometryRouteFallbackReason fallbackReason = GeometryRouteFallbackReason::None;
		std::string fallbackDiagnostic;
		std::uint64_t routeGeneration = 0u;
		std::uint64_t deterministicHash = 0u;
		GeometryRoutePolicyState nextState{};
	};

	struct DirectMeshletBatch
	{
		std::uint64_t geometryIdentity = 0u;
		std::uint64_t geometryGeneration = 0u;
		std::uint64_t materialIdentity = 0u;
		std::vector<std::uint64_t> instanceIdentities;
		std::uint32_t firstMeshlet = 0u;
		std::uint32_t meshletCount = 0u;
	};

	struct PreparedRenderRouteEnvelope
	{
		std::uint64_t frameId = 0u;
		std::uint64_t snapshotGeneration = 0u;
		std::vector<GeometryRouteDecision> routeDecisions;
		std::vector<DirectMeshletBatch> directBatches;
		std::vector<VirtualGeometryPacketIdentity> virtualGeometryPackets;
		std::uint64_t backendCapabilityIdentity = 0u;
		std::uint64_t preparationHash = 0u;
		std::vector<std::string> rejectionDiagnostics;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return frameId != 0u && snapshotGeneration != 0u && preparationHash != 0u;
		}
	};

	struct GeometryClassPolicy
	{
		std::uint64_t policyIdentity = 0u;
		std::uint32_t policyVersion = 0u;
		GeometryClass geometryClass = GeometryClass::Static;
		GeometryClassSupportState supportState = GeometryClassSupportState::Unavailable;
		std::uint64_t allowedRouteMask = 0u;
		std::uint64_t allowedRepresentationMask = 0u;
		std::uint64_t requiredCapabilityMask = 0u;
		std::uint64_t requiredViewMask = 0u;
		std::string fallbackRepresentation;
		std::uint64_t evidenceIdentity = 0u;
	};

	struct GeometryGenerationCandidate
	{
		std::uint64_t candidateIdentity = 0u;
		std::uint64_t sourceGeneration = 0u;
		std::uint64_t representationIdentity = 0u;
		GeometryClassPolicy classPolicy{};
		GeometryGenerationState state = GeometryGenerationState::Prepared;
		std::uint64_t validationHash = 0u;
		std::uint64_t predecessorGeneration = 0u;
		std::vector<std::string> validationFailures;

		[[nodiscard]] bool CanPublish() const noexcept
		{
			return candidateIdentity != 0u
				&& sourceGeneration != 0u
				&& validationHash != 0u
				&& state == GeometryGenerationState::Validated
				&& validationFailures.empty();
		}
	};
} // namespace TerraEngine::Renderer
