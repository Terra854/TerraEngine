// MotionDatabaseCompiler.h - Cancellable deterministic motion-database cooking.

#pragma once

#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/MotionMatching/MotionMatchCpu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::AssetPipeline {
	inline constexpr std::uint32_t kMotionDatabaseAuthoringSchemaVersion = 1U;
	inline constexpr std::size_t kMotionDatabaseMaximumAuthoredSources = 4096U;
	inline constexpr std::size_t kMotionDatabaseMaximumAuthoredAnnotations = 65536U;
	inline constexpr std::uint64_t kMotionDatabaseMaximumAuthoringBytes = 16U * 1024U * 1024U;
	// History retains whole documents for exact restoration, so its depth must be
	// bounded independently from the already-bounded size of one sidecar.
	inline constexpr std::size_t kMotionDatabaseMaximumAuthoringHistoryEntries = 32U;

	// Legacy/pre-extracted samples remain supported for importers that already
	// produce the complete 48-lane vector. New compiler clients should prefer the
	// semantic clip input below so feature extraction has one reproducible owner.
	struct MotionDatabaseSourceSample {
		std::uint64_t stableSampleId = 0U;
		std::uint64_t sourceClipId = 0U;
		Animation::AnimationFixedTick sourceTick = 0;
		std::array<float, Animation::kMotionDatabaseFeatureCount> features{};
		std::uint64_t tagMask = 0U;
		std::uint32_t contactMask = 0U;
		std::uint16_t phaseQuantized = 0U;
		std::uint16_t transitionFlags = 0U;
		std::uint32_t continuityGroup = 0U;
		std::uint32_t exclusionFlags = 0U;
		std::uint64_t continuityPredecessorId = 0U;
		std::uint64_t continuitySuccessorId = 0U;
	};

	struct MotionDatabaseSourceVector3 {
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;
	};

	// Each of the three horizons contributes x/z displacement and x/z facing,
	// exactly matching the 12-lane trajectory block in the cooked schema.
	struct MotionDatabaseTrajectoryPoint {
		float positionX = 0.0F;
		float positionZ = 0.0F;
		float facingX = 0.0F;
		float facingZ = 1.0F;
	};

	struct MotionDatabaseClipSampleInput {
		// Zero requests a stable ID derived from sourceClipId and sourceTick.
		std::uint64_t stableSampleId = 0U;
		Animation::AnimationFixedTick sourceTick = 0;
		std::array<MotionDatabaseTrajectoryPoint, 3U> trajectory{};
		std::array<MotionDatabaseSourceVector3, 6U> posePoints{};
		std::array<MotionDatabaseSourceVector3, 3U> velocityPoints{};
		MotionDatabaseSourceVector3 facing{0.0F, 0.0F, 1.0F};
		std::uint64_t tagMask = 0U;
		std::uint32_t contactMask = 0U;
		std::uint16_t phaseQuantized = 0U;
		std::uint16_t transitionFlags = 0U;
		std::uint32_t continuityGroup = 0U;
		std::uint32_t exclusionFlags = 0U;
	};

	struct MotionDatabaseContactRange {
		Animation::AnimationFixedTick firstTick = 0;
		Animation::AnimationFixedTick lastTick = 0;
		std::uint32_t contactMask = 0U;

		auto operator<=>(const MotionDatabaseContactRange&) const = default;
	};

	struct MotionDatabaseTagRange {
		Animation::AnimationFixedTick firstTick = 0;
		Animation::AnimationFixedTick lastTick = 0;
		std::uint64_t tagMask = 0U;

		auto operator<=>(const MotionDatabaseTagRange&) const = default;
	};

	struct MotionDatabaseExclusionRange {
		Animation::AnimationFixedTick firstTick = 0;
		Animation::AnimationFixedTick lastTick = 0;
		std::uint32_t exclusionFlags = 0U;

		auto operator<=>(const MotionDatabaseExclusionRange&) const = default;
	};

	struct MotionDatabaseSourceClipInput {
		std::uint64_t sourceClipId = 0U;
		std::uint64_t sourceClipFingerprint = 0U;
		std::uint64_t baseTagMask = 0U;
		std::uint32_t baseContactMask = 0U;
		std::vector<MotionDatabaseClipSampleInput> samples;
		std::vector<MotionDatabaseContactRange> contactRanges;
		std::vector<MotionDatabaseTagRange> tagRanges;
		std::vector<MotionDatabaseExclusionRange> exclusionRanges;
	};

	using MotionDatabaseWorkerSourceResolver = std::function<Animation::AnimationStatus(
		std::vector<MotionDatabaseSourceClipInput>&, const std::atomic_bool&)>;

	struct MotionDatabaseCompileInput {
		Animation::AnimationGuid assetId{};
		Animation::AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint64_t sourceFingerprint = 0U;
		// The editor increments this value for every saved authoring change. A
		// worker product may publish only while the same revision is still current.
		std::uint64_t authoringGeneration = 0U;
		// Authoring sessions set this path so serialization and read-back staging
		// also run on the worker. Empty keeps the compiler's memory-only behavior.
		std::filesystem::path publicationPath;
		std::uint32_t samplesPerBlock = static_cast<std::uint32_t>(
			Animation::kMotionDatabaseSamplesPerBlockDefault);
		std::vector<MotionDatabaseSourceSample> samples;
		std::vector<MotionDatabaseSourceClipInput> sourceClips;
		// Async editor builds may defer clip decode/evaluation to the same worker
		// that cooks the database. Direct CompileMotionDatabase calls reject this
		// callback so expensive source preparation cannot run accidentally inline.
		MotionDatabaseWorkerSourceResolver workerSourceResolver;
	};

	// This JSON-backed document is the editable source of truth. Cooked database
	// files intentionally contain no editor paths, while this sidecar retains the
	// clip list, tag/contact annotations, exclusions, and preview feature costs.
	struct MotionDatabaseAuthoringSource {
		std::uint64_t sourceClipId = 0U;
		std::uint64_t sourceClipFingerprint = 0U;
		std::string sourcePath;
		bool enabled = true;
		std::uint64_t baseTagMask = 0U;
		std::uint32_t baseContactMask = 0U;
		std::vector<MotionDatabaseContactRange> contactRanges;
		std::vector<MotionDatabaseTagRange> tagRanges;
		std::vector<MotionDatabaseExclusionRange> exclusionRanges;

		auto operator<=>(const MotionDatabaseAuthoringSource&) const = default;
	};

	struct MotionDatabaseAuthoringDocument {
		std::uint32_t schemaVersion = kMotionDatabaseAuthoringSchemaVersion;
		std::uint64_t authoringGeneration = 1U;
		Animation::AnimationGuid assetId{};
		Animation::AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint32_t samplesPerBlock = static_cast<std::uint32_t>(
			Animation::kMotionDatabaseSamplesPerBlockDefault);
		std::array<std::uint16_t, Animation::kMotionDatabaseFeatureCount> featureWeights{};
		std::vector<MotionDatabaseAuthoringSource> sources;

		auto operator<=>(const MotionDatabaseAuthoringDocument&) const = default;
	};

	// A snapshot keeps both the typed document and the exact bytes that produced
	// it. The bytes are the compare-and-swap token, so an external rewrite with
	// the same generation cannot be mistaken for the version the editor loaded.
	struct MotionDatabaseAuthoringSnapshot {
		MotionDatabaseAuthoringDocument document;
		std::string serializedBytes;

		auto operator<=>(const MotionDatabaseAuthoringSnapshot&) const = default;
	};

	[[nodiscard]] Animation::AnimationStatus ValidateMotionDatabaseAuthoringDocument(
		const MotionDatabaseAuthoringDocument& document) noexcept;
	[[nodiscard]] std::uint64_t ComputeMotionDatabaseAuthoringFingerprint(
		const MotionDatabaseAuthoringDocument& document) noexcept;
	[[nodiscard]] Animation::AnimationStatus LoadMotionDatabaseAuthoringDocument(
		const std::filesystem::path& path, MotionDatabaseAuthoringDocument& output) noexcept;
	[[nodiscard]] Animation::AnimationStatus LoadMotionDatabaseAuthoringSnapshot(
		const std::filesystem::path& path, MotionDatabaseAuthoringSnapshot& output) noexcept;
	// The two-argument form is an atomic create: it rejects an existing path. Use
	// the snapshot overload for updates so every replacement proves which exact
	// bytes were edited and returns the new token for the next save.
	[[nodiscard]] Animation::AnimationStatus SaveMotionDatabaseAuthoringDocumentAtomically(
		const std::filesystem::path& path,
		const MotionDatabaseAuthoringDocument& document) noexcept;
	[[nodiscard]] Animation::AnimationStatus SaveMotionDatabaseAuthoringDocumentAtomically(
		const std::filesystem::path& path,
		const MotionDatabaseAuthoringDocument& document,
		const MotionDatabaseAuthoringSnapshot* expectedCurrent,
		MotionDatabaseAuthoringSnapshot* publishedCurrent) noexcept;

	// This façade owns complete typed documents instead of field-level commands.
	// One BeginEdit/CommitEdit pair therefore groups every source, annotation, and
	// feature change into one undo step without retaining references into vectors.
	class MotionDatabaseAuthoringHistory {
	public:
		explicit MotionDatabaseAuthoringHistory(
			std::size_t maximumEntries = kMotionDatabaseMaximumAuthoringHistoryEntries);
		void Reset(const MotionDatabaseAuthoringDocument& document);
		[[nodiscard]] Animation::AnimationStatus BeginEdit(
			const MotionDatabaseAuthoringDocument& document);
		[[nodiscard]] Animation::AnimationStatus CommitEdit(
			MotionDatabaseAuthoringDocument& document);
		[[nodiscard]] Animation::AnimationStatus CancelEdit(
			MotionDatabaseAuthoringDocument& document) noexcept;
		[[nodiscard]] Animation::AnimationStatus Undo(
			MotionDatabaseAuthoringDocument& document);
		[[nodiscard]] Animation::AnimationStatus Redo(
			MotionDatabaseAuthoringDocument& document);
		[[nodiscard]] bool IsEditActive() const noexcept;
		[[nodiscard]] bool CanUndo() const noexcept;
		[[nodiscard]] bool CanRedo() const noexcept;
		[[nodiscard]] const MotionDatabaseAuthoringDocument& Current() const noexcept;

	private:
		[[nodiscard]] Animation::AnimationStatus RestoreFrom(
			std::vector<std::shared_ptr<const MotionDatabaseAuthoringDocument>>& source,
			std::vector<std::shared_ptr<const MotionDatabaseAuthoringDocument>>& destination,
			MotionDatabaseAuthoringDocument& document);
		void PushBounded(
			std::vector<std::shared_ptr<const MotionDatabaseAuthoringDocument>>& stack,
			const MotionDatabaseAuthoringDocument& document);

		std::size_t maximumEntries_ = 0U;
		std::uint64_t greatestGeneration_ = 0U;
		bool initialized_ = false;
		MotionDatabaseAuthoringDocument current_;
		std::optional<MotionDatabaseAuthoringDocument> editBaseline_;
		std::vector<std::shared_ptr<const MotionDatabaseAuthoringDocument>> undo_;
		std::vector<std::shared_ptr<const MotionDatabaseAuthoringDocument>> redo_;
	};
	// Resolved clips contain immutable samples supplied by the editor's asset
	// service. This merge proves that every enabled persisted source resolves to
	// the exact clip fingerprint before a build is queued.
	[[nodiscard]] Animation::AnimationStatus BuildMotionDatabaseCompileInput(
		const MotionDatabaseAuthoringDocument& document,
		std::span<const MotionDatabaseSourceClipInput> resolvedClips,
		MotionDatabaseCompileInput& output) noexcept;

	struct MotionDatabaseCompileReport {
		std::uint32_t sourceSamples = 0U;
		std::uint32_t extractedSamples = 0U;
		std::uint32_t publishedSamples = 0U;
		std::uint32_t residentBlocks = 0U;
		std::uint64_t contentFingerprint = 0U;
		std::string diagnostic;
	};

	// The worker reads this copied token at bounded intervals between extraction,
	// normalization, quantization, indexing, and validation stages. It never waits
	// on another thread and a cancelled result cannot reach publication.
	struct MotionDatabaseCompileCancellation {
		const std::atomic_bool* requested = nullptr;
		std::atomic_uint32_t* completedSteps = nullptr;
		std::atomic_uint32_t* totalSteps = nullptr;

		[[nodiscard]] bool IsCancellationRequested() const noexcept {
			return requested != nullptr && requested->load(std::memory_order_acquire);
		}

		void ReportProgress(std::uint32_t completed, std::uint32_t total) const noexcept {
			if(totalSteps != nullptr) totalSteps->store(std::max(total, 1U), std::memory_order_release);
			if(completedSteps != nullptr) completedSteps->store(
				std::min(completed, std::max(total, 1U)), std::memory_order_release);
		}
	};

	Animation::AnimationStatus CompileMotionDatabase(
		const MotionDatabaseCompileInput& input, Animation::MotionDatabaseAsset& output,
		MotionDatabaseCompileReport& report,
		MotionDatabaseCompileCancellation cancellation = {}) noexcept;

	class MotionDatabaseCompileExecutor {
	public:
		virtual ~MotionDatabaseCompileExecutor() = default;
		virtual void Enqueue(std::function<void()> work) = 0;
	};

	struct MotionDatabasePublishedRecord {
		Animation::AnimationGeneration generation = 0U;
		std::uint64_t contentFingerprint = 0U;
		std::shared_ptr<const Animation::MotionDatabaseAsset> asset;
	};

	enum class MotionDatabaseBuildStage : std::uint8_t {
		Idle,
		Queued,
		Building,
		ReadyToPublish,
		Published,
		Cancelled,
		Rejected
	};

	struct MotionDatabaseBuildProgress {
		std::uint64_t sequence = 0U;
		std::uint64_t authoringGeneration = 0U;
		std::uint32_t completedSteps = 0U;
		std::uint32_t totalSteps = 1U;
		MotionDatabaseBuildStage stage = MotionDatabaseBuildStage::Idle;
		Animation::AnimationStatus status = Animation::AnimationStatus::NotReady;

		[[nodiscard]] float Fraction() const noexcept {
			return static_cast<float>(std::min(completedSteps, std::max(totalSteps, 1U)))
				/ static_cast<float>(std::max(totalSteps, 1U));
		}
	};

	using MotionDatabasePublicationValidator = std::function<Animation::AnimationStatus(
		const Animation::MotionDatabaseAsset&, const std::filesystem::path&)>;

	// The publisher owns copied build requests. Cancelling or rejecting one worker
	// product never exposes a partially normalized database to runtime readers.
	class MotionDatabaseCompiler {
	public:
		explicit MotionDatabaseCompiler(std::shared_ptr<MotionDatabaseCompileExecutor> executor,
			std::size_t maximumPendingJobs = 8U);
		[[nodiscard]] Animation::AnimationStatus RequestBuild(const MotionDatabaseCompileInput& input);
		void CancelPending() noexcept;
		[[nodiscard]] Animation::AnimationStatus PollAndPublish(
			Animation::AnimationGuid expectedAssetId,
			Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
			std::uint64_t expectedAuthoringGeneration = 0U,
			MotionDatabasePublicationValidator publicationValidator = {});
		[[nodiscard]] std::optional<MotionDatabasePublishedRecord> GetPublished() const;
		[[nodiscard]] MotionDatabaseBuildProgress GetBuildProgress() const noexcept;

	private:
		struct Job;
		std::shared_ptr<MotionDatabaseCompileExecutor> executor_;
		mutable std::mutex mutex_;
		std::size_t maximumPendingJobs_ = 0U;
		std::uint64_t newestSequence_ = 0U;
		Animation::AnimationGeneration nextGeneration_ = 1U;
		std::vector<std::shared_ptr<Job>> jobs_;
		std::optional<MotionDatabasePublishedRecord> published_;
		MotionDatabaseBuildProgress lastProgress_{};
	};

	// The staged bytes are read back and validated before one protected rename.
	// A stale authoring revision or any IO/validation failure leaves the previous
	// complete .temotiondb file untouched.
	[[nodiscard]] Animation::AnimationStatus PublishMotionDatabaseAssetAtomically(
		const std::filesystem::path& outputPath,
		const Animation::MotionDatabaseAsset& asset,
		std::uint64_t expectedAuthoringGeneration,
		std::uint64_t currentAuthoringGeneration) noexcept;

	struct MotionDatabaseQueryPreviewState {
		std::uint64_t databaseGeneration = 0U;
		Animation::MotionMatchQuery query{};
		std::vector<std::uint64_t> excludedSampleIds;
		Animation::MotionMatchCpuResult result{};

		MotionDatabaseQueryPreviewState() = default;
		MotionDatabaseQueryPreviewState(const MotionDatabaseQueryPreviewState& other)
			: databaseGeneration(other.databaseGeneration), query(other.query),
			  excludedSampleIds(other.excludedSampleIds), result(other.result) {
			query.excludedSampleIds = excludedSampleIds;
		}
		MotionDatabaseQueryPreviewState& operator=(const MotionDatabaseQueryPreviewState& other) {
			if(this == &other) return *this;
			databaseGeneration = other.databaseGeneration;
			query = other.query;
			excludedSampleIds = other.excludedSampleIds;
			result = other.result;
			query.excludedSampleIds = excludedSampleIds;
			return *this;
		}
		MotionDatabaseQueryPreviewState(MotionDatabaseQueryPreviewState&& other) noexcept
			: databaseGeneration(other.databaseGeneration), query(other.query),
			  excludedSampleIds(std::move(other.excludedSampleIds)), result(std::move(other.result)) {
			query.excludedSampleIds = excludedSampleIds;
		}
		MotionDatabaseQueryPreviewState& operator=(MotionDatabaseQueryPreviewState&& other) noexcept {
			if(this == &other) return *this;
			databaseGeneration = other.databaseGeneration;
			query = other.query;
			excludedSampleIds = std::move(other.excludedSampleIds);
			result = std::move(other.result);
			query.excludedSampleIds = excludedSampleIds;
			return *this;
		}
	};

	// This UI-independent session is the integration seam for the window and for
	// headless editor automation. The caller resolves clip samples, while the
	// session owns bounded async cooking, stale rejection, atomic file publication,
	// and the actual CPU reference query/candidate diagnostics shown by the UI.
	class MotionDatabaseAuthoringSession {
	public:
		explicit MotionDatabaseAuthoringSession(
			std::shared_ptr<MotionDatabaseCompileExecutor> executor,
			std::size_t maximumPendingJobs = 2U);
		[[nodiscard]] Animation::AnimationStatus RequestBuild(
			const MotionDatabaseAuthoringDocument& document,
			std::span<const MotionDatabaseSourceClipInput> resolvedClips,
			std::filesystem::path outputPath);
		[[nodiscard]] Animation::AnimationStatus RequestBuild(
			const MotionDatabaseAuthoringDocument& document,
			MotionDatabaseWorkerSourceResolver workerSourceResolver,
			std::filesystem::path outputPath);
		void CancelBuild() noexcept;
		[[nodiscard]] Animation::AnimationStatus Poll(
			const MotionDatabaseAuthoringDocument& currentDocument);
		[[nodiscard]] MotionDatabaseBuildProgress GetBuildProgress() const noexcept;
		[[nodiscard]] std::optional<MotionDatabasePublishedRecord> GetPublished() const;
		[[nodiscard]] Animation::AnimationStatus RunQueryPreview(
			Animation::MotionMatchQuery query,
			std::span<const std::uint64_t> excludedSampleIds = {});
		[[nodiscard]] std::optional<MotionDatabaseQueryPreviewState> GetQueryPreview() const;

	private:
		mutable std::mutex mutex_;
		MotionDatabaseCompiler compiler_;
		std::filesystem::path outputPath_;
		std::uint64_t requestedAuthoringGeneration_ = 0U;
		std::optional<MotionDatabaseQueryPreviewState> queryPreview_;
	};
}
