// ShadowWorkPlan.h — renderer-neutral immutable shadow input/publication policy.
// Plans contain copied semantic values, never live scene or Vulkan pointers.
#pragma once

#include "Utilities/UUID.h"
#include "Core/Export.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

enum class RenderViewType : std::uint8_t;
enum class ShadowResourceKind : std::uint32_t;

namespace TerraEngine::Renderer
{
	struct ShadowCanonicalOrigin
	{
		std::array<std::int64_t, 3> cell{};
		std::array<double, 3> offsetMetres{};
		bool operator==(const ShadowCanonicalOrigin&) const = default;
	};
	struct ShadowLightProjectionStamp
	{
		std::array<double, 16> viewProjection{};
		std::array<double, 4> lightParameters{};
		std::array<double, 4> shadowFilterParameters{};
		std::uint64_t semanticRevision = 0;
		// A cube token authorizes six physical faces together. Retain every exact
		// matrix rather than assuming face zero or a digest proves the other five.
		std::array<std::array<double, 16>, 6> cubeViewProjections{};
		bool cubeProjectionEvidenceComplete = false;
		bool operator==(const ShadowLightProjectionStamp&) const = default;
	};
	struct ShadowViewKey
	{
		std::uint64_t rendererSessionGeneration = 0;
		RenderViewType viewType = static_cast<RenderViewType>(0);
		std::uint32_t viewId = 0;
		ShadowCanonicalOrigin referenceOrigin{};
		UUID lightId{};
		ShadowResourceKind resourceKind = static_cast<ShadowResourceKind>(0);
		std::uint32_t faceIndex = 0;
		std::uint64_t allocationGeneration = 0, regionId = 0;
		std::array<std::uint32_t, 2> extent{};
		std::uint32_t depthFormat = 0;
		ShadowLightProjectionStamp lightProjectionStamp{};
		bool operator==(const ShadowViewKey&) const = default;
	};
	// Explicit proof bits distinguish known zero (for example a bind-pose mesh)
	// from a missing generation. Unknown required evidence always forbids reuse.
	enum class ShadowCasterEvidence : std::uint64_t
	{
		Identity = 1ULL << 0, Transform = 1ULL << 1, Geometry = 1ULL << 2,
		Material = 1ULL << 3, Shader = 1ULL << 4, Texture = 1ULL << 5,
		Deformation = 1ULL << 6, Selection = 1ULL << 7, Residency = 1ULL << 8
	};
	inline constexpr std::uint64_t kCompleteShadowCasterEvidence = (1ULL << 9) - 1;
	struct ShadowTexturePublicationStamp
	{
		UUID textureId{};
		std::uint64_t publicationGeneration = 0;
		std::uint32_t bindingIndex = 0;
		bool operator==(const ShadowTexturePublicationStamp&) const = default;
	};
	// Captured authoring and native support proof; absent proof stays dynamic.
	struct ShadowStaticSupport
	{
		bool authoredStatic = false, knownDefaultShadowAbi = false, ownerPublicationsComplete = false;
		bool materialShadowSupported = false, geometrySupported = false, deformationAbsent = false;
		bool operator==(const ShadowStaticSupport&) const = default;
	};
	struct ShadowCasterStamp
	{
		UUID objectId{};
		std::uint64_t lifetimeGeneration = 0, segmentId = 0;
		UUID meshId{}, assetId{}, materialId{}, shaderId{}, textureId{};
		std::uint64_t meshRevision = 0, assetRevision = 0, materialRevision = 0;
		std::uint64_t shaderRevision = 0, textureRevision = 0;
		// Every actual sampler dependency retains exact accepted identity. A hash
		// cannot authorize alpha-tested reuse when a second binding changes.
		std::vector<ShadowTexturePublicationStamp> texturePublications;
		// Exact canonical transform values avoid pointer/padding signatures and
		// preserve large-world cells independently from localized float matrices.
		ShadowCanonicalOrigin worldPosition{}, referenceOrigin{};
		std::array<double, 4> rotation{1, 0, 0, 0};
		std::array<double, 3> scale{1, 1, 1};
		std::uint64_t shadowBehavior = 0;
		std::array<double, 4> materialShadowParameters{};
		std::uint64_t animationInstanceId = 0, poseGeneration = 0, morphGeneration = 0;
		std::uint64_t paletteGeneration = 0, deformationGeneration = 0, deformationProgramGeneration = 0;
		// The content fingerprint is a fast selector, never an equality proof.
		// Exact CPU inputs follow; transient plan/packet/upload offsets stay separate.
		std::uint64_t deformationContentIdLow = 0, deformationContentIdHigh = 0;
		// Immutable CPU shader inputs establish exact proof beyond a fingerprint.
		// Known bind pose uses empty vectors; GPU-only inputs lack Deformation proof.
		std::vector<std::array<double, 16>> deformationPalette;
		std::vector<double> morphWeights;
		std::uint32_t geometryRoute = 0, selectedLod = 0;
		// Selection flags can change deformation/fallback behavior even when the
		// meshlet span stays equal. Retain initialized bits as semantic inputs.
		std::uint32_t selectionFlags = 0;
		std::uint64_t firstMeshlet = 0, meshletCount = 0, firstCluster = 0, clusterCount = 0;
		std::uint64_t pageId = 0, fallbackPageId = 0, pageContentRevision = 0, residencyGeneration = 0;
		std::uint64_t payloadContentRevision = 0, payloadByteOffset = 0, payloadByteSize = 0;
		std::uint32_t payloadSource = 0, emittedSource = 0;
		bool staticEligible = false;
		ShadowStaticSupport staticSupport{};
		std::uint32_t staticClassificationReason = 0;
		std::uint64_t evidenceMask = 0;
		// Pose/morph generations remain diagnostic observation counters. Exact
		// drawn palette/weights and all other semantic inputs authorize equality.
		TERRAENGINE_API bool operator==(const ShadowCasterStamp&) const noexcept;
	};
	// These observation selectors intentionally never enter semantic rows. A
	// republished pose or different upload slot is not itself changed shadow data.
	struct ShadowCasterTransientIdentity
	{
		std::uint64_t frameId = 0, viewGeneration = 0, packetSerial = 0, uploadByteOffset = 0;
	};
	struct ShadowCasterStampInput
	{
		ShadowCasterStamp semantic{};
		ShadowCasterTransientIdentity transient{};
	};
	struct ShadowCasterMembership
	{
		std::vector<ShadowCasterStamp> rows;
		std::uint64_t digest = 0;
		bool reusable = false;
		std::string reason;
	};
	TERRAENGINE_API bool IsShadowCasterStampReusable(const ShadowCasterStamp& stamp);
	TERRAENGINE_API bool ExactShadowCasterStampEqual(const ShadowCasterStamp& left, const ShadowCasterStamp& right);
	TERRAENGINE_API ShadowCasterMembership NormalizeShadowCasterMembership(std::span<const ShadowCasterStampInput> inputs);
	TERRAENGINE_API bool ExactShadowCasterMembershipEqual(const ShadowCasterMembership& left, const ShadowCasterMembership& right);

	enum class StaticShadowClassificationReason : std::uint8_t
	{
		Eligible, NotAuthoredStatic, UnknownShaderAbi, IncompleteOwners,
		UnsupportedMaterial, UnsupportedGeometry, Deformable, IncompleteSemanticProof
	};
	struct StaticShadowClassification
	{
		bool eligible = false;
		StaticShadowClassificationReason reason = StaticShadowClassificationReason::IncompleteSemanticProof;
	};
	struct StaticShadowPartition
	{
		ShadowViewKey view{};
		bool valid = false;
		std::string reason;
		ShadowCasterMembership allMembership, staticMembership, dynamicMembership;
		// Draw indices retain original input order, independent of normalized rows.
		std::vector<std::uint32_t> allSourceIndices, staticSourceIndices, dynamicSourceIndices;
	};
	TERRAENGINE_API StaticShadowClassification ClassifyStaticShadowCaster(const ShadowCasterStamp& stamp);
	TERRAENGINE_API StaticShadowPartition BuildStaticShadowPartition(const ShadowViewKey& view,
		std::span<const ShadowCasterStampInput> inputs);

	inline constexpr std::uint64_t kStaticShadowDefaultBudgetBytes = 64ULL * 1024ULL * 1024ULL;
	enum class StaticShadowChargeState : std::uint8_t { Reserved, Resident, Retiring };
	struct StaticShadowAdmissionRequest
	{
		ShadowViewKey view{};
		double retainedLightPriority = 0;
		std::uint64_t queriedDepthBytes = 0, queriedAuxBytes = 0;
		std::uint8_t requiredFaceMask = 1;
	};
	struct StaticShadowAllocationCharge
	{
		ShadowViewKey view{};
		std::uint64_t allocationGeneration = 0, depthBytes = 0, auxiliaryBytes = 0;
		double retainedLightPriority = 0;
		// Reserved bytes are queried requirements; Resident/Retiring are actual
		// allocation charges. Command references live only in the native tracker.
		StaticShadowChargeState state = StaticShadowChargeState::Reserved;
	};
	struct StaticShadowAdmissionPlan
	{
		bool valid = false, draining = false;
		std::string reason;
		std::uint64_t requestedBytes = 0, effectiveBytes = 0, chargedBytes = 0;
		// chargedBytes/categories describe supplied physical owners only. The
		// separate newReservationBytes must be added when installing this plan.
		std::uint64_t reservedBytes = 0, residentBytes = 0, retiringBytes = 0, newReservationBytes = 0;
		// Request indices and charge indices are separate namespaces.
		std::vector<std::uint32_t> admittedRequestIndices, deferredRequestIndices;
		std::vector<std::uint32_t> retainedChargeIndices, retireChargeIndices;
	};
	TERRAENGINE_API StaticShadowAdmissionPlan BuildStaticShadowAdmissionPlan(
		std::uint64_t requestedBytes, std::uint64_t previousEffectiveBytes,
		std::span<const StaticShadowAdmissionRequest> requests,
		std::span<const StaticShadowAllocationCharge> charges);
	// chargedBytes includes this reservation. Actual bytes must fit it and the
	// effective bound before an unused allocation can become a published owner.
	TERRAENGINE_API bool ReconcileStaticShadowAllocation(const StaticShadowAdmissionRequest& reservation,
		std::uint64_t actualDepthBytes, std::uint64_t actualAuxiliaryBytes,
		std::uint64_t chargedBytes, std::uint64_t effectiveBytes, std::string& reason);

	enum class ShadowCachePublicationState : std::uint8_t { Invalid, Prepared, Recorded, Submitted, Complete };
	struct ShadowRefreshCandidate
	{
		ShadowViewKey view{};
		ShadowCasterMembership membership{};
		std::uint64_t contentToken = 0, frameSerial = 0, commandSerial = 0;
		std::uint8_t requiredFaceMask = 1, recordedFaceMask = 1;
		bool recordingComplete = true;
	};
	struct ShadowCachePublicationEntry
	{
		ShadowViewKey view{};
		ShadowCasterMembership membership{};
		std::uint64_t contentToken = 0, frameSerial = 0, commandSerial = 0, submitSerial = 0, completionSerial = 0;
		ShadowCachePublicationState status = ShadowCachePublicationState::Invalid;
	};
	class TERRAENGINE_API ShadowCachePublicationLedger
	{
	public:
		explicit ShadowCachePublicationLedger(std::uint64_t session = 1);
		bool Prepare(const ShadowRefreshCandidate& candidate);
		bool Record(const ShadowRefreshCandidate& candidate);
		bool ConfirmSubmission(std::uint64_t commandSerial, std::uint64_t submitSerial, bool succeeded = true);
		// Commands must be listed in their actual order in one graphics submission.
		bool ConfirmSubmissionBatch(std::span<const std::uint64_t> orderedCommandSerials,
			std::uint64_t submitSerial, bool succeeded = true);
		// A later incomplete physical writer may tombstone an earlier candidate.
		// Submission filters actual commands using current pending coverage.
		bool HasRecordedCandidate(std::uint64_t commandSerial) const noexcept;
		bool HasRecordedCandidate(const ShadowViewKey& view, std::uint64_t contentToken,
			std::uint64_t commandSerial) const noexcept;
		void Discard(std::uint64_t commandSerial);
		void Complete(std::uint64_t submitSerial, std::uint64_t completionSerial);
		bool CanReuse(const ShadowViewKey& view, std::uint64_t contentToken) const;
		bool AliasSubmittedContent(const ShadowViewKey& qualifiedView, std::uint64_t expectedContentToken,
			const ShadowCasterMembership& exactMembership);
		std::optional<ShadowCachePublicationEntry> Find(const ShadowViewKey& view) const;
		// Validation can inspect the actual submitted region owner across views.
		// Callers still use CanReuse with that full key; this grants no cache hit.
		std::optional<ShadowCachePublicationEntry> FindSubmittedPhysicalOwner(const ShadowViewKey& overlappingView) const;
		// Copied diagnostics let regressions prove moving views do not retain an
		// obsolete full caster collection for every previous projection.
		std::size_t RetainedLogicalEntryCount() const noexcept { return entries.size(); }
		void Reset(std::uint64_t session);
		// Revoke one native region without resetting other views or the submit clock.
		// This logical operation never releases a charged physical allocation.
		void InvalidatePhysicalRegion(const ShadowViewKey& view) noexcept;
	private:
		std::uint64_t rendererSessionGeneration = 1;
		std::vector<ShadowCachePublicationEntry> entries;
		std::vector<ShadowCachePublicationEntry> submittedPhysicalOwners;
		std::vector<ShadowRefreshCandidate> pending;
		// Blocked ownership is separate from last submitted diagnostic receipts.
		// Once recording can overwrite a face, failure never revives an old alias.
		std::vector<ShadowViewKey> blockedPhysicalRegions;
		std::uint64_t lastSubmissionSerial = 0;
	};
}

namespace TerraEngine::Renderer
{
	// These are copied protected preparation identities, not live backend pointers.
	// Per-instance models and selected offsets remain in RenderInstanceRecord.
	struct ShadowBatchCompatibilityKey
	{
		UUID meshId{}, materialId{}, shaderId{};
		std::uint64_t pipeline = 0, layout = 0, pipelineGeneration = 0, layoutGeneration = 0;
		std::array<std::uint64_t, 2> descriptors{}, descriptorGenerations{};
		std::uint64_t meshGeneration = 0, materialGeneration = 0, shaderGeneration = 0, residencyGeneration = 0;
		std::uint32_t materialSelector = 0, textureSelector = 0, materialFlags = 0, shadowPolicy = 0;
		std::uint32_t geometryRoute = 0, deformationPolicy = 0, selectionFlags = 0, selectedLod = 0;
		std::uint32_t dispatchMeshletCount = 0;
		// Exact initialized push range triples: stages, byte offset, byte size.
		std::array<std::array<std::uint32_t, 3>, 3> pushRanges{};
		std::uint32_t pushRangeCount = 0;
		bool operator==(const ShadowBatchCompatibilityKey&) const = default;
		auto operator<=>(const ShadowBatchCompatibilityKey&) const = default;
	};
	struct ShadowBatchMember
	{
		std::uint32_t sourceOrdinal = 0;
		UUID objectId{};
		std::uint64_t lifetimeGeneration = 0, segmentId = 0;
		std::uint64_t firstMeshlet = 0, meshletCount = 0, firstCluster = 0, clusterCount = 0;
		std::uint64_t pageId = 0, payloadOffset = 0, payloadSize = 0;
		ShadowBatchCompatibilityKey key{};
		bool knownDefaultTaskAbi = false, supportedRoute = false, deformationBridgeComplete = false;
		bool conservativeDeformation = false;
	};
	struct ShadowBatchLimits
	{
		// Capacity is an available record count starting at firstInstance, not an
		// absolute end index. Task limits apply to each resulting indirect draw.
		std::uint32_t firstInstance = 0, instanceCapacity = 0, maxInstancesPerGroup = 0, maxGroups = 0;
		std::uint32_t maxTaskGroupsX = 0, maxTaskGroupsTotal = 0;
	};
	enum class ShadowBatchFailure : std::uint8_t { None, InvalidView, InvalidMember, DuplicateContribution, InvalidLimits, Capacity };
	struct ShadowBatchGroup
	{
		ShadowBatchCompatibilityKey key{};
		std::uint32_t representativeIndex = 0, firstInstance = 0, instanceCount = 0, taskGroupsX = 0;
		std::vector<std::uint32_t> memberIndices;
	};
	struct ShadowBatchPlan
	{
		ShadowViewKey view{};
		bool coverageComplete = false;
		ShadowBatchFailure failure = ShadowBatchFailure::None;
		std::vector<ShadowBatchGroup> groups;
		std::vector<std::uint32_t> singletonIndices;
		// Retained even on successful preparation. Only actual complete upload and
		// recording admission may suppress these original singleton commands.
		std::vector<std::uint32_t> allSingletonIndices;
	};
	TERRAENGINE_API ShadowBatchPlan BuildShadowBatchPlan(const ShadowViewKey& view,
		std::span<const ShadowBatchMember> members, const ShadowBatchLimits& limits);
}
