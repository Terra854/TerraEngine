// TerrainDiagnostics.h -- Immutable status records for terrain consumers.

#pragma once

#include "Resource/ResourceStreaming.h"
#include "Terrain/TerrainEditTransaction.h"
#include "Terrain/TerrainLayerStack.h"
#include "Terrain/TerrainMaterial.h"

#include <vector>
#include <optional>
#include <string>
#include <utility>

namespace TerraEngine::Physics::Terrain { struct TerrainCollisionSnapshot; }

namespace TerraEngine::Terrain
{
	enum class TerrainDerivedStatus : std::uint8_t { Absent, Queued, Preparing, ReadyCandidate, PublishedCurrent, Superseded, RejectedStale, Cancelled, Failed };

	struct TerrainResidencyRecord
	{
		TerrainAreaCoordinate coordinate{};
		std::uint64_t residentBytes = 0u;
		bool pinned = false;
	};
	// Resource pressure is supplied by the residency owner. Keeping used and
	// budget values together prevents diagnostics from guessing a percentage.
	struct TerrainResourcePressureRecord { std::string resource; std::uint64_t usedBytes = 0u; std::uint64_t budgetBytes = 0u; bool evictionRequired = false; };
	struct TerrainSeamRecord { TerrainAreaCoordinate coordinate{}; TerrainAreaCoordinate neighbour{}; bool holeBoundary = false; std::string message; };

	enum class TerrainPageByteState : std::uint8_t
	{
		Missing,
		Requested,
		Reading,
		Decoded,
		Uploading,
		Resident,
		RetirePending,
		Rejected
	};

	struct TerrainAreaLodDiagnosticRecord
	{
		TerrainAreaCoordinate coordinate{};
		std::uint16_t desiredLod = 0u;
		std::uint16_t balancedLod = 0u;
		std::uint16_t residentLod = 0u;
		float projectedErrorPixels = 0.0F;
		float targetErrorPixels = 1.5F;
		bool hysteresisActive = false;
		std::uint8_t stitchMask = 0u;
		bool holePinned = false;
		std::uint32_t desiredPageId = 0xffffffffu;
		std::uint32_t actualPageId = 0xffffffffu;
		std::uint16_t fallbackDepth = 0u;
		std::uint64_t residentBytes = 0u;
		TerrainPageByteState byteState = TerrainPageByteState::Missing;
		bool missingOuterHalo = false;
		bool rolledBack = false;
		std::string stableCode;
		std::string message;
	};

	struct TerrainJobRecord
	{
		TerraEngine::UUID jobId = TerraEngine::EMPTY_UUID;
		TerrainDerivedStatus status = TerrainDerivedStatus::Absent;
		std::uint64_t workerNanoseconds = 0u;
	};

	struct TerrainConflictRecord
	{
		TerraEngine::UUID transactionId = TerraEngine::EMPTY_UUID;
		TerrainResultCode code = TerrainResultCode::Ok;
		std::string message;
	};

	// These categories are stable automation payloads, not presentation strings.
	// A consumer can choose its own UI while retaining the exact recovery action.
	enum class TerrainDiagnosticSeverity : std::uint8_t { Information, Warning, Error };
	enum class TerrainDiagnosticImpact : std::uint8_t { None, DelayedDerivedData, LastKnownGoodActive, MutationRejected, Unavailable };
	enum class TerrainDiagnosticAction : std::uint8_t { None, Retry, Rebuild, ReloadSource, ResolveConflict, IncreaseBudget, LocateArea };
	enum class TerrainDiagnosticCategory : std::uint8_t
	{
		Job,
		Conflict,
		Collision,
		Derived,
		Residency,
		Source,
		Seam,
		LayerValidation,
		Sampler,
		Resize
	};
	struct TerrainDiagnosticItem
	{
		TerrainDiagnosticCategory category = TerrainDiagnosticCategory::Job;
		TerrainDiagnosticSeverity severity = TerrainDiagnosticSeverity::Information;
		TerrainDiagnosticImpact impact = TerrainDiagnosticImpact::None;
		TerrainDiagnosticAction action = TerrainDiagnosticAction::None;
		std::optional<TerrainAreaCoordinate> location;
		ContentHash sourceHash{};
		Generation sourceGeneration = 0u;
		std::string message;
	};
	struct TerrainStagePercentile { std::string stage; std::uint64_t p50Nanoseconds = 0u; std::uint64_t p95Nanoseconds = 0u; std::size_t sampleCount = 0u; };

	// Resize diagnostics are value-only so UI, automation, and tests can observe
	// the same proposal/publication state without reaching into a running job.
	struct TerrainResizeDiagnosticRecord
	{
		TerraEngine::UUID jobId = TerraEngine::EMPTY_UUID;
		TerrainResizeKind kind = TerrainResizeKind::Expand;
		TerrainSampleBounds currentBounds{};
		TerrainSampleBounds proposedBounds{};
		TerrainExpansionPolicy expansionPolicy = TerrainExpansionPolicy::EdgeBlend;
		TerrainBoundaryHolePolicy holePolicy = TerrainBoundaryHolePolicy::CloseAtFormerEdge;
		ContentHash sourceRootHash{};
		ContentHash proposalFingerprint{};
		Generation sourceGeneration = 0u;
		Generation commonReadyGeneration = 0u;
		float progress = 0.0F;
		std::size_t affectedAreas = 0u;
		std::size_t retiredAreas = 0u;
		std::size_t retainedHistoryBytes = 0u;
		TerrainResizeBudget budget{};
		TerrainCropLossSummary cropLoss{};
		bool cancellationAvailable = false;
		bool confirmationRequired = false;
		bool confirmationAcknowledged = false;
		bool rolledBack = false;
		TerrainResultCode terminalCode = TerrainResultCode::Ok;
		std::string stage;
		std::string message;
	};

	// A validation attempt remains visible even when publication is rejected and
	// the last-known-good terrain stays live. Counts describe the detached input;
	// the stable diagnostic code tells UI and automation which repair to offer.
	struct TerrainLayerValidationDiagnosticRecord
	{
		TerrainResultCode resultCode = TerrainResultCode::Ok;
		TerrainRootDiagnosticCode diagnosticCode = TerrainRootDiagnosticCode::None;
		std::uint32_t definitionCount = 0u;
		std::uint32_t enabledCount = 0u;
		bool accepted = true;
		bool legacyRootRejected = false;
		std::string message;
	};

	// These stable reasons are renderer-neutral diagnostic values. The Vulkan
	// sampler policy maps its capability result into this record before the
	// protected publication becomes visible to editor or automation consumers.
	enum class TerrainSamplerFallbackReason : std::uint8_t
	{
		None,
		RequestedLevelClamped,
		AnisotropyUnsupported,
		InvalidDeviceLimit,
		DescriptorCapacityExceeded
	};

	struct TerrainSamplerQualityDiagnosticRecord
	{
		std::uint16_t stableLayerId = 0u;
		SamplerFilter requestedQuality = SamplerFilter::Trilinear;
		SamplerFilter effectiveQuality = SamplerFilter::Trilinear;
		float requestedAnisotropy = 1.0F;
		float effectiveAnisotropy = 1.0F;
		TerrainSamplerFallbackReason fallbackReason = TerrainSamplerFallbackReason::None;
		std::string message;
	};

	// Source loss changes presentation only: paint remains keyed by stable IDs.
	// Editors expose these records so a fallback material is never mistaken for
	// destructive weight remapping.
	struct TerrainLayerFallbackRecord
	{
		std::uint16_t stableLayerId = 0u;
		TerraEngine::UUID materialAssetId = TerraEngine::EMPTY_UUID;
		TerrainLayerFallbackState state = TerrainLayerFallbackState::Healthy;
		std::string message;
	};

	// Collision products are derived and may lag an authored root. This record
	// makes the active last-known-good identity explicit instead of implying that
	// a pending or failed rebuild is physically current.
	enum class TerrainCollisionStatus : std::uint8_t { Unavailable, Pending, Current, Stale, Failed };
	struct TerrainCollisionRecord
	{
		TerrainAreaCoordinate coordinate{};
		TerrainCollisionStatus status = TerrainCollisionStatus::Unavailable;
		ContentHash sourceRootHash{};
		Generation sourcePublicationGeneration = 0u;
		ContentHash activeSourceRootHash{};
		Generation activeSourcePublicationGeneration = 0u;
		std::uint64_t ageMilliseconds = 0u;
		std::uint64_t buildLatencyMilliseconds = 0u;
		bool lastKnownGoodActive = false;
		std::string message;
	};

	struct TerrainDiagnosticSnapshot
	{
		Generation revision = 0u;
		Generation authoredGeneration = 0u;
		Generation savedGeneration = 0u;
		Generation visibleGeneration = 0u;
		Generation renderGeneration = 0u;
		Generation controlGeneration = 0u;
		Generation collisionGeneration = 0u;
		Generation commonReadyGeneration = 0u;
		std::optional<TerrainAreaCoordinate> selectedArea;
		std::size_t availableAreaCount = 0u;
		std::size_t retiringAreaCount = 0u;
		std::size_t collisionRegionCount = 0u;
		float streamingProgress = 0.0F;
		bool commonReadyMismatch = false;
		bool dirty = false;
		std::size_t pendingTransactions = 0u;
		std::size_t historyDepth = 0u;
		std::uint64_t historyBytes = 0u;
		std::vector<TerrainResidencyRecord> residency;
		std::vector<TerrainResourcePressureRecord> resourcePressure;
		std::vector<TerrainSeamRecord> seams;
		std::vector<TerrainAreaLodDiagnosticRecord> areaLod;
		std::vector<TerrainJobRecord> jobs;
		std::vector<TerrainConflictRecord> conflicts;
		std::vector<TerrainLayerValidationDiagnosticRecord> layerValidations;
		std::vector<TerrainSamplerQualityDiagnosticRecord> samplerQuality;
		std::vector<TerrainLayerFallbackRecord> layerFallbacks;
		std::vector<TerrainCollisionRecord> collisions;
		std::vector<TerrainResizeDiagnosticRecord> resizes;
		std::optional<TerraEngine::Resource::TerrainTextureStreamingDiagnosticSnapshot>
			textureStreaming;
		std::vector<TerrainDiagnosticItem> items;
		std::vector<TerrainStagePercentile> stagePercentiles;
	};
	struct TerrainDiagnosticBuildInput
	{
		TerrainDiagnosticSnapshot snapshot;
		// Timings are supplied by a scheduler/profiler. The builder never invents
		// latency for stages that did not report a sample.
		std::vector<std::pair<std::string, std::vector<std::uint64_t>>> stageSamples;
	};

	// Adapts a provider-neutral physics snapshot into the editor/automation value
	// contract. Callers supply timing because clocks belong to the scheduler.
	TERRAENGINE_API TerrainCollisionRecord MakeTerrainCollisionRecord(
		const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& snapshot,
		std::uint64_t ageMilliseconds = 0u, std::uint64_t buildLatencyMilliseconds = 0u);
	// Converts a resolved control sample into an actionable source diagnostic.
	// The weights themselves remain untouched and safe for rendering.
	TERRAENGINE_API void AppendTerrainLayerReferenceDiagnostics(
		TerrainDiagnosticSnapshot& snapshot,
		const TerrainResolvedWeightSample& resolved,
		bool requiredControlMissing = false);
	[[nodiscard]] TERRAENGINE_API TerrainLayerValidationDiagnosticRecord
		MakeTerrainLayerValidationDiagnosticRecord(
			TerrainResultCode resultCode,
			TerrainRootDiagnosticCode diagnosticCode,
			std::uint32_t definitionCount,
			std::uint32_t enabledCount,
			std::string message);
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(
		TerrainSamplerFallbackReason reason) noexcept;
	TERRAENGINE_API TerrainDiagnosticSnapshot BuildTerrainDiagnosticSnapshot(TerrainDiagnosticBuildInput input);
}
