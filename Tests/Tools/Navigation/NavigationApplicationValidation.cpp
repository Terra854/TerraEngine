// NavigationApplicationValidation.cpp - scheduler-backed NavigationAgent acceptance workload.
//
// This utility belongs under Tests/Tools, but the engine links it for installed
// launcher validation. It never calls NavigationSystem::Update directly: every
// route capture, worker preparation, publication, and transactional mutation
// is observed through the active application's normal Engine::Update path.

#include "NavigationApplicationValidation.h"

#include "Components/Transform.h"
#include "Components/NavigationAgentComponent.h"
#include "Core/MainEngine.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/NavigationSystem.h"
#include "Navigation/Persistence/SavedNavigationCache.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Renderer/RenderSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "ThreadPool/ThreadPool.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Navigation::Validation {
	namespace {
		using Clock = std::chrono::steady_clock;

		constexpr std::uint32_t kMaximumPopulation = 4096U;
		constexpr std::uint32_t kMaximumChangesPerSecond = 10000U;
		constexpr std::uint32_t kMaximumDurationSeconds = 7200U;
		constexpr std::uint32_t kMaximumFrameBudget = 1'000'000U;
		constexpr std::uint32_t kMinimumQualificationWarmupSeconds = 30U;
		constexpr std::uint32_t kMinimumQualificationMeasurementSeconds = 600U;
		constexpr std::uint32_t kRequiredQualificationWorkers = 8U;
		constexpr std::uint32_t kMeasurementDrainSeconds = 30U;

		enum class Cohort : std::uint8_t {
			Direct,
			Agent
		};

		enum class ExecutionMode : std::uint8_t {
			Export,
			EditorPlay
		};

		enum class ValidationScenario : std::uint8_t {
			Workload,
			ClearanceParity
		};

		// Managed outcome probes are deliberately a validation-host concern, not
		// authored gameplay configuration. They keep the installed editor running
		// normally so MCP can observe the real C# facade, while preventing this
		// native workload runner from submitting an unrelated route cohort.
		enum class ManagedOutcomeProbe : std::uint8_t {
			None,
			Partial,
			Budget
		};

		enum class ClearanceProfileSelection : std::uint8_t {
			Small,
			Large
		};

		enum class CacheExpectation : std::uint8_t {
			Absent,
			Current,
			Stale,
			Malformed
		};

		struct WorkloadOptions {
			Cohort cohort = Cohort::Agent;
			ExecutionMode executionMode = ExecutionMode::Export;
			ValidationScenario scenario = ValidationScenario::Workload;
			ManagedOutcomeProbe managedOutcomeProbe = ManagedOutcomeProbe::None;
			ClearanceProfileSelection clearanceProfile = ClearanceProfileSelection::Small;
			std::uint32_t warmupSeconds = kMinimumQualificationWarmupSeconds;
			std::uint32_t measurementSeconds = kMinimumQualificationMeasurementSeconds;
			std::uint32_t requiredWorkerCount = kRequiredQualificationWorkers;
			std::uint32_t population = 1000U;
			std::uint32_t changesPerSecond = 100U;
			std::uint32_t frameBudget = 0U;
			std::uint32_t timeoutSeconds = 0U;
			std::filesystem::path fixtureManifestPath;
			std::filesystem::path cacheOutputPath;
			std::filesystem::path cacheStaleOutputPath;
			CacheExpectation expectedCacheMode = CacheExpectation::Absent;
			bool requireRenderedPresentation = false;
			bool headlessCpuFallback = false;
			bool qualificationRequested = false;
		};

		struct Counters {
			std::uint64_t submitted = 0U;
			std::uint64_t warmupSubmitted = 0U;
			std::uint64_t measurementSubmitted = 0U;
			std::uint64_t accepted = 0U;
			std::uint64_t measurementAdmitted = 0U;
			std::uint64_t measurementFinalized = 0U;
			std::uint64_t admittedFinalized = 0U;
			std::uint64_t measurementRejected = 0U;
			std::uint64_t measurementExpired = 0U;
			std::uint64_t measurementCancelled = 0U;
			std::uint64_t measurementSuperseded = 0U;
			std::uint64_t measurementTimedOut = 0U;
			std::uint64_t pendingAtCutoff = 0U;
			std::uint64_t rejected = 0U;
			std::uint64_t expired = 0U;
			std::uint64_t cancelled = 0U;
			std::uint64_t superseded = 0U;
			std::uint64_t timedOut = 0U;
			std::uint64_t retries = 0U;
			std::uint64_t coalesced = 0U;
			std::uint64_t completed = 0U;
			std::uint64_t following = 0U;
			std::uint64_t arrived = 0U;
			std::uint64_t stopped = 0U;
			std::uint64_t failed = 0U;
			std::uint64_t peakOutstanding = 0U;
			std::uint64_t memoryHighWaterBytes = 0U;
			std::uint64_t schedulerPrepareFrames = 0U;
			std::uint64_t movingBlockerUpdates = 0U;
			std::uint64_t movingBlockerMutationsQueued = 0U;
			std::uint64_t movingBlockerTransactionsCommitted = 0U;
			std::uint64_t movingBlockerTransactionsRejected = 0U;
			std::uint64_t movingBlockerObservedTransformRevision = 0U;
			std::uint64_t movingBlockerPublishedSourceRevision = 0U;
			std::string movingBlockerLastTransactionId;
			std::optional<Scene::WorldPosition> movingBlockerLastIntendedPose;
			std::optional<Scene::WorldPosition> movingBlockerLastObservedPose;
			std::uint64_t retainedPeak = 0U;
			std::uint64_t workerDispatchPeak = 0U;
			std::uint64_t agentPreparePartitionPeak = 0U;
			std::uint32_t agentPrepareChunkMask = 0U;
			// Scheduler execution records distinguish partition zero, which may prepare
			// one ground/dynamic build, from the route-only partitions. Keep bounded
			// totals and maxima instead of retaining one timing record per frame.
			std::uint64_t navigationPrepareObservedFrames = 0U;
			std::uint64_t navigationPreparePartitionZeroCount = 0U;
			std::uint64_t navigationPreparePartitionZeroRunNanoseconds = 0U;
			std::uint64_t navigationPreparePartitionZeroMaximumNanoseconds = 0U;
			std::uint64_t navigationPrepareRoutePartitionCount = 0U;
			std::uint64_t navigationPrepareRouteRunNanoseconds = 0U;
			std::uint64_t navigationPrepareRouteMaximumNanoseconds = 0U;
			std::uint64_t navigationPrepareRouteWorkItems = 0U;
			std::vector<double> latencyMilliseconds;
			std::vector<double> readyLatencyMilliseconds;
			// This stricter diagnostic starts at actual admission (not command
			// submission) and is recorded only when the matching profile coverage is
			// complete at both admission and terminal observation.
			std::vector<double> readyCoverageLatencyMilliseconds;
		};

		enum class LatencyDisposition : std::uint8_t {
			Pending,
			Ready,
			TerminalFailure,
			Rejected,
			Expired,
			Cancelled,
			Superseded,
			TimedOut
		};

		struct PendingLatency {
			Clock::time_point submittedAt{};
			Clock::time_point admittedAt{};
			NavigationRequestHandle directRequest{};
			NavigationAgentIdentity agent{};
			NavigationProfile profile{};
			NavigationGeneration requiredIntent = 0U;
			std::uint64_t correlationId = 0U;
			std::uint32_t ownerIndex = 0U;
			bool measurement = false;
			bool admissionApplied = false;
			bool readyCoverageAtAdmission = false;
			bool active = false;
			LatencyDisposition disposition = LatencyDisposition::Pending;
		};

		struct Session {
			Engine* engine = nullptr;
			WorkloadOptions options{};
			Clock::time_point startedAt{};
			Clock::time_point measurementStartedAt{};
			Clock::time_point nextChangeAt{};
			Clock::time_point nextFrameDue{};
			std::uint64_t nextCorrelationId = 1U;
			std::uint64_t nextOrderingToken = 1U;
			std::uint64_t changeIndex = 0U;
			std::uint64_t frames = 0U;
			std::uint64_t lastBlockerSecond = 0U;
			std::optional<Scene::WorldPosition> pendingBlockerPosition;
			bool measurementStarted = false;
			// Copy bounded publication counters, not diagnostic history. The first four
			// retain the public rebase outcomes; the remaining six identify why a
			// fail-closed Unknown could not traverse its transition certificate.
			std::array<std::uint64_t, 10U> routeTransitionCounters{};
			std::array<std::uint64_t, 10U> routeTransitionBaseline{};
			nlohmann::json firstObservedTransitionUnknownDiagnostics = nlohmann::json::array();
			// These are copied published observations, not claims about allocator RSS
			// or task state after the application stops sampling its final frame.
			std::array<std::uint64_t, 7U> asyncBuildLastObserved{};
			std::uint64_t asyncBuildPeakOutstandingObserved = 0U;
			// Fixed-size scalar sampling separates provider work from queue and poll
			// delays. It does not change request latency samples or qualification gates.
			std::array<std::uint64_t, 8U> asyncTimingCounters{};
			std::array<std::uint64_t, 8U> asyncTimingBaseline{};
			// Route stages use their own populations: provider dispatch timing cannot
			// explain a ready-route FIFO backlog or time waiting for dirty coverage.
			std::array<std::uint64_t, 12U> routeTimingCounters{};
			std::array<std::uint64_t, 12U> routeTimingBaseline{};
			// Provider work counters show whether the measured application actually
			// reused tiles. Baselines separate startup work from measurement and drain.
			std::array<std::uint64_t, 4U> tileReuseCounters{};
			std::array<std::uint64_t, 4U> tileReuseBaseline{};
			nlohmann::json firstObservedTileReuseFallbacks = nlohmann::json::array();
			// Scheduler topology uses a stable frameId. The monotonic execution end
			// timestamp instead prevents repeated polling from counting one frame twice.
			std::uint64_t lastObservedNavigationPrepareEndNanoseconds = 0U;
			bool measurementDrainStarted = false;
			Clock::time_point measurementDrainStartedAt{};
			bool renderedPresentationObserved = false;
			bool navigationDidNotMoveOwners = true;
			bool fixtureBindingsObserved = false;
			// Binding waits are expected while terrain/geometry publications warm up.
			// Keep the latest value-only reason and root counts so a real timeout names
			// the missing producer instead of reporting an opaque zero-work failure.
			std::string fixtureBindingWaitReason;
			std::uint32_t fixtureRootTerrainState = 0U;
			std::uint64_t fixtureRootTerrainPublicationGeneration = 0U;
			std::size_t fixtureRootCoverageCount = 0U;
			std::size_t fixtureRootSourceCount = 0U;
			// The navigation world can remain absent when its upstream terrain boundary
			// has not run. Preserve the public collision snapshot so timeout evidence
			// names staging, scheduling, or provider rejection rather than just zeroes.
			std::uint32_t fixtureCollisionStatus = 0U;
			std::uint32_t fixtureCollisionPendingCandidates = 0U;
			std::uint64_t fixtureCollisionCandidatesStaged = 0U;
			std::uint64_t fixtureCollisionPublicationAttempts = 0U;
			std::uint64_t fixtureCollisionPublicationCommits = 0U;
			std::uint64_t fixtureCollisionPublicationRejections = 0U;
			std::string fixtureCollisionMessage;
			// Preserve compact scheduler evidence from real completed application frames.
			// This is diagnostic only: it identifies a missing protected boundary or an
			// inactive prerequisite without retaining callbacks or whole frame plans.
			std::uint64_t omittedWorkloadFrames = 0U;
			nlohmann::json frameEndWorkload = nlohmann::json(nullptr);
			nlohmann::json firstOmittedWorkload = nlohmann::json(nullptr);
			nlohmann::json lastOmittedWorkload = nlohmann::json(nullptr);
			nlohmann::json firstPlanDiagnostic = nlohmann::json(nullptr);
			nlohmann::json lastPlanDiagnostic = nlohmann::json(nullptr);
			bool eligibleStaticSourcesObserved = false;
			std::uint32_t eligibleStaticSourceCount = 0U;
			bool warmupSeeded = false;
			bool warmupRetainedPopulationReady = false;
			bool warmupReachabilityCaptured = false;
			nlohmann::json warmupReachability = nlohmann::json::array();
			bool cacheOutputWritten = false;
			bool cacheStaleOutputWritten = false;
			std::string cacheOutputFailure;
			std::optional<SavedNavigationDescriptor> currentCacheDescriptor;
			std::optional<NavigationSystem::SavedNavigationCacheStatus> completedCacheStatus;
			bool workloadProfileRegistered = false;
			bool blockerMeasurementStarted = false;
			bool schedulerDiagnosticsWereEnabled = false;
			bool schedulerDiagnosticsRequested = false;
			bool clearanceRequestSubmitted = false;
			bool clearanceRequestTerminal = false;
			NavigationRequestHandle clearanceRequest{};
			// Clearance diagnostics are capped independently of workload history. They
			// distinguish a doorway route from an unintended path around fixture walls.
			nlohmann::json clearanceSourceGeometry = nlohmann::json::array();
			nlohmann::json clearanceRoutePoints = nlohmann::json::array();
			std::size_t clearanceRoutePointCount = 0U;
			NavigationOutcome clearanceOutcome = NavigationOutcome::Queued;
			NavigationReason clearanceReason = NavigationReason::None;
			bool ledgerCapacityExceeded = false;
			bool teardownStarted = false;
			std::string teardownFailure;
			bool ownersDeactivationQueued = false;
			bool preRevokeUsageObserved = false;
			bool postRevokeUsageObserved = false;
			std::uint32_t teardownFrames = 0U;
			NavigationSystem::StoreUsageSnapshot preRevokeStoreUsage{};
			NavigationSystem::StoreUsageSnapshot postRevokeStoreUsage{};
			std::vector<UUID> ownerIds;
			std::vector<NavigationAgentIdentity> agentIdentities;
			std::vector<NavigationRequestHandle> currentDirectRequests;
			std::vector<NavigationRequestHandle> retiredDirectRequests;
			std::vector<PendingLatency> pendingLatency;
			std::vector<Scene::WorldPosition> initialOwnerPositions;
			Counters counters{};
		};

		std::mutex sessionMutex;
		std::optional<Session> activeSession;

		[[nodiscard]] nlohmann::json Failure(std::string message)
		{
			return {
				{"succeeded", false},
				{"pending", false},
				{"orderlyShutdown", false},
				{"executedCaseCount", 0U},
				{"actualWorkerCount", ThreadPool::instance().getThreadCount()},
				{"semanticChecks", nlohmann::json::object()},
				{"latencyMilliseconds", nlohmann::json::object()},
				{"successOnlyLatencyMilliseconds", nlohmann::json::object()},
				{"failure", std::move(message)}
			};
		}

		[[nodiscard]] bool ReadUnsigned(const nlohmann::json& document, std::string_view key,
			std::uint32_t& output, std::string& error)
		{
			const auto entry = document.find(std::string(key));
			if(entry == document.end()) {
				return true;
			}
			if(!entry->is_number_integer()
				|| (!entry->is_number_unsigned() && entry->get<std::int64_t>() < 0)
				|| entry->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
				error = "Option '" + std::string(key) + "' must be an unsigned 32-bit integer.";
				return false;
			}
			output = entry->get<std::uint32_t>();
			return true;
		}

		[[nodiscard]] bool ReadBoolean(const nlohmann::json& document, std::string_view key,
			bool& output, std::string& error)
		{
			const auto entry = document.find(std::string(key));
			if(entry == document.end()) {
				return true;
			}
			if(!entry->is_boolean()) {
				error = "Option '" + std::string(key) + "' must be boolean.";
				return false;
			}
			output = entry->get<bool>();
			return true;
		}

		[[nodiscard]] std::optional<WorkloadOptions> ParseOptions(const nlohmann::json& document,
			std::string& error)
		{
			if(!document.is_object()) {
				error = "Navigation application validation options must be an object.";
				return std::nullopt;
			}

			WorkloadOptions options{};
			if(const auto managedProbe = document.find("managedOutcomeProbe"); managedProbe != document.end()) {
				if(!managedProbe->is_string()) {
					error = "Option 'managedOutcomeProbe' must be 'partial' or 'budget'.";
					return std::nullopt;
				}
				const std::string& value = managedProbe->get_ref<const std::string&>();
				if(value == "partial") {
					options.managedOutcomeProbe = ManagedOutcomeProbe::Partial;
				}
				else if(value == "budget") {
					options.managedOutcomeProbe = ManagedOutcomeProbe::Budget;
				}
				else {
					error = "Option 'managedOutcomeProbe' must be 'partial' or 'budget'.";
					return std::nullopt;
				}
			}
			const std::string cohort = document.value("cohort", std::string{"agent"});
			if(cohort == "direct") {
				options.cohort = Cohort::Direct;
			}
			else if(cohort != "agent") {
				error = "Option 'cohort' must be 'direct' or 'agent'.";
				return std::nullopt;
			}

			const std::string executionMode = document.value("executionMode", std::string{"export"});
			if(executionMode == "export") {
				options.executionMode = ExecutionMode::Export;
			}
			else if(executionMode == "editor-play") {
				options.executionMode = ExecutionMode::EditorPlay;
			}
			else {
				error = "Option 'executionMode' must be 'export' or 'editor-play'.";
				return std::nullopt;
			}
			const std::string scenario = document.value("scenario", std::string{"workload"});
			if(scenario == "workload") {
				options.scenario = ValidationScenario::Workload;
			}
			else if(scenario == "clearance-parity") {
				options.scenario = ValidationScenario::ClearanceParity;
				const auto clearanceProfile = document.find("clearanceProfile");
				if(clearanceProfile == document.end() || !clearanceProfile->is_string()) {
					error = "Clearance-parity validation requires clearanceProfile 'small' or 'large'.";
					return std::nullopt;
				}
				if(clearanceProfile->get_ref<const std::string&>() == "small") {
					options.clearanceProfile = ClearanceProfileSelection::Small;
				}
				else if(clearanceProfile->get_ref<const std::string&>() == "large") {
					options.clearanceProfile = ClearanceProfileSelection::Large;
				}
				else {
					error = "Option 'clearanceProfile' must be 'small' or 'large'.";
					return std::nullopt;
				}
			}
			else {
				error = "Option 'scenario' must be 'workload' or 'clearance-parity'.";
				return std::nullopt;
			}

			if(!ReadUnsigned(document, "warmupSeconds", options.warmupSeconds, error)
				|| !ReadUnsigned(document, "measurementSeconds", options.measurementSeconds, error)
				|| !ReadUnsigned(document, "requiredWorkerCount", options.requiredWorkerCount, error)
				|| !ReadUnsigned(document, "agentPopulation", options.population, error)
				|| !ReadUnsigned(document, "changesPerSecond", options.changesPerSecond, error)
				|| !ReadUnsigned(document, "frameBudget", options.frameBudget, error)
				|| !ReadUnsigned(document, "timeoutSeconds", options.timeoutSeconds, error)
				|| !ReadBoolean(document, "requireRenderedPresentation", options.requireRenderedPresentation, error)
				|| !ReadBoolean(document, "requireRenderedProof", options.requireRenderedPresentation, error)
				|| !ReadBoolean(document, "headlessCpuFallback", options.headlessCpuFallback, error)
				|| !ReadBoolean(document, "qualification", options.qualificationRequested, error)) {
				return std::nullopt;
			}
			// Keep the migration input accepted while callers move to agentPopulation.
			if(!document.contains("agentPopulation")
				&& !ReadUnsigned(document, "population", options.population, error)) {
				return std::nullopt;
			}

			const auto manifest = document.find("fixtureManifestPath");
			if(manifest == document.end() || !manifest->is_string() || manifest->get_ref<const std::string&>().empty()) {
				error = "Option 'fixtureManifestPath' must name the installed AgentWorkload manifest.";
				return std::nullopt;
			}
			options.fixtureManifestPath = manifest->get<std::string>();
			if(!std::filesystem::is_regular_file(options.fixtureManifestPath)) {
				error = "The supplied AgentWorkload manifest does not exist.";
				return std::nullopt;
			}

			if(const auto cacheOutput = document.find("cacheOutputPath"); cacheOutput != document.end()) {
				if(!cacheOutput->is_string() || cacheOutput->get_ref<const std::string&>().empty()) {
					error = "Option 'cacheOutputPath' must be a non-empty absolute output path.";
					return std::nullopt;
				}
				options.cacheOutputPath = cacheOutput->get<std::string>();
				if(!options.cacheOutputPath.is_absolute()) {
					error = "Option 'cacheOutputPath' must be absolute so installed validation cannot write outside its declared artifact.";
					return std::nullopt;
				}
			}
			if(const auto staleOutput = document.find("cacheStaleOutputPath"); staleOutput != document.end()) {
				if(!staleOutput->is_string() || staleOutput->get_ref<const std::string&>().empty()) {
					error = "Option 'cacheStaleOutputPath' must be a non-empty absolute output path.";
					return std::nullopt;
				}
				options.cacheStaleOutputPath = staleOutput->get<std::string>();
				if(!options.cacheStaleOutputPath.is_absolute()) {
					error = "Option 'cacheStaleOutputPath' must be absolute so installed validation cannot write outside its declared artifact.";
					return std::nullopt;
				}
			}
			if(options.cacheOutputPath.empty() != options.cacheStaleOutputPath.empty()) {
				error = "cacheOutputPath and cacheStaleOutputPath must be supplied together.";
				return std::nullopt;
			}
			if(const auto expectedCacheMode = document.find("expectedCacheMode"); expectedCacheMode != document.end()) {
				if(!expectedCacheMode->is_string()) {
					error = "Option 'expectedCacheMode' must be 'absent', 'current', 'stale', or 'malformed'.";
					return std::nullopt;
				}
				const std::string mode = expectedCacheMode->get<std::string>();
				if(mode == "absent") options.expectedCacheMode = CacheExpectation::Absent;
				else if(mode == "current") options.expectedCacheMode = CacheExpectation::Current;
				else if(mode == "stale") options.expectedCacheMode = CacheExpectation::Stale;
				else if(mode == "malformed") options.expectedCacheMode = CacheExpectation::Malformed;
				else {
					error = "Option 'expectedCacheMode' must be 'absent', 'current', 'stale', or 'malformed'.";
					return std::nullopt;
				}
			}
			try {
				std::ifstream manifestInput(options.fixtureManifestPath);
				const nlohmann::json manifestDocument = nlohmann::json::parse(manifestInput);
				const bool validFixture = manifestDocument.value("schemaVersion", 0U) == 1U
					&& manifestDocument.value("fixtureId", std::string{}) == "navigation-agent-workload-v2"
					&& manifestDocument["generator"].value("seed", 0U) == 59001U
					&& manifestDocument["generator"].value("endpointPairCount", 0U) == 1000U
					&& manifestDocument["cohorts"]["agent"].value("retainedAgents", 0U) == 1000U
					&& manifestDocument["cohorts"].value("destinationChangesPerSecond", 0U) == 100U;
				if(!validFixture) {
					error = "The supplied manifest is not the pinned navigation-agent-workload-v2 contract.";
					return std::nullopt;
				}
			}
			catch(const std::exception&) {
				error = "The supplied AgentWorkload manifest could not be parsed.";
				return std::nullopt;
			}

			if(options.population == 0U || options.population > kMaximumPopulation
				|| options.changesPerSecond == 0U || options.changesPerSecond > kMaximumChangesPerSecond
				|| options.warmupSeconds > kMaximumDurationSeconds
				|| options.measurementSeconds == 0U || options.measurementSeconds > kMaximumDurationSeconds
				|| options.frameBudget > kMaximumFrameBudget
				|| options.requiredWorkerCount == 0U) {
				error = "Population, worker count, change rate, and measurement duration are outside bounded validation limits.";
				return std::nullopt;
			}
			const std::uint64_t requiredWallSeconds = static_cast<std::uint64_t>(options.warmupSeconds)
				+ static_cast<std::uint64_t>(options.measurementSeconds)
				+ static_cast<std::uint64_t>(kMeasurementDrainSeconds);
			if(options.timeoutSeconds != 0U
				&& static_cast<std::uint64_t>(options.timeoutSeconds) < requiredWallSeconds) {
				error = "timeoutSeconds must cover the complete real warmup and measurement duration.";
				return std::nullopt;
			}
			if(options.timeoutSeconds == 0U) {
				// A retained route that never becomes usable must not turn the default
				// export invocation into an unbounded host loop. The grace period leaves
				// ordinary terrain/bootstrap work time to settle after warmup.
				options.timeoutSeconds = static_cast<std::uint32_t>(requiredWallSeconds + 60U);
			}
			return options;
		}

		[[nodiscard]] std::uint64_t SplitMix64(std::uint64_t value) noexcept
		{
			value += 0x9e3779b97f4a7c15ULL;
			value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
			value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
			return value ^ (value >> 31U);
		}

		[[nodiscard]] glm::dvec2 PerimeterEndpoint(std::uint64_t value)
		{
			constexpr std::uint32_t gridResolution = 64U;
			constexpr std::uint32_t perimeterSlots = 4U * (gridResolution - 1U);
			const std::uint32_t slot = static_cast<std::uint32_t>(value % perimeterSlots);
			std::uint32_t x = 0U;
			std::uint32_t z = 0U;
			if(slot < gridResolution) {
				x = slot;
			}
			else if(slot < gridResolution + (gridResolution - 1U)) {
				x = gridResolution - 1U;
				z = slot - gridResolution + 1U;
			}
			else if(slot < gridResolution + (gridResolution - 1U) * 2U) {
				x = (gridResolution - 1U) - (slot - (gridResolution + (gridResolution - 1U)) + 1U);
				z = gridResolution - 1U;
			}
			else {
				z = (gridResolution - 1U) - (slot - (gridResolution + (gridResolution - 1U) * 2U) + 1U);
			}
			// Keep all 64 perimeter samples inside the fixture's two-metre terrain
			// margin while retaining its pinned 252-slot ordering and splitmix seed.
			constexpr double marginSafeExtent = 124.0;
			constexpr double gridIntervals = static_cast<double>(gridResolution - 1U);
			return {-62.0 + marginSafeExtent * static_cast<double>(x) / gridIntervals,
				-62.0 + marginSafeExtent * static_cast<double>(z) / gridIntervals};
		}

		[[nodiscard]] NavigationLocation Endpoint(NavigationWorldHandle world, std::uint64_t pairIndex)
		{
			constexpr std::uint64_t seed = 59001U;
			constexpr std::uint64_t perimeterSlots = 252U;
			const std::uint64_t start = SplitMix64(seed + pairIndex * 2U) % perimeterSlots;
			const std::uint64_t offset = 1U + SplitMix64(seed + pairIndex * 2U + 1U) % (perimeterSlots - 1U);
			const glm::dvec2 point = PerimeterEndpoint((start + offset) % perimeterSlots);
			NavigationLocation endpoint{};
			endpoint.world = world;
			endpoint.position.offsetMetres = {point.x, 0.0, point.y};
			return endpoint;
		}

		[[nodiscard]] bool IsPinnedFixtureProfile(const NavigationProfile& profile) noexcept
		{
			// The manifest fixes the controller-compatible workload profile. A
			// different project default can build a different clearance world and
			// would make lane evidence incomparable.
			return profile.radiusMetres == 0.5 && profile.heightMetres == 2.0
				&& profile.stepHeightMetres == 0.4 && profile.maxSlopeDegrees == 45.0
				&& profile.clearanceMarginMetres == 0.1;
		}

		[[nodiscard]] NavigationProfile PinnedFixtureProfile() noexcept
		{
			NavigationProfile profile = NavigationProfile::Defaults();
			profile.radiusMetres = 0.5;
			profile.heightMetres = 2.0;
			profile.stepHeightMetres = 0.4;
			profile.maxSlopeDegrees = 45.0;
			profile.clearanceMarginMetres = 0.1;
			return profile;
		}

		[[nodiscard]] bool HasReadyCoverageForProfile(const NavigationProfile& profile)
		{
			const std::shared_ptr<const NavigationWorldRoot> world = NavigationSystem::Instance().PublishedWorld();
			if(!world || world->terrainState != NavigationTerrainState::Available
				|| world->terrainPublicationGeneration == 0U) {
				return false;
			}
			const std::uint64_t profileKey = MakeGroundProfileBuildKey(profile);
			return std::any_of(world->coverage.begin(), world->coverage.end(),
				[profileKey](const NavigationCoverageRecord& coverage) {
					return coverage.key.profileBuildKey == profileKey
						&& coverage.state == NavigationCoverageState::Ready
						&& coverage.complete && coverage.coverageGeneration != 0U;
				});
		}

		[[nodiscard]] NavigationProfile ClearanceFixtureProfile(ClearanceProfileSelection selection) noexcept
		{
			NavigationProfile profile = NavigationProfile::Defaults();
			// Keep the ordinary default resolution/margin values. The fixture's 4m
			// opening is intentionally generous, leaving the 1.6m roof as the only
			// semantic difference between the 1.2m and 2.1m capsules.
			profile.radiusMetres = selection == ClearanceProfileSelection::Small ? 0.25 : 0.55;
			profile.heightMetres = selection == ClearanceProfileSelection::Small ? 1.20 : 2.10;
			return profile;
		}

		[[nodiscard]] NavigationProfile SelectedFixtureProfile(const WorkloadOptions& options) noexcept
		{
			return options.scenario == ValidationScenario::ClearanceParity
				? ClearanceFixtureProfile(options.clearanceProfile) : PinnedFixtureProfile();
		}

		[[nodiscard]] std::string_view ClearanceProfileName(ClearanceProfileSelection selection) noexcept
		{
			return selection == ClearanceProfileSelection::Small ? "small" : "large";
		}

		void EnsurePinnedFixtureProfileRegistration(Session& session)
		{
			if(session.workloadProfileRegistered) {
				return;
			}
			NavigationSystem& navigation = NavigationSystem::Instance();
			const NavigationProfileResolver current = navigation.ProfileResolverSnapshot();
			const NavigationProfile pinned = SelectedFixtureProfile(session.options);
			std::vector<NavigationProfileRegistration> registrations = current.Registrations();
			const bool alreadyRegistered = std::any_of(registrations.begin(), registrations.end(),
				[&pinned](const NavigationProfileRegistration& registration) {
					return registration.profile == pinned;
				});
			if(!alreadyRegistered) {
				// The workload routes use an explicit controller-compatible profile. Add
				// the same value to the copied bootstrap catalog before the first engine
				// frame, so current cache reuse happens before warmup submissions.
				constexpr UUID kWorkloadProfileId{
					0x4EU, 0x41U, 0x56U, 0x57U, 0x4FU, 0x52U, 0x4BU, 0x4CU,
					0x4FU, 0x41U, 0x44U, 0x50U, 0x52U, 0x4FU, 0x46U, 0x01U};
				constexpr UUID kClearanceSmallProfileId{
					0x4EU, 0x41U, 0x56U, 0x43U, 0x4CU, 0x45U, 0x41U, 0x52U,
					0x53U, 0x4DU, 0x41U, 0x4CU, 0x4CU, 0x01U, 0x00U, 0x01U};
				constexpr UUID kClearanceLargeProfileId{
					0x4EU, 0x41U, 0x56U, 0x43U, 0x4CU, 0x45U, 0x41U, 0x52U,
					0x4CU, 0x41U, 0x52U, 0x47U, 0x45U, 0x01U, 0x00U, 0x01U};
				const UUID profileId = session.options.scenario == ValidationScenario::Workload
					? kWorkloadProfileId : session.options.clearanceProfile == ClearanceProfileSelection::Small
					? kClearanceSmallProfileId : kClearanceLargeProfileId;
				registrations.push_back({profileId, pinned});
				navigation.ConfigureProfileRegistrations(current.DefaultProfile(), std::move(registrations));
			}
			session.workloadProfileRegistered = true;
		}

		constexpr std::size_t kMaximumLatencySamples = 65536U;

		void FinalizeLatency(Session& session, PendingLatency& pending,
			LatencyDisposition disposition, bool readyForGuidance)
		{
			if(!pending.active) {
				return;
			}
			pending.active = false;
			pending.disposition = disposition;
			if(!pending.measurement) {
				return;
			}

			// Each measurement submission reaches exactly one disposition. Rejected
			// pre-admission commands stay in this accounting but never affect the
			// admitted-command percentile below.
			++session.counters.measurementFinalized;
			switch(disposition) {
			case LatencyDisposition::Rejected: ++session.counters.measurementRejected; break;
			case LatencyDisposition::Expired: ++session.counters.measurementExpired; break;
			case LatencyDisposition::Cancelled: ++session.counters.measurementCancelled; break;
			case LatencyDisposition::Superseded: ++session.counters.measurementSuperseded; break;
			case LatencyDisposition::TimedOut: ++session.counters.measurementTimedOut; break;
			default: break;
			}
			if(!pending.admissionApplied) {
				return;
			}

			++session.counters.admittedFinalized;
			++session.counters.completed;
			// Qualification runs retain at most 60,000 admitted samples. A longer
			// smoke run fails honestly if it exceeds this bounded evidence buffer.
			if(session.counters.latencyMilliseconds.size() >= kMaximumLatencySamples) {
				session.ledgerCapacityExceeded = true;
				return;
			}
			const double milliseconds = std::chrono::duration<double, std::milli>(
				Clock::now() - pending.submittedAt).count();
			session.counters.latencyMilliseconds.push_back(milliseconds);
			if(readyForGuidance && session.counters.readyLatencyMilliseconds.size() < kMaximumLatencySamples) {
				session.counters.readyLatencyMilliseconds.push_back(milliseconds);
			}
			if(pending.readyCoverageAtAdmission && pending.admittedAt != Clock::time_point{}
				&& HasReadyCoverageForProfile(pending.profile)
				&& session.counters.readyCoverageLatencyMilliseconds.size() < kMaximumLatencySamples) {
				const double admittedMilliseconds = std::chrono::duration<double, std::milli>(
					Clock::now() - pending.admittedAt).count();
				session.counters.readyCoverageLatencyMilliseconds.push_back(admittedMilliseconds);
			}
		}

		[[nodiscard]] PendingLatency* FindActiveLatency(Session& session, std::uint32_t ownerIndex)
		{
			for(auto iterator = session.pendingLatency.rbegin(); iterator != session.pendingLatency.rend(); ++iterator) {
				if(iterator->active && iterator->ownerIndex == ownerIndex) {
					return &*iterator;
				}
			}
			return nullptr;
		}

		[[nodiscard]] PendingLatency* AddLatency(Session& session, PendingLatency pending)
		{
			// Retain only active work and its bounded terminal audit records. The
			// qualification budget is 61,000 total submissions, below this cap.
			if(session.pendingLatency.size() >= kMaximumLatencySamples) {
				session.ledgerCapacityExceeded = true;
				return nullptr;
			}
			session.pendingLatency.push_back(std::move(pending));
			return &session.pendingLatency.back();
		}

		[[nodiscard]] double Percentile(std::vector<double> values, double fraction)
		{
			if(values.empty()) {
				return 0.0;
			}
			std::sort(values.begin(), values.end());
			const std::size_t index = static_cast<std::size_t>(fraction
				* static_cast<double>(values.size() - 1U));
			return values[index];
		}

		[[nodiscard]] bool IsTerminal(NavigationOutcome outcome) noexcept
		{
			// New terminal outcomes must fail closed by default. Only documented work
			// stages remain nonterminal, so Partial/Unreachable/BudgetExhausted and
			// future terminal values cannot be counted repeatedly as pending work.
			return outcome != NavigationOutcome::Queued
				&& outcome != NavigationOutcome::PendingPreparation
				&& outcome != NavigationOutcome::Searching;
		}

		void ObserveRenderedPresentation(Engine& engine, Session& session)
		{
			if(!session.options.requireRenderedPresentation || session.renderedPresentationObserved) {
				return;
			}
			RenderSystem* renderer = engine.GetSystem<RenderSystem>();
			if(renderer == nullptr) {
				return;
			}
			// A nonzero completed publication frame is evidence that the application's
			// real render system published a Game view, not merely that a renderer was
			// registered in the scheduler.
			const bool rendererPublished = renderer->GetLatestRenderStats(
				RenderViewType::Game).publicationFrameId != 0U;
			// A completed Game publication is an immutable RenderSystem result from
			// the active application frame. Unlike a caller-supplied lane label, it
			// cannot be produced by a headless scheduler without presentation.
			session.renderedPresentationObserved = rendererPublished;
		}

		[[nodiscard]] bool CaptureFixtureBindings(Session& session, std::string& error)
		{
			NavigationSystem& navigation = NavigationSystem::Instance();
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			const auto collision = TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().Snapshot();
			session.fixtureCollisionStatus = static_cast<std::uint32_t>(collision.status);
			session.fixtureCollisionPendingCandidates = collision.mountedPendingCandidateCount;
			session.fixtureCollisionCandidatesStaged = collision.mountedCandidatesStaged;
			session.fixtureCollisionPublicationAttempts = collision.mountedPublicationAttempts;
			session.fixtureCollisionPublicationCommits = collision.mountedPublicationCommits;
			session.fixtureCollisionPublicationRejections = collision.mountedPublicationRejections;
			session.fixtureCollisionMessage = collision.message;
			const std::shared_ptr<const NavigationWorldRoot> world = navigation.PublishedWorld();
			if(world == nullptr) {
				session.fixtureBindingWaitReason = "NavigationSystem has not published a world root."
					" collisionStatus=" + std::to_string(session.fixtureCollisionStatus)
					+ " staged=" + std::to_string(session.fixtureCollisionCandidatesStaged)
					+ " pending=" + std::to_string(session.fixtureCollisionPendingCandidates)
					+ " attempts=" + std::to_string(session.fixtureCollisionPublicationAttempts)
					+ " commits=" + std::to_string(session.fixtureCollisionPublicationCommits)
					+ " rejections=" + std::to_string(session.fixtureCollisionPublicationRejections) + ".";
				session.fixtureRootTerrainState = 0U;
				session.fixtureRootTerrainPublicationGeneration = 0U;
				session.fixtureRootCoverageCount = 0U;
				session.fixtureRootSourceCount = 0U;
				return false;
			}
			session.fixtureRootTerrainState = static_cast<std::uint32_t>(world->terrainState);
			session.fixtureRootTerrainPublicationGeneration = world->terrainPublicationGeneration;
			session.fixtureRootCoverageCount = world->coverage.size();
			session.fixtureRootSourceCount = world->sources.size();
			if(world->coverage.empty()) {
				session.fixtureBindingWaitReason = std::string("Navigation coverage is not published yet."
					" terrainState=") + std::to_string(session.fixtureRootTerrainState)
					+ " terrainPublicationGeneration="
					+ std::to_string(session.fixtureRootTerrainPublicationGeneration)
					+ " sourceCount=" + std::to_string(session.fixtureRootSourceCount) + ".";
				return false;
			}
			if(world->terrainState != NavigationTerrainState::Available
				|| world->terrainPublicationGeneration == 0U) {
				error = "The workload fixture has coverage records without an available published terrain key.";
				return false;
			}
			ObjectManager& objects = ObjectManager::instance();
			session.ownerIds.clear();
			session.agentIdentities.clear();
			session.initialOwnerPositions.clear();
			session.pendingLatency.clear();
			session.currentDirectRequests.clear();
			session.retiredDirectRequests.clear();
			session.ownerIds.reserve(session.options.population);
			session.agentIdentities.reserve(session.options.population);
			session.initialOwnerPositions.reserve(session.options.population);
			for(std::uint32_t index = 0U; index < session.options.population; ++index) {
				char name[32]{};
				std::snprintf(name, sizeof(name), "NAV_OWNER_%04u", index);
				const std::optional<Object> owner = objects.FindObjectByName(name);
				if(!owner) {
					error = "The serialized fixture is missing owner '" + std::string(name) + "'.";
					return false;
				}
				const UUID objectId = objects.getUUID(*owner);
				const NavigationAgentComponent* component = objects.getRegistry().try_get<NavigationAgentComponent>(owner->EntityId());
				if(session.options.cohort == Cohort::Agent) {
					if(component == nullptr || !component->active || !IsPinnedFixtureProfile(component->ToNavigationProfile())) {
						error = "The serialized agent fixture owner is missing the pinned NavigationAgentComponent profile.";
						return false;
					}
				}
				else if(component != nullptr) {
					// The direct cohort deliberately exercises the established component-free
					// request API. Accidentally serializing an agent here invalidates that lane.
					error = "The serialized direct fixture owner unexpectedly has NavigationAgentComponent.";
					return false;
				}
				const std::optional<Scene::WorldPose> pose = objects.GetCanonicalWorldPose(*owner);
				if(!pose) {
					error = "The serialized fixture owner has no canonical Transform pose.";
					return false;
				}
				session.ownerIds.push_back(objectId);
				session.initialOwnerPositions.push_back(pose->position);
				if(session.options.cohort == Cohort::Agent) {
					const std::optional<NavigationAgentIdentity> identity = agents.ResolvePublishedAgent(objectId);
					if(!identity || !identity->IsValid()) {
						session.fixtureBindingWaitReason = "NavigationAgentSystem has not published the complete agent binding map yet.";
						return false;
					}
					session.agentIdentities.push_back(*identity);
				}
			}
			const std::optional<Object> blocker = objects.FindObjectByName("NAV_BLOCKER");
			if(!blocker) {
				error = "The serialized fixture is missing the required NAV_BLOCKER.";
				return false;
			}
			std::vector<UUID> expectedStaticSourceOwners;
			expectedStaticSourceOwners.reserve(65U);
			for(std::uint32_t index = 0U; index < 64U; ++index) {
				char name[32]{};
				std::snprintf(name, sizeof(name), "NAV_BOX_%02u", index);
				const std::optional<Object> box = objects.FindObjectByName(name);
				if(!box || !objects.isObjectStatic(*box)) {
					error = "The serialized fixture is missing static navigation box '" + std::string(name) + "'.";
					return false;
				}
				expectedStaticSourceOwners.push_back(objects.getUUID(*box));
			}
			if(!objects.isObjectStatic(*blocker)) {
				error = "NAV_BLOCKER must retain static navigation-source metadata while moving kinematically.";
				return false;
			}
			expectedStaticSourceOwners.push_back(objects.getUUID(*blocker));
			const std::size_t eligibleStaticSources = static_cast<std::size_t>(std::count_if(
				world->sources.begin(), world->sources.end(), [&expectedStaticSourceOwners](const NavigationSourceIndexRecord& source) {
					return source.enabled && source.identity.kind == NavigationSourceKind::Collider
						&& source.revisionEvidence.IsComplete()
						&& std::find(expectedStaticSourceOwners.begin(), expectedStaticSourceOwners.end(),
							source.identity.objectId) != expectedStaticSourceOwners.end();
				}));
			if(eligibleStaticSources != expectedStaticSourceOwners.size()) {
				error = "The published navigation source index does not contain all 64 static boxes and NAV_BLOCKER.";
				return false;
			}
			session.eligibleStaticSourceCount = static_cast<std::uint32_t>(eligibleStaticSources);
			session.eligibleStaticSourcesObserved = true;
			session.fixtureBindingsObserved = true;
			session.fixtureBindingWaitReason.clear();
			session.currentDirectRequests.assign(session.options.population, NavigationRequestHandle{});
			return true;
		}

		[[nodiscard]] bool CaptureClearanceFixtureBindings(Session& session, std::string& error)
		{
			NavigationSystem& navigation = NavigationSystem::Instance();
			const std::shared_ptr<const NavigationWorldRoot> world = navigation.PublishedWorld();
			if(world == nullptr) {
				session.fixtureBindingWaitReason = "NavigationSystem has not published a clearance world root.";
				return false;
			}
			session.fixtureRootTerrainState = static_cast<std::uint32_t>(world->terrainState);
			session.fixtureRootTerrainPublicationGeneration = world->terrainPublicationGeneration;
			session.fixtureRootCoverageCount = world->coverage.size();
			session.fixtureRootSourceCount = world->sources.size();
			const std::uint64_t profileKey = MakeGroundProfileBuildKey(SelectedFixtureProfile(session.options));
			const bool profileCoverageReady = std::any_of(world->coverage.begin(), world->coverage.end(),
				[profileKey](const NavigationCoverageRecord& coverage) {
					return coverage.key.profileBuildKey == profileKey
						&& coverage.state == NavigationCoverageState::Ready && coverage.complete
						&& coverage.coverageGeneration != 0U;
				});
			if(world->terrainState != NavigationTerrainState::Available
				|| world->terrainPublicationGeneration == 0U || !profileCoverageReady) {
				session.fixtureBindingWaitReason = "Clearance fixture is waiting for its selected profile coverage.";
				return false;
			}

			ObjectManager& objects = ObjectManager::instance();
			constexpr std::array<std::string_view, 3U> kRequiredColliders{
				"NAV_CLEARANCE_LOWER_JAMB", "NAV_CLEARANCE_UPPER_JAMB", "NAV_CLEARANCE_ROOF"};
			std::array<UUID, kRequiredColliders.size()> owners{};
			for(std::size_t index = 0U; index < kRequiredColliders.size(); ++index) {
				const std::optional<Object> object = objects.FindObjectByName(kRequiredColliders[index]);
				if(!object || !objects.isObjectStatic(*object)) {
					error = "The clearance fixture is missing static collider '"
						+ std::string(kRequiredColliders[index]) + "'.";
					return false;
				}
				owners[index] = objects.getUUID(*object);
			}
			const std::size_t eligibleSources = static_cast<std::size_t>(std::count_if(
				world->sources.begin(), world->sources.end(), [&owners](const NavigationSourceIndexRecord& source) {
					return source.enabled && source.identity.kind == NavigationSourceKind::Collider
						&& source.revisionEvidence.IsComplete()
						&& std::find(owners.begin(), owners.end(), source.identity.objectId) != owners.end();
				}));
			if(eligibleSources != owners.size()) {
				error = "The clearance fixture did not publish three distinct static collider sources.";
				return false;
			}
			session.eligibleStaticSourcesObserved = true;
			session.eligibleStaticSourceCount = static_cast<std::uint32_t>(eligibleSources);
			if(world->eligibleSourceIndex) {
				for(const auto& source : world->eligibleSourceIndex->Records()) {
					if(session.clearanceSourceGeometry.size() >= 16U) break;
					if(!source.geometry) continue;
					const auto& origin = source.geometry->Descriptor().canonicalFrameOrigin;
					const auto& bounds = source.geometry->Bounds();
					session.clearanceSourceGeometry.push_back({
						{"source", ToHexString(source.identity.sourceId)},
						{"object", ToHexString(source.identity.objectId)},
						{"kind", static_cast<std::uint32_t>(source.identity.kind)},
						{"originCell", {origin.cell.x, origin.cell.y, origin.cell.z}},
						{"originOffset", {origin.offsetMetres.x, origin.offsetMetres.y, origin.offsetMetres.z}},
						{"localMinimum", {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z}},
						{"localMaximum", {bounds.maximum.x, bounds.maximum.y, bounds.maximum.z}}});
				}
			}
			session.fixtureBindingsObserved = true;
			session.fixtureBindingWaitReason.clear();
			return true;
		}

		[[nodiscard]] bool SubmitClearanceRoute(Session& session, std::string& error)
		{
			if(session.clearanceRequestSubmitted) {
				return true;
			}
			const std::shared_ptr<const NavigationWorldRoot> world = NavigationSystem::Instance().PublishedWorld();
			if(world == nullptr) {
				error = "Clearance fixture lost its published world before route submission.";
				return false;
			}
			Ground::GroundRouteQuery::Request request{};
			request.owner = {world->world, NavigationOwnerId{0xC1EAU}, 1U};
			request.start.world = world->world;
			request.start.position.offsetMetres = {-24.0, 0.0, 0.0};
			request.destination.world = world->world;
			request.destination.position.offsetMetres = {24.0, 0.0, 0.0};
			request.profile = SelectedFixtureProfile(session.options);
			session.clearanceRequest = NavigationSystem::Instance().SubmitOneShotRoute(std::move(request));
			if(!session.clearanceRequest.IsValid()) {
				error = "Clearance fixture direct route was rejected before admission.";
				return false;
			}
			session.clearanceRequestSubmitted = true;
			++session.counters.submitted;
			++session.counters.accepted;
			return true;
		}

		void ObserveClearanceRoute(Session& session)
		{
			if(!session.clearanceRequestSubmitted || session.clearanceRequestTerminal) {
				return;
			}
			const NavigationStoreReadResult<NavigationRequestRecord> read =
				NavigationSystem::Instance().ReadRequest(session.clearanceRequest);
			if(read.record && IsTerminal(read.record->outcome)) {
				session.clearanceOutcome = read.record->outcome;
				session.clearanceReason = read.record->reason;
				const auto route = NavigationSystem::Instance().ReadRoute(
					NavigationSystem::Instance().PublishedRouteForRequest(session.clearanceRequest));
				session.clearanceRoutePointCount = route.points.size();
				for(const auto& point : route.points) {
					if(session.clearanceRoutePoints.size() >= 256U) break;
					session.clearanceRoutePoints.push_back({
						{"cell", {point.position.cell.x, point.position.cell.y, point.position.cell.z}},
						{"offset", {point.position.offsetMetres.x, point.position.offsetMetres.y, point.position.offsetMetres.z}}});
				}
				session.clearanceRequestTerminal = true;
			}
		}

		void QueueMovingBlockerMutation(Session& session, Clock::time_point now)
		{
			const double elapsedSeconds = std::chrono::duration<double>(now - session.measurementStartedAt).count();
			const std::optional<Object> blocker = ObjectManager::instance().FindObjectByName("NAV_BLOCKER");
			if(!blocker) {
				return;
			}
			Scene::WorldPose pose{};
			pose.position.offsetMetres = {
				24.0 * std::sin(2.0 * std::numbers::pi * elapsedSeconds / 32.0), 0.0,
				24.0 * std::cos(2.0 * std::numbers::pi * elapsedSeconds / 32.0)};
			const std::string transactionId = "navigation-agent-workload-blocker-"
				+ std::to_string(static_cast<std::uint64_t>(elapsedSeconds));
			Scene::TransactionalMutationBuilder mutation(
				transactionId,
				Scene::TransactionOrigin::Test, "NavigationApplicationValidation");
			mutation.SetWorldTransform(ObjectManager::instance(), ObjectManager::instance().getUUID(*blocker), pose);
			Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(mutation).Build());
			++session.counters.movingBlockerMutationsQueued;
			session.counters.movingBlockerLastTransactionId = transactionId;
			session.pendingBlockerPosition = pose.position;
			session.counters.movingBlockerLastIntendedPose = pose.position;
		}

		void SubmitOneDestination(Session& session, std::uint32_t ownerIndex,
			std::uint64_t pairIndex, Clock::time_point now)
		{
			NavigationSystem& navigation = NavigationSystem::Instance();
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			const std::shared_ptr<const NavigationWorldRoot> world = navigation.PublishedWorld();
			if(world == nullptr || world->coverage.empty()) {
				return;
			}
			const NavigationLocation destination = Endpoint(world->world, pairIndex);
			++session.counters.submitted;
			const bool measurement = session.measurementStarted;
			if(measurement) {
				++session.counters.measurementSubmitted;
			}
			else {
				++session.counters.warmupSubmitted;
			}
			if(session.options.cohort == Cohort::Direct) {
				if(PendingLatency* replaced = FindActiveLatency(session, ownerIndex)) {
					// Replacement is an explicit terminal outcome for the previous
					// command, rather than an omitted sample from the admitted ledger.
					FinalizeLatency(session, *replaced, LatencyDisposition::Cancelled, false);
				}
				if(session.currentDirectRequests[ownerIndex].IsValid()) {
					// One route remains retained per direct owner. Explicitly release the
					// replaced result at this host boundary rather than accumulating every
					// 100 Hz historical request in the bounded request store.
					const NavigationRequestHandle replacedRequest = session.currentDirectRequests[ownerIndex];
					(void)navigation.CancelRequest(replacedRequest);
					if(!navigation.ReleaseRequestResult(replacedRequest)) {
						session.retiredDirectRequests.push_back(replacedRequest);
					}
					session.currentDirectRequests[ownerIndex] = {};
				}
				const NavigationSystem::ObjectRouteReceipt receipt = navigation.RequestRoute(
					ObjectManager::instance(), session.ownerIds[ownerIndex], destination,
					PinnedFixtureProfile(), NavigationRoutePolicy::NearestReachable,
					static_cast<std::uint64_t>(ownerIndex) + 1U);
				if(receipt.Succeeded()) {
					++session.counters.accepted;
					session.currentDirectRequests[ownerIndex] = receipt.request;
					if(measurement) {
						++session.counters.measurementAdmitted;
					}
					PendingLatency pending{};
					pending.submittedAt = now;
					pending.admittedAt = Clock::now();
					pending.directRequest = receipt.request;
					pending.profile = PinnedFixtureProfile();
					pending.ownerIndex = ownerIndex;
					pending.measurement = measurement;
					pending.admissionApplied = true;
					pending.readyCoverageAtAdmission = HasReadyCoverageForProfile(pending.profile);
					pending.active = true;
					(void)AddLatency(session, std::move(pending));
				}
				else {
					++session.counters.rejected;
					PendingLatency pending{};
					pending.submittedAt = now;
					pending.ownerIndex = ownerIndex;
					pending.measurement = measurement;
					pending.active = true;
					if(PendingLatency* recorded = AddLatency(session, std::move(pending))) {
						FinalizeLatency(session, *recorded, LatencyDisposition::Rejected, false);
					}
				}
				return;
			}

			const NavigationAgentIdentity identity = session.agentIdentities[ownerIndex];
			const NavigationAgentStatusSnapshot status = agents.ReadStatus(identity);
			const std::uint64_t correlation = ++session.nextCorrelationId;
			const NavigationAgentReceipt receipt = agents.QueueLocationDestination(identity, destination,
				status.configurationRevision, status.intentSequence, ++session.nextOrderingToken,
				correlation);
			if(receipt.state == NavigationAgentReceiptState::Rejected) {
				++session.counters.rejected;
				PendingLatency pending{};
				pending.submittedAt = now;
				pending.agent = identity;
				pending.correlationId = correlation;
				pending.ownerIndex = ownerIndex;
				pending.measurement = measurement;
				pending.active = true;
				if(PendingLatency* recorded = AddLatency(session, std::move(pending))) {
					FinalizeLatency(session, *recorded, LatencyDisposition::Rejected, false);
				}
				return;
			}
			if(receipt.state == NavigationAgentReceiptState::Superseded) {
				++session.counters.coalesced;
			}
			if(PendingLatency* replaced = FindActiveLatency(session, ownerIndex)) {
				// A later destination deliberately replaces this owner intent. Record
				// that terminal supersession rather than silently dropping it from the
				// raw outcome denominator.
				++session.counters.coalesced;
				FinalizeLatency(session, *replaced, LatencyDisposition::Superseded, false);
			}
			PendingLatency pending{};
			pending.submittedAt = now;
			pending.agent = identity;
			pending.profile = PinnedFixtureProfile();
			pending.requiredIntent = status.intentSequence + 1U;
			pending.correlationId = correlation;
			pending.ownerIndex = ownerIndex;
			pending.measurement = measurement;
			pending.active = true;
			(void)AddLatency(session, std::move(pending));
		}

		void SubmitDueChanges(Session& session, Clock::time_point now)
		{
			if(!session.measurementStarted || session.measurementDrainStarted
				|| session.options.changesPerSecond == 0U) {
				return;
			}
			const auto interval = std::chrono::duration_cast<Clock::duration>(
				std::chrono::duration<double>(1.0 / static_cast<double>(session.options.changesPerSecond)));
			// `AdvanceOneCompletedFrame` observes the real clock only after the normal
			// frame returns. A long frame can therefore cross the measurement deadline.
			// Bound catch-up by the scheduled deadline, rather than by that late wall
			// sample, so a five-second 100 Hz test contains exactly its intended 500
			// arrival slots. Already accepted work remains in the ledger and is drained
			// normally; this only prevents post-window commands from being mislabeled as
			// measurement traffic.
			const Clock::time_point measurementDeadline = session.measurementStartedAt
				+ std::chrono::duration_cast<Clock::duration>(
					std::chrono::seconds(session.options.measurementSeconds));
			while(now >= session.nextChangeAt && session.nextChangeAt < measurementDeadline) {
				const std::uint32_t owner = static_cast<std::uint32_t>(session.changeIndex % session.options.population);
				const std::uint64_t pair = (session.changeIndex / session.options.population + owner)
					% session.options.population;
				SubmitOneDestination(session, owner, pair, now);
				++session.changeIndex;
				session.nextChangeAt += interval;
			}
		}

		[[nodiscard]] bool HasActiveMeasurementLatency(const Session& session)
		{
			return std::any_of(session.pendingLatency.begin(), session.pendingLatency.end(),
				[](const PendingLatency& pending) { return pending.measurement && pending.active; });
		}

		void TimeoutOutstandingMeasurementLatency(Session& session)
		{
			for(PendingLatency& pending : session.pendingLatency) {
				if(pending.measurement && pending.active) {
					// Explicit censored outcomes are included in admitted latency. A route
					// that outlives the bounded drain therefore cannot disappear from p95.
					FinalizeLatency(session, pending, LatencyDisposition::TimedOut, false);
				}
			}
		}

		void RestoreSchedulerDiagnostics(Session& session)
		{
			if(!session.schedulerDiagnosticsRequested) {
				return;
			}
			Scheduling::SetEcsGraphFrameDiagnosticsRequested(session.schedulerDiagnosticsWereEnabled);
			session.schedulerDiagnosticsRequested = false;
		}

		[[nodiscard]] nlohmann::json DescribeWorkloadResult(
			const Scheduling::FrameWorkloadResult& result)
		{
			// Copy only declarative values. In particular, never serialize the callback
			// fields in FrameWorkloadDeclaration, which are not safe report evidence.
			return {
				{"descriptorId", result.declaration.stableDescriptorId},
				{"ownerSystem", result.declaration.ownerSystem},
				{"phase", result.declaration.phase},
				{"active", result.declaration.active},
				{"ready", result.declaration.ready},
				{"readinessReason", result.declaration.readinessReason},
				{"executionPolicy", std::string(Scheduling::ToString(result.executionPolicy))},
				{"omitted", result.omitted},
				{"completed", result.completed},
				{"explicitDependencies", result.declaration.explicitDependencies}};
		}

		[[nodiscard]] nlohmann::json DescribePlanDiagnostic(
			const Scheduling::ArchitectureDiagnostic& diagnostic)
		{
			return {
				{"severity", std::string(Scheduling::ToString(diagnostic.severity))},
				{"code", std::string(Scheduling::ToString(diagnostic.code))},
				{"workloadName", diagnostic.workloadName},
				{"resourceName", diagnostic.resourceName},
				{"message", diagnostic.message},
				{"recommendedAction", diagnostic.recommendedAction}};
		}

		void ObserveFramePlanDiagnostics(Engine& engine, Session& session)
		{
			const Scheduling::FrameExecutionPlan& plan = engine.GetParallelFrameExecutionPlan();
			bool frameOmittedWorkload = false;
			for(const Scheduling::FrameWorkloadResult& result : plan.results) {
				if(result.declaration.stableDescriptorId == "SystemScheduler/MutationFlush/Frame End") {
					session.frameEndWorkload = DescribeWorkloadResult(result);
				}
				if(!result.omitted) {
					continue;
				}
				frameOmittedWorkload = true;
				const nlohmann::json evidence = DescribeWorkloadResult(result);
				if(session.firstOmittedWorkload.is_null()) {
					session.firstOmittedWorkload = evidence;
				}
				session.lastOmittedWorkload = evidence;
			}
			if(frameOmittedWorkload) {
				++session.omittedWorkloadFrames;
			}
			if(!plan.diagnostics.empty()) {
				const nlohmann::json first = DescribePlanDiagnostic(plan.diagnostics.front());
				const nlohmann::json last = DescribePlanDiagnostic(plan.diagnostics.back());
				if(session.firstPlanDiagnostic.is_null()) {
					session.firstPlanDiagnostic = first;
				}
				session.lastPlanDiagnostic = last;
			}
		}

		void ObserveNavigationPrepareExecutionTiming(Engine& engine, Session& session)
		{
			const Scheduling::FrameExecutionPlan& plan = engine.GetParallelFrameExecutionPlan();
			constexpr std::string_view kPrepareDescriptor = "NavigationSystem/Navigation/Prepare";
			std::uint64_t newestEnd = session.lastObservedNavigationPrepareEndNanoseconds;
			bool observedFrame = false;
			for(const Profiling::WorkloadExecutionRecord& record : plan.executionRecords) {
				if(record.descriptorId != kPrepareDescriptor
					|| record.runDuration.availability != Profiling::TelemetryAvailability::Available
					|| record.runEndNanoseconds == 0U
					|| record.runEndNanoseconds <= session.lastObservedNavigationPrepareEndNanoseconds) {
					continue;
				}
				observedFrame = true;
				newestEnd = std::max(newestEnd, record.runEndNanoseconds);
				const auto saturatingAdd = [](std::uint64_t& total, std::uint64_t value) {
					total = value > std::numeric_limits<std::uint64_t>::max() - total
						? std::numeric_limits<std::uint64_t>::max() : total + value;
				};
				if(record.partitionIndex == 0U) {
					++session.counters.navigationPreparePartitionZeroCount;
					saturatingAdd(session.counters.navigationPreparePartitionZeroRunNanoseconds,
						record.runDuration.nanoseconds);
					session.counters.navigationPreparePartitionZeroMaximumNanoseconds = std::max(
						session.counters.navigationPreparePartitionZeroMaximumNanoseconds,
						record.runDuration.nanoseconds);
				}
				else {
					++session.counters.navigationPrepareRoutePartitionCount;
					saturatingAdd(session.counters.navigationPrepareRouteRunNanoseconds,
						record.runDuration.nanoseconds);
					saturatingAdd(session.counters.navigationPrepareRouteWorkItems,
						record.usefulWorkItemCount);
					session.counters.navigationPrepareRouteMaximumNanoseconds = std::max(
						session.counters.navigationPrepareRouteMaximumNanoseconds,
						record.runDuration.nanoseconds);
				}
			}
			if(observedFrame) {
				++session.counters.navigationPrepareObservedFrames;
				session.lastObservedNavigationPrepareEndNanoseconds = newestEnd;
			}
		}

		void SeedWarmupCohort(Session& session, Clock::time_point now)
		{
			if(session.warmupSeeded) {
				return;
			}
			// Admit one deterministic destination for every owner before timing begins.
			// Later measurement replacements therefore exercise a warmed, retained
			// route population instead of treating first-admission work as throughput.
			for(std::uint32_t owner = 0U; owner < session.options.population; ++owner) {
				SubmitOneDestination(session, owner, owner, now);
			}
			// Warmup establishes retained routes but is not part of the manifest's
			// measured `changeIndex` sequence. Measurement therefore starts at zero.
			session.changeIndex = 0U;
			session.warmupSeeded = true;
		}

		void ObserveCounters(Engine& engine, Session& session)
		{
			ObserveNavigationPrepareExecutionTiming(engine, session);
			NavigationSystem& navigation = NavigationSystem::Instance();
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			const NavigationSystem::StoreUsageSnapshot storeUsage = navigation.PublishedStoreUsage();
			if(const auto diagnostics = navigation.PublishedDiagnostics()) {
				// Preserve the first snapshot containing tuple evidence. This bounded
				// sample avoids relying on UI diagnostic records being mirrored to stdout.
				if(session.firstObservedTransitionUnknownDiagnostics.empty()
					&& diagnostics->preparedRouteTransitionUnknownCount != 0U) {
					for(const auto& record : diagnostics->records) {
						if(record.code != "navigation.prepared_route_transition_unknown") continue;
						session.firstObservedTransitionUnknownDiagnostics.push_back({
							{"publishedGeneration", record.publishedGeneration}, {"message", record.message}});
						if(session.firstObservedTransitionUnknownDiagnostics.size() >= 8U) break;
					}
				}
				session.routeTransitionCounters = {
					diagnostics->preparedRouteRootMismatchCount,
					diagnostics->preparedRouteTransitionRebasedCount,
					diagnostics->preparedRouteTransitionIntersectedCount,
					diagnostics->preparedRouteTransitionUnknownCount,
					diagnostics->preparedRouteTransitionInvalidInputCount,
					diagnostics->preparedRouteTransitionTerrainMismatchCount,
					diagnostics->preparedRouteTransitionMissingLinkCount,
					diagnostics->preparedRouteTransitionInvalidLinkCount,
					diagnostics->preparedRouteTransitionSourceChainMismatchCount,
					diagnostics->preparedRouteTransitionFinalSourceMismatchCount};
				session.asyncBuildLastObserved = {diagnostics->asyncBuildCurrentCount,
					diagnostics->asyncBuildRetiredCount, diagnostics->asyncBuildChargedBytes,
					diagnostics->asyncBuildPeakChargedBytes, diagnostics->asyncBuildAdmissionRejectedCount,
					diagnostics->asyncBuildCompletionCount, diagnostics->asyncBuildStaleDiscardCount};
				session.asyncBuildPeakOutstandingObserved = std::max(session.asyncBuildPeakOutstandingObserved,
					diagnostics->asyncBuildCurrentCount + diagnostics->asyncBuildRetiredCount);
				session.asyncTimingCounters = {
					diagnostics->asyncBuildGroundCompletionCount,
					diagnostics->asyncBuildDynamicCompletionCount,
					diagnostics->asyncBuildTotalWorkerNanoseconds,
					diagnostics->asyncBuildMaxWorkerNanoseconds,
					diagnostics->asyncBuildTotalQueueWaitNanoseconds,
					diagnostics->asyncBuildMaxQueueWaitNanoseconds,
					diagnostics->asyncBuildTotalPublicationWaitNanoseconds,
					diagnostics->asyncBuildMaxPublicationWaitNanoseconds};
				session.routeTimingCounters = {
					diagnostics->routeAdmissionToCaptureCount,
					diagnostics->routeAdmissionToCaptureNanoseconds,
					diagnostics->routeAdmissionToCaptureMaxNanoseconds,
					diagnostics->routeWorkerSearchCount,
					diagnostics->routeWorkerSearchNanoseconds,
					diagnostics->routeWorkerSearchMaxNanoseconds,
					diagnostics->routeWorkerToPublicationCount,
					diagnostics->routeWorkerToPublicationNanoseconds,
					diagnostics->routeWorkerToPublicationMaxNanoseconds,
					diagnostics->routeDeferredCoverageWaitCount,
					diagnostics->routeDeferredCoverageWaitNanoseconds,
					diagnostics->routeDeferredCoverageWaitMaxNanoseconds};
				session.tileReuseCounters = {diagnostics->dynamicTileReuseAttemptCount,
					diagnostics->dynamicTileReuseTileCount, diagnostics->dynamicTileRebuildTileCount,
					diagnostics->dynamicTileReuseFallbackCount};
				if(session.firstObservedTileReuseFallbacks.empty()) {
					for(const NavigationDiagnosticRecord& record : diagnostics->records) {
						if(record.code != "navigation.dynamic_tile_reuse_fallback") continue;
						session.firstObservedTileReuseFallbacks.push_back({
							{"publishedGeneration", record.publishedGeneration}, {"message", record.message}});
						if(session.firstObservedTileReuseFallbacks.size() >= 8U) break;
					}
				}
			}
			// This navigation-only estimate is intentionally separate from the
			// runner's OS process sampler. It attributes current store/work records
			// without inventing allocator-wide RSS ownership.
			session.counters.memoryHighWaterBytes = std::max<std::uint64_t>(
				session.counters.memoryHighWaterBytes, storeUsage.estimatedOwnedBytes);
			if(session.options.cohort == Cohort::Direct) {
				std::size_t outstanding = 0U;
				std::size_t retainedRoutes = 0U;
				for(const NavigationRequestHandle handle : session.currentDirectRequests) {
					const NavigationStoreReadResult<NavigationRequestRecord> request = navigation.ReadRequest(handle);
					if(!request.record) {
						continue;
					}
					const Ground::GroundRouteQuery::RetainedRoute route = navigation.ReadRoute(
						navigation.PublishedRouteForRequest(handle));
					if(route.IsValid() && request.record->outcome == NavigationOutcome::Complete) {
						++retainedRoutes;
					}
					if(!IsTerminal(request.record->outcome)) {
						++outstanding;
					}
				}
				for(PendingLatency& pending : session.pendingLatency) {
					if(!pending.active || !pending.directRequest.IsValid()) {
						continue;
					}
					const NavigationStoreReadResult<NavigationRequestRecord> request = navigation.ReadRequest(pending.directRequest);
					if(request.record && IsTerminal(request.record->outcome)) {
						FinalizeLatency(session, pending,
							request.record->outcome == NavigationOutcome::Complete
								? LatencyDisposition::Ready : LatencyDisposition::TerminalFailure,
							request.record->outcome == NavigationOutcome::Complete);
					}
				}
				session.counters.peakOutstanding = std::max<std::uint64_t>(session.counters.peakOutstanding, outstanding);
				session.counters.retainedPeak = std::max<std::uint64_t>(session.counters.retainedPeak,
					static_cast<std::uint64_t>(retainedRoutes));
			}
			else {
				const std::shared_ptr<const NavigationAgentDiagnosticsSnapshot> diagnostics = agents.PublishedDiagnostics();
				if(diagnostics) {
					session.counters.following = diagnostics->followingAgents;
					session.counters.arrived = diagnostics->arrivedAgents;
					session.counters.stopped = diagnostics->stoppedAgents;
					session.counters.failed = diagnostics->failedAgents;
					session.counters.retries = diagnostics->retryCount;
					session.counters.coalesced = std::max(session.counters.coalesced, diagnostics->coalescedCommandCount);
					session.counters.memoryHighWaterBytes = std::max<std::uint64_t>(
						session.counters.memoryHighWaterBytes,
						storeUsage.estimatedOwnedBytes + diagnostics->currentOwnedMemoryBytes);
					session.counters.peakOutstanding = std::max<std::uint64_t>(session.counters.peakOutstanding,
						diagnostics->requestingAgents + diagnostics->followingAgents);
					const std::size_t retainedGuidance = static_cast<std::size_t>(std::count_if(
						diagnostics->agents.begin(), diagnostics->agents.end(),
						[](const NavigationAgentDiagnosticRecord& record) {
							// A nearest-reachable Partial route is terminal evidence, but it
							// cannot satisfy the fixture's complete retained-route warmup.
							return record.guidance.IsUsable()
								&& record.guidance.outcome == NavigationOutcome::Complete;
						}));
					session.counters.retainedPeak = std::max<std::uint64_t>(session.counters.retainedPeak,
						static_cast<std::uint64_t>(retainedGuidance));
				}
				for(PendingLatency& pending : session.pendingLatency) {
					if(!pending.active) {
						continue;
					}
					const std::optional<NavigationAgentReceipt> receipt = agents.ReadReceipt(
						pending.agent, pending.correlationId);
					if(receipt && receipt->state == NavigationAgentReceiptState::Rejected) {
						++session.counters.rejected;
						FinalizeLatency(session, pending, LatencyDisposition::Rejected, false);
						continue;
					}
					if(receipt && receipt->state == NavigationAgentReceiptState::Superseded) {
						++session.counters.coalesced;
						FinalizeLatency(session, pending, LatencyDisposition::Superseded, false);
						continue;
					}
					if(receipt && receipt->state == NavigationAgentReceiptState::Expired) {
						++session.counters.expired;
						FinalizeLatency(session, pending, LatencyDisposition::Expired, false);
						continue;
					}
					if(receipt && receipt->state == NavigationAgentReceiptState::Applied && !pending.admissionApplied) {
						++session.counters.accepted;
						if(pending.measurement) {
							++session.counters.measurementAdmitted;
						}
						pending.admittedAt = Clock::now();
						pending.readyCoverageAtAdmission = HasReadyCoverageForProfile(pending.profile);
						pending.admissionApplied = true;
					}
					if(!pending.admissionApplied) {
						continue;
					}
					const NavigationAgentStatusSnapshot status = agents.ReadStatus(pending.agent);
					if(status.intentSequence >= pending.requiredIntent
						&& (status.state == NavigationAgentState::Following
							|| status.state == NavigationAgentState::Arrived
							|| status.state == NavigationAgentState::Failed
							|| status.state == NavigationAgentState::Stopped)) {
						const bool ready = status.state == NavigationAgentState::Following
							|| status.state == NavigationAgentState::Arrived;
						FinalizeLatency(session, pending,
							ready ? LatencyDisposition::Ready : LatencyDisposition::TerminalFailure, ready);
					}
				}
			}

			ObjectManager& objects = ObjectManager::instance();
			if(session.pendingBlockerPosition) {
				const std::optional<Object> blocker = objects.FindObjectByName("NAV_BLOCKER");
				const std::optional<Scene::WorldPose> pose = blocker ? objects.GetCanonicalWorldPose(*blocker) : std::nullopt;
				const std::shared_ptr<const NavigationWorldRoot> world = navigation.PublishedWorld();
				const bool sourceRevisionObserved = blocker && world && std::any_of(
					world->sources.begin(), world->sources.end(), [&objects, &pose, &blocker](const NavigationSourceIndexRecord& source) {
						return source.enabled && source.identity.kind == NavigationSourceKind::Collider
							&& source.identity.objectId == objects.getUUID(*blocker)
							&& pose && source.revisionEvidence.transformRevision == pose->transformRevision;
					});
				const Scene::TransactionDiagnostics transactionDiagnostics =
					Scene::SceneMutationQueue::Instance().GetTransactionDiagnostics();
				session.counters.movingBlockerTransactionsCommitted = transactionDiagnostics.committedCount;
				session.counters.movingBlockerTransactionsRejected = transactionDiagnostics.rejectedCount;
				if(pose) {
					session.counters.movingBlockerLastObservedPose = pose->position;
					session.counters.movingBlockerObservedTransformRevision = pose->transformRevision;
				}
				if(world && blocker) {
					for(const NavigationSourceIndexRecord& source : world->sources) {
						if(source.enabled && source.identity.kind == NavigationSourceKind::Collider
							&& source.identity.objectId == objects.getUUID(*blocker)) {
							session.counters.movingBlockerPublishedSourceRevision = source.revisionEvidence.transformRevision;
							break;
						}
					}
				}
				if(pose && pose->position == *session.pendingBlockerPosition && sourceRevisionObserved) {
					++session.counters.movingBlockerUpdates;
					session.pendingBlockerPosition.reset();
				}
			}
			for(std::size_t index = 0U; index < session.ownerIds.size(); ++index) {
				const std::optional<Object> owner = objects.getObject(session.ownerIds[index]);
				const std::optional<Scene::WorldPose> pose = owner ? objects.GetCanonicalWorldPose(*owner) : std::nullopt;
				if(!pose || pose->position != session.initialOwnerPositions[index]) {
					session.navigationDidNotMoveOwners = false;
					break;
				}
			}

			const TerraEngine::Scheduling::FrameExecutionPlan& executedPlan = engine.GetParallelFrameExecutionPlan();
			const std::string_view expectedOwner = session.options.scenario == ValidationScenario::ClearanceParity
				? "NavigationSystem:" : session.options.cohort == Cohort::Agent
				? "NavigationAgentSystem:" : "NavigationSystem:";
			if(executedPlan.migrationSummary.workerPreparationTimeNs != 0U
				&& std::any_of(executedPlan.migrationSummary.workerCategories.begin(), executedPlan.migrationSummary.workerCategories.end(),
				[expectedOwner](const std::string& category) { return category.starts_with(expectedOwner); })) {
				++session.counters.schedulerPrepareFrames;
			}
			if(session.options.scenario == ValidationScenario::Workload && session.options.cohort == Cohort::Agent) {
				for(const TerraEngine::Scheduling::FrameWorkloadResult& result : executedPlan.results) {
					if(!result.completed || result.fallbackUsed) {
						continue;
					}
					for(std::size_t chunk = 0U; chunk < 8U; ++chunk) {
						const std::string descriptor = chunk == 0U
							? "NavigationAgentSystem/Navigation/Prepare"
							: "NavigationAgentSystem/Navigation/Prepare/" + std::to_string(chunk);
						if(result.declaration.stableDescriptorId == descriptor) {
							session.counters.agentPrepareChunkMask |= (1U << chunk);
							session.counters.agentPreparePartitionPeak = std::max<std::uint64_t>(
								session.counters.agentPreparePartitionPeak, result.partitionCount);
							break;
						}
					}
				}
			}
			const std::shared_ptr<const ThreadPoolTelemetrySnapshot> telemetry = ThreadPool::instance().GetTelemetrySnapshot();
			if(telemetry) {
				const std::size_t dispatchedWorkers = static_cast<std::size_t>(std::count_if(
					telemetry->workers.begin(), telemetry->workers.end(),
					[](const ThreadPoolWorkerTelemetry& worker) { return worker.dispatchCount != 0U; }));
				session.counters.workerDispatchPeak = std::max<std::uint64_t>(
					session.counters.workerDispatchPeak, dispatchedWorkers);
			}
			if(session.warmupSeeded && session.counters.retainedPeak >= session.options.population) {
				session.warmupRetainedPopulationReady = true;
			}
			// Keep polling cost proportional to live owner intents, not to the full
			// 60,000-command evidence history. Finalized counts and latency samples
			// were copied above, so their ledger records no longer need retention.
			session.pendingLatency.erase(std::remove_if(session.pendingLatency.begin(), session.pendingLatency.end(),
				[](const PendingLatency& pending) { return !pending.active; }), session.pendingLatency.end());
		}

		void ReleaseDirectRequests(Session& session)
		{
			NavigationSystem& navigation = NavigationSystem::Instance();
			std::vector<NavigationRequestHandle> unreleased;
			std::vector<NavigationRequestHandle> requests = session.currentDirectRequests;
			requests.insert(requests.end(), session.retiredDirectRequests.begin(), session.retiredDirectRequests.end());
			for(const NavigationRequestHandle request : requests) {
				if(!request.IsValid()) {
					continue;
				}
				(void)navigation.CancelRequest(request);
				if(!navigation.ReleaseRequestResult(request)) {
					unreleased.push_back(request);
				}
			}
			session.currentDirectRequests = std::move(unreleased);
			session.retiredDirectRequests.clear();
			// Clearance parity owns one direct handle outside the workload owner array.
			// Release it on timeout/error too, so a failing semantic lane cannot hide a
			// request behind host shutdown or affect the next in-process editor session.
			if(session.clearanceRequest.IsValid()) {
				(void)navigation.CancelRequest(session.clearanceRequest);
				(void)navigation.ReleaseRequestResult(session.clearanceRequest);
				session.clearanceRequest = {};
			}
		}

		void QueueAgentStops(Session& session)
		{
			if(session.options.cohort != Cohort::Agent) {
				return;
			}
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			for(const NavigationAgentIdentity identity : session.agentIdentities) {
				const NavigationAgentStatusSnapshot status = agents.ReadStatus(identity);
				const NavigationAgentReceipt receipt = agents.QueueStop(identity,
					status.configurationRevision, status.intentSequence, ++session.nextOrderingToken,
					++session.nextCorrelationId);
				if(receipt.state == NavigationAgentReceiptState::Rejected) {
					++session.counters.rejected;
				}
			}
		}

		void QueueOwnerDeactivation(Session& session)
		{
			if(session.ownersDeactivationQueued) {
				return;
			}
			Scene::TransactionalMutationBuilder transaction(
				"navigation-agent-workload-owner-teardown", Scene::TransactionOrigin::Test,
				"NavigationApplicationValidation");
			transaction.SetBudget({session.ownerIds.size(), 128U});
			for(const UUID owner : session.ownerIds) {
				transaction.SetActive(owner, false);
			}
			Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(transaction).Build());
			session.ownersDeactivationQueued = true;
		}

		[[nodiscard]] bool CaptureWarmupReachability(Session& session);

		void BeginTeardown(Session& session)
		{
			if(session.teardownStarted) {
				return;
			}
			// Revocation deliberately clears cache ownership and status. Retain the
			// observed pre-teardown result so final evidence describes the run, not
			// the empty runtime produced by successful cleanup.
			session.completedCacheStatus = NavigationSystem::Instance().SavedGroundCacheStatus();
			// A failed warmup is evidence too. Preserve its per-owner outcomes before
			// Stop and release erase the records needed to diagnose missing coverage.
			if(session.warmupSeeded && !session.warmupReachabilityCaptured) {
				(void)CaptureWarmupReachability(session);
			}
			// Stop is queued first so the normal agent capture/publish path observes
			// the explicit terminal intent before the following frame deactivates the
			// fixture owners. Direct results are owned by this runner and release now.
			QueueAgentStops(session);
			ReleaseDirectRequests(session);
			session.teardownStarted = true;
		}

		[[nodiscard]] bool TeardownComplete(Session& session)
		{
			if(!session.teardownStarted || session.teardownFrames < 3U) {
				return false;
			}
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			NavigationSystem& navigation = NavigationSystem::Instance();
			const std::shared_ptr<const NavigationAgentDiagnosticsSnapshot> beforeRevoke = agents.PublishedDiagnostics();
			session.preRevokeStoreUsage = navigation.PublishedStoreUsage();
			session.preRevokeUsageObserved = true;
			if((beforeRevoke && beforeRevoke->activeAgents != 0U)
				|| session.preRevokeStoreUsage.activeRequests != 0U
				|| session.preRevokeStoreUsage.activeRoutes != 0U
				|| session.preRevokeStoreUsage.pendingRoutes != 0U
				|| session.preRevokeStoreUsage.deferredRoutes != 0U
				|| session.preRevokeStoreUsage.preparedRoutes != 0U) {
				// Do not let RevokeAllAgents manufacture a passing zero count. The
				// queued owner deactivation must first become visible through normal
				// capture/publication frames.
				return false;
			}
			// Revoke the copied runtime packet lifetime only after inactive owners had
			// a normal scheduler frame to publish. This avoids retaining route/request
			// handles after a validation session without mutating live storage on a worker.
			agents.RevokeAllAgents(NavigationReason::SceneReplaced);
			const std::shared_ptr<const NavigationAgentDiagnosticsSnapshot> diagnostics = agents.PublishedDiagnostics();
			session.postRevokeStoreUsage = navigation.PublishedStoreUsage();
			session.postRevokeUsageObserved = true;
			return (diagnostics == nullptr || diagnostics->activeAgents == 0U)
				&& session.postRevokeStoreUsage.activeRequests == 0U
				&& session.postRevokeStoreUsage.activeRoutes == 0U
				&& session.postRevokeStoreUsage.pendingRoutes == 0U
				&& session.postRevokeStoreUsage.deferredRoutes == 0U
				&& session.postRevokeStoreUsage.preparedRoutes == 0U;
		}

		[[nodiscard]] bool CaptureWarmupReachability(Session& session)
		{
			if(session.warmupReachabilityCaptured) {
				return session.warmupReachability.size() == session.options.population;
			}
			nlohmann::json reachability = nlohmann::json::array();
			for(std::uint32_t pairIndex = 0U; pairIndex < session.options.population; ++pairIndex) {
				std::string outcome;
				std::string reason;
				if(session.options.cohort == Cohort::Direct) {
					const auto request = NavigationSystem::Instance().ReadRequest(session.currentDirectRequests[pairIndex]);
					if(!request.record) {
						outcome = "MissingRequest";
						reason = "NoRetainedHandle";
					}
					else {
						outcome = std::string(ToString(request.record->outcome));
						reason = std::string(ToString(request.record->reason));
					}
				}
				else {
					const NavigationAgentStatusSnapshot status = NavigationAgentSystem::Instance().ReadStatus(
						session.agentIdentities[pairIndex]);
					outcome = std::string(ToString(status.outcome));
					reason = std::string(ToString(status.reason));
				}
				reachability.push_back({
					{"pairIndex", pairIndex},
					{"outcome", std::move(outcome)},
					{"reason", std::move(reason)}});
			}
			session.warmupReachability = std::move(reachability);
			session.warmupReachabilityCaptured = true;
			return session.warmupReachability.size() == session.options.population;
		}

		[[nodiscard]] nlohmann::json CaptureWarmupOutcomeSummary(const Session& session)
		{
			nlohmann::json summary = nlohmann::json::object();
			auto count = [&summary](std::string_view outcome, std::string_view reason) {
				const std::string key = std::string(outcome) + "/" + std::string(reason);
				summary[key] = summary.value(key, 0U) + 1U;
			};
			for(const nlohmann::json& entry : session.warmupReachability) {
				count(entry.value("outcome", std::string{"MissingRequest"}),
					entry.value("reason", std::string{"NoRetainedHandle"}));
			}
			return summary;
		}

		[[nodiscard]] bool WriteWarmupCacheArtifacts(Session& session, std::string& error)
		{
			if(session.options.cacheOutputPath.empty()) {
				return true;
			}
			const std::optional<NavigationSystem::SavedNavigationCacheSnapshot> snapshot =
				NavigationSystem::Instance().CapturePublishedGroundCache(SelectedFixtureProfile(session.options));
			if(!snapshot) {
				error = "Current complete selected-profile ground coverage could not be exported.";
				session.cacheOutputFailure = error;
				return false;
			}
			const SavedNavigationWriteResult current = WriteSavedNavigationCacheAtomically(
				session.options.cacheOutputPath, snapshot->descriptor, snapshot->chunks);
			if(!current.accepted) {
				error = "Unable to write current navigation cache artifact: " + current.reason;
				session.cacheOutputFailure = error;
				return false;
			}
			SavedNavigationDescriptor staleDescriptor = snapshot->descriptor;
			staleDescriptor.sourceFingerprint = staleDescriptor.sourceFingerprint
				== std::numeric_limits<std::uint64_t>::max()
				? staleDescriptor.sourceFingerprint - 1U : staleDescriptor.sourceFingerprint + 1U;
			const SavedNavigationWriteResult stale = WriteSavedNavigationCacheAtomically(
				session.options.cacheStaleOutputPath, staleDescriptor, snapshot->chunks);
			if(!stale.accepted) {
				error = "Unable to write stale navigation cache artifact: " + stale.reason;
				session.cacheOutputFailure = error;
				return false;
			}
			session.currentCacheDescriptor = snapshot->descriptor;
			session.cacheOutputWritten = true;
			session.cacheStaleOutputWritten = true;
			return true;
		}

		[[nodiscard]] nlohmann::json BuildReport(const Session& session, bool succeeded,
			bool pending, std::string failure = {})
		{
			const Clock::time_point now = Clock::now();
			const double wallClockSeconds = std::chrono::duration<double>(now - session.startedAt).count();
			const bool qualification = session.options.qualificationRequested
				&& session.options.warmupSeconds >= kMinimumQualificationWarmupSeconds
				&& session.options.measurementSeconds >= kMinimumQualificationMeasurementSeconds
				&& session.options.population == 1000U
				&& session.options.changesPerSecond == 100U
				&& session.options.requiredWorkerCount == kRequiredQualificationWorkers;
			const std::uint64_t executedCaseCount = session.fixtureBindingsObserved ? 1U
				+ (session.eligibleStaticSourcesObserved ? 1U : 0U)
				+ (session.counters.accepted != 0U ? 1U : 0U)
				+ (session.counters.schedulerPrepareFrames != 0U ? 1U : 0U)
				+ ((!session.options.requireRenderedPresentation || session.renderedPresentationObserved) ? 1U : 0U) : 0U;
			const NavigationSettings settings = NavigationSystem::Instance().SettingsSnapshot();
			const NavigationSystem::StoreUsageSnapshot storeUsage = NavigationSystem::Instance().PublishedStoreUsage();
			const NavigationSystem::SavedNavigationCacheStatus cacheStatus =
				session.completedCacheStatus.value_or(NavigationSystem::Instance().SavedGroundCacheStatus());
			const std::shared_ptr<const NavigationAgentDiagnosticsSnapshot> agentDiagnostics =
				NavigationAgentSystem::Instance().PublishedDiagnostics();
			const auto cacheModeName = [&session]() -> std::string_view {
				switch(session.options.expectedCacheMode) {
				case CacheExpectation::Absent: return "absent";
				case CacheExpectation::Current: return "current";
				case CacheExpectation::Stale: return "stale";
				case CacheExpectation::Malformed: return "malformed";
				}
				return "absent";
			};
			const bool cacheExpectationSatisfied = [&session, &cacheStatus]() {
				switch(session.options.expectedCacheMode) {
				case CacheExpectation::Absent:
					return !cacheStatus.cacheUsed && cacheStatus.rejectionReason.empty();
				case CacheExpectation::Current:
					return cacheStatus.cacheUsed && cacheStatus.rejectionReason.empty();
				case CacheExpectation::Stale:
				case CacheExpectation::Malformed:
					return !cacheStatus.cacheUsed && !cacheStatus.rejectionReason.empty()
						&& cacheStatus.rejectionReason != "SavedNavigationAwaitingAuthoritativeCapture";
				}
				return false;
			}();
			const auto describeBlockerPose = [](const std::optional<Scene::WorldPosition>& pose) -> nlohmann::json {
				if(!pose) return nullptr;
				return nlohmann::json{
					{"cell", {pose->cell.x, pose->cell.y, pose->cell.z}},
					{"offsetMetres", {pose->offsetMetres.x, pose->offsetMetres.y, pose->offsetMetres.z}}};
			};
			const NavigationProfile pinnedProfile = SelectedFixtureProfile(session.options);
			nlohmann::json pinnedProfileEvidence = {
				{"radiusMetres", pinnedProfile.radiusMetres},
				{"heightMetres", pinnedProfile.heightMetres},
				{"stepHeightMetres", pinnedProfile.stepHeightMetres},
				{"maxSlopeDegrees", pinnedProfile.maxSlopeDegrees},
				{"clearanceMarginMetres", pinnedProfile.clearanceMarginMetres},
				{"horizontalResolutionMetres", pinnedProfile.horizontalResolutionMetres},
				{"verticalResolutionMetres", pinnedProfile.verticalResolutionMetres},
				{"excludedAreasMask", pinnedProfile.excludedAreasMask},
				{"areaPenalties", nlohmann::json::array()}};
			for(const double penalty : pinnedProfile.areaPenalties) {
				pinnedProfileEvidence["areaPenalties"].push_back(penalty);
			}
			const bool clearanceExpectationPassed = session.options.scenario != ValidationScenario::ClearanceParity
				|| (session.options.clearanceProfile == ClearanceProfileSelection::Small
					? session.clearanceOutcome == NavigationOutcome::Complete
						&& session.clearanceReason == NavigationReason::None
					: session.clearanceOutcome == NavigationOutcome::Unreachable
						&& session.clearanceReason != NavigationReason::None);
			const double measuredSeconds = session.measurementStarted
				? std::chrono::duration<double>(now - session.measurementStartedAt).count() : 0.0;
			const auto transitionEvidence = [&session](std::size_t index) {
				const std::uint64_t before = session.routeTransitionBaseline[index];
				const std::uint64_t after = session.routeTransitionCounters[index];
				// An unexpected lifetime reset is explicit missing evidence, never an
				// unsigned wraparound or a fabricated zero-count measurement.
				return nlohmann::json{{"beforeMeasurement", before}, {"lastObserved", after},
					{"measurementAndDrainDelta", session.measurementStarted && after >= before
						? nlohmann::json(after - before) : nlohmann::json(nullptr)}};
			};
			const auto routeTimingEvidence = [&session](std::size_t index, const char* population,
				const char* excludes) {
				// Counts and sums can be differenced across measurement. Lifetime maxima
				// cannot: expose both endpoints without mislabelling a startup maximum.
				const auto delta = [&session](std::size_t field) -> nlohmann::json {
					const std::uint64_t before = session.routeTimingBaseline[field];
					const std::uint64_t after = session.routeTimingCounters[field];
					return session.measurementStarted && after >= before
						? nlohmann::json(after - before) : nlohmann::json(nullptr);
				};
				return nlohmann::json{
					{"population", population}, {"excludes", excludes},
					{"count", {{"baseline", session.routeTimingBaseline[index]},
						{"lastObserved", session.routeTimingCounters[index]},
						{"measurementAndDrainDelta", delta(index)}}},
					{"totalNanoseconds", {{"baseline", session.routeTimingBaseline[index + 1U]},
						{"lastObserved", session.routeTimingCounters[index + 1U]},
						{"measurementAndDrainDelta", delta(index + 1U)}}},
					{"maxNanoseconds", {{"baseline", session.routeTimingBaseline[index + 2U]},
						{"lastObserved", session.routeTimingCounters[index + 2U]}}}};
			};
			const auto tileReuseEvidence = [&session](std::size_t index) {
				const std::uint64_t before = session.tileReuseBaseline[index];
				const std::uint64_t after = session.tileReuseCounters[index];
				// Match the transition counter contract: lifetime resets are unavailable
				// evidence, and the final sample does not imply post-shutdown state.
				return nlohmann::json{{"beforeMeasurement", before}, {"lastObserved", after},
					{"measurementAndDrainDelta", session.measurementStarted && after >= before
						? nlohmann::json(after - before) : nlohmann::json(nullptr)}};
			};
			return {
				{"succeeded", succeeded},
				{"pending", pending},
				{"orderlyShutdown", false},
				{"executedCaseCount", executedCaseCount},
				{"qualification", qualification},
				{"qualificationRequested", session.options.qualificationRequested},
				{"cohort", session.options.cohort == Cohort::Agent ? "agent" : "direct"},
				{"executionMode", session.options.executionMode == ExecutionMode::EditorPlay ? "editor-play" : "export"},
				{"scenario", session.options.scenario == ValidationScenario::ClearanceParity ? "clearance-parity" : "workload"},
				{"clearanceProfile", session.options.scenario == ValidationScenario::ClearanceParity
					? std::string(ClearanceProfileName(session.options.clearanceProfile)) : std::string{}},
				{"clearanceRoute", session.options.scenario == ValidationScenario::ClearanceParity ? nlohmann::json{
					{"submitted", session.clearanceRequestSubmitted},
					{"terminal", session.clearanceRequestTerminal},
					{"outcome", std::string(ToString(session.clearanceOutcome))},
					{"reason", std::string(ToString(session.clearanceReason))},
					{"start", {-24.0, 0.0, 0.0}},
					{"destination", {24.0, 0.0, 0.0}}} : nlohmann::json(nullptr)},
				{"clearanceExpectationPassed", clearanceExpectationPassed},
				{"wallClockSeconds", wallClockSeconds},
				{"measuredSeconds", measuredSeconds},
				{"warmupSeconds", session.options.warmupSeconds},
				{"measurementSeconds", session.options.measurementSeconds},
				{"requiredWorkerCount", session.options.requiredWorkerCount},
				{"actualWorkerCount", ThreadPool::instance().getThreadCount()},
				{"population", session.options.population},
				{"changesPerSecond", session.options.changesPerSecond},
				{"frames", session.frames},
				{"fixtureBindingsObserved", session.fixtureBindingsObserved},
				{"fixtureBindingWait", {
					{"reason", session.fixtureBindingWaitReason},
					{"terrainState", session.fixtureRootTerrainState},
					{"terrainPublicationGeneration", session.fixtureRootTerrainPublicationGeneration},
					{"coverageCount", session.fixtureRootCoverageCount},
					{"sourceCount", session.fixtureRootSourceCount},
					{"terrainCollision", {
						{"status", session.fixtureCollisionStatus},
						{"pendingCandidates", session.fixtureCollisionPendingCandidates},
						{"candidatesStaged", session.fixtureCollisionCandidatesStaged},
						{"publicationAttempts", session.fixtureCollisionPublicationAttempts},
						{"publicationCommits", session.fixtureCollisionPublicationCommits},
						{"publicationRejections", session.fixtureCollisionPublicationRejections},
						{"message", session.fixtureCollisionMessage}}}}},
				// This bounded diagnostic captures the real scheduler result that preceded
				// a missing navigation root. It makes Frame End's execution policy and
				// any propagated inactive dependency visible in installed smoke reports.
				{"framePlanDiagnostics", {
					{"omittedWorkloadFrames", session.omittedWorkloadFrames},
					{"frameEnd", session.frameEndWorkload},
					{"firstOmittedWorkload", session.firstOmittedWorkload},
					{"lastOmittedWorkload", session.lastOmittedWorkload},
					{"firstDiagnostic", session.firstPlanDiagnostic},
					{"lastDiagnostic", session.lastPlanDiagnostic}}},
				{"eligibleStaticSourcesObserved", session.eligibleStaticSourcesObserved},
				{"eligibleStaticSourceCount", session.eligibleStaticSourceCount},
				{"warmupRetainedPopulationReady", session.warmupRetainedPopulationReady},
				{"warmupReachability", session.warmupReachability},
				{"warmupOutcomeSummary", CaptureWarmupOutcomeSummary(session)},
				{"cacheUsed", cacheStatus.cacheUsed},
				{"cacheRejectionReason", cacheStatus.rejectionReason},
				{"cacheCandidatePending", cacheStatus.candidatePending},
				{"cacheCandidateBytes", cacheStatus.candidateBytes},
				{"cacheLogicalPath", cacheStatus.logicalPath},
				{"expectedCacheMode", std::string(cacheModeName())},
				{"cacheOutputWritten", session.cacheOutputWritten},
				{"cacheStaleOutputWritten", session.cacheStaleOutputWritten},
				{"cacheOutputFailure", session.cacheOutputFailure},
				{"currentCacheDescriptor", session.currentCacheDescriptor ? nlohmann::json{
					{"formatVersion", session.currentCacheDescriptor->formatVersion},
					{"backendAbi", session.currentCacheDescriptor->backendAbi},
					{"sourceFingerprint", session.currentCacheDescriptor->sourceFingerprint},
					{"settingsFingerprint", session.currentCacheDescriptor->settingsFingerprint}}
					: nlohmann::json(nullptr)},
				{"renderedPresentationObserved", session.renderedPresentationObserved},
				// `submitted` remains the historical total for existing tooling. The
				// qualification contract uses the explicitly named measurement counters.
				{"submitted", session.counters.submitted},
				{"totalSubmitted", session.counters.submitted},
				{"warmupSubmitted", session.counters.warmupSubmitted},
				{"measurementSubmitted", session.counters.measurementSubmitted},
				{"accepted", session.counters.accepted},
				{"admitted", session.counters.measurementAdmitted},
				{"admittedFinalized", session.counters.admittedFinalized},
				{"pendingAdmitted", session.counters.measurementAdmitted - session.counters.admittedFinalized},
				{"measurementFinalized", session.counters.measurementFinalized},
				{"pendingAtCutoff", session.counters.pendingAtCutoff},
				{"rejected", session.counters.rejected},
				{"measurementDispositions", {
					{"rejected", session.counters.measurementRejected},
					{"expired", session.counters.measurementExpired},
					{"cancelled", session.counters.measurementCancelled},
					{"superseded", session.counters.measurementSuperseded},
					{"timedOut", session.counters.measurementTimedOut}}},
				{"retries", session.counters.retries},
				{"preparedRouteTransitions", {
					{"rootMismatch", transitionEvidence(0U)},
					{"rebasedUnaffected", transitionEvidence(1U)},
					{"rejectedIntersection", transitionEvidence(2U)},
					{"rejectedUnknown", transitionEvidence(3U)},
					{"unknownReasons", {
						{"invalidInput", transitionEvidence(4U)},
						{"terrainMismatch", transitionEvidence(5U)},
						{"missingLink", transitionEvidence(6U)},
						{"invalidLink", transitionEvidence(7U)},
						{"sourceChainMismatch", transitionEvidence(8U)},
						{"finalSourceMismatch", transitionEvidence(9U)}}}}},
				{"clearanceGeometryEvidence", session.clearanceSourceGeometry},
				{"firstObservedTransitionUnknownDiagnostics", session.firstObservedTransitionUnknownDiagnostics},
				{"asyncBuildObservations", {
					{"timings", {
						{"scope", "cumulative completed packets including failed and retired; admission-to-worker, worker execution, and completion-to-protected-poll"},
						{"GroundCompletionCount", {{"baseline", session.asyncTimingBaseline[0]}, {"lastObserved", session.asyncTimingCounters[0]}}},
						{"DynamicCompletionCount", {{"baseline", session.asyncTimingBaseline[1]}, {"lastObserved", session.asyncTimingCounters[1]}}},
						{"TotalWorkerNanoseconds", {{"baseline", session.asyncTimingBaseline[2]}, {"lastObserved", session.asyncTimingCounters[2]}}},
						{"MaxWorkerNanoseconds", {{"baseline", session.asyncTimingBaseline[3]}, {"lastObserved", session.asyncTimingCounters[3]}}},
						{"TotalQueueWaitNanoseconds", {{"baseline", session.asyncTimingBaseline[4]}, {"lastObserved", session.asyncTimingCounters[4]}}},
						{"MaxQueueWaitNanoseconds", {{"baseline", session.asyncTimingBaseline[5]}, {"lastObserved", session.asyncTimingCounters[5]}}},
						{"TotalPublicationWaitNanoseconds", {{"baseline", session.asyncTimingBaseline[6]}, {"lastObserved", session.asyncTimingCounters[6]}}},
						{"MaxPublicationWaitNanoseconds", {{"baseline", session.asyncTimingBaseline[7]}, {"lastObserved", session.asyncTimingCounters[7]}}}}},
					{"lastCurrent", session.asyncBuildLastObserved[0]},
					{"lastRetired", session.asyncBuildLastObserved[1]},
					{"lastChargedBytes", session.asyncBuildLastObserved[2]},
					{"peakChargedBytes", session.asyncBuildLastObserved[3]},
					{"admissionRejectedCumulative", session.asyncBuildLastObserved[4]},
					{"completedCumulative", session.asyncBuildLastObserved[5]},
					{"staleDiscardedCumulative", session.asyncBuildLastObserved[6]},
					{"peakOutstandingObserved", session.asyncBuildPeakOutstandingObserved}}},
				{"routeStageTimingObservations", {
					{"scope", "diagnostic cumulative stage populations; baseline precedes measurement, final observation includes drain; not end-to-end latency percentiles"},
					{"readyAdmissionToCapture", routeTimingEvidence(0U,
						"requests captured at least once; one sample per packet",
						"requests never captured; deferred coverage waiting before ready admission")},
					{"workerSearch", routeTimingEvidence(3U,
						"search executions, including repeat execution after requeue",
						"packets whose search never executes")},
					{"workerToPublication", routeTimingEvidence(6U,
						"prepared routes successfully assigned a valid route-store handle",
						"stale, cancelled, rejected, or failed packets without a stored route")},
					{"deferredCoverageWait", routeTimingEvidence(9U,
						"deferred requests promoted into the ready queue",
						"deferred requests cancelled or terminally failed before promotion")}}},
				{"dynamicTileReuse", {
					{"scope", "prepared dynamic work observed at publication; not a count of committed world roots"},
					{"attempts", tileReuseEvidence(0U)}, {"reusedTiles", tileReuseEvidence(1U)},
					{"rebuiltTiles", tileReuseEvidence(2U)}, {"fallbacks", tileReuseEvidence(3U)},
					{"firstObservedFallbacks", session.firstObservedTileReuseFallbacks}}},
				{"clearanceRoutePointCount", session.clearanceRoutePointCount},
				{"clearanceRoutePoints", session.clearanceRoutePoints},
				{"coalesced", session.counters.coalesced},
				{"completed", session.counters.completed},
				{"following", session.counters.following},
				{"arrived", session.counters.arrived},
				{"stopped", session.counters.stopped},
				{"failed", session.counters.failed},
				{"peakOutstanding", session.counters.peakOutstanding},
				{"memoryHighWaterBytes", session.counters.memoryHighWaterBytes},
				{"schedulerPrepareFrames", session.counters.schedulerPrepareFrames},
				{"navigationPrepareTimingNanoseconds", {
					// Keep partition zero separate for comparison with earlier synchronous
					// build runs. Background provider-task duration is not frame-range time.
					{"observedFrames", session.counters.navigationPrepareObservedFrames},
					{"partitionZero", {
						{"executionCount", session.counters.navigationPreparePartitionZeroCount},
						{"total", session.counters.navigationPreparePartitionZeroRunNanoseconds},
						{"maximum", session.counters.navigationPreparePartitionZeroMaximumNanoseconds}}},
					{"routePartitions", {
						{"executionCount", session.counters.navigationPrepareRoutePartitionCount},
						{"total", session.counters.navigationPrepareRouteRunNanoseconds},
						{"maximum", session.counters.navigationPrepareRouteMaximumNanoseconds},
						{"usefulWorkItems", session.counters.navigationPrepareRouteWorkItems}}}}},
				{"movingBlockerUpdates", session.counters.movingBlockerUpdates},
				{"movingBlockerDiagnostics", {
					{"mutationsQueued", session.counters.movingBlockerMutationsQueued},
					{"transactionsCommitted", session.counters.movingBlockerTransactionsCommitted},
					{"transactionsRejected", session.counters.movingBlockerTransactionsRejected},
					{"lastTransactionId", session.counters.movingBlockerLastTransactionId},
					{"lastIntendedPose", describeBlockerPose(session.counters.movingBlockerLastIntendedPose)},
					{"lastObservedPose", describeBlockerPose(session.counters.movingBlockerLastObservedPose)},
					{"observedTransformRevision", session.counters.movingBlockerObservedTransformRevision},
					{"publishedSourceRevision", session.counters.movingBlockerPublishedSourceRevision}}},
				{"retainedPeak", session.counters.retainedPeak},
				{"workerDispatchPeak", session.counters.workerDispatchPeak},
				{"agentPreparePartitionPeak", session.counters.agentPreparePartitionPeak},
				{"agentPrepareChunkMask", session.counters.agentPrepareChunkMask},
				{"remainingRequests", storeUsage.activeRequests},
				{"remainingRoutes", storeUsage.activeRoutes},
				{"remainingAgents", agentDiagnostics ? agentDiagnostics->activeAgents : 0U},
				{"navigationStoreUsage", {
					{"activeRequests", storeUsage.activeRequests},
					{"activeRoutes", storeUsage.activeRoutes},
					{"pendingRoutes", storeUsage.pendingRoutes},
					{"deferredRoutes", storeUsage.deferredRoutes},
					{"preparedRoutes", storeUsage.preparedRoutes},
					{"estimatedOwnedBytes", storeUsage.estimatedOwnedBytes},
					{"scope", "current NavigationSystem stores and queued work; excludes process RSS and externally retained snapshots"}}},
				{"teardownStoreUsage", {
					{"preRevokeObserved", session.preRevokeUsageObserved},
					{"preRevokeActiveRequests", session.preRevokeStoreUsage.activeRequests},
					{"preRevokeActiveRoutes", session.preRevokeStoreUsage.activeRoutes},
					{"postRevokeObserved", session.postRevokeUsageObserved},
					{"postRevokeActiveRequests", session.postRevokeStoreUsage.activeRequests},
					{"postRevokeActiveRoutes", session.postRevokeStoreUsage.activeRoutes}}},
				{"settings", {
					{"maxActiveProfileBuilds", settings.maxActiveProfileBuilds},
					{"maxActiveAgents", settings.maxActiveAgents},
					{"maxAdmittedRequests", settings.maxAdmittedRequests},
					{"maxConcurrentBuilds", settings.maxConcurrentBuilds},
					{"maxConcurrentSearches", settings.maxConcurrentSearches},
					{"maxDetourNodesPerQuery", settings.maxDetourNodesPerQuery},
					{"maxEngineNodesPerRequest", settings.maxEngineNodesPerRequest},
					{"maxRoutePoints", settings.maxRoutePoints},
					{"workerExpansionQuantum", settings.workerExpansionQuantum},
					{"maxGeometryBytes", settings.maxGeometryBytes},
					{"maxPublishedAndRetiredBytes", settings.maxPublishedAndRetiredBytes},
					{"maxBuildScratchBytes", settings.maxBuildScratchBytes},
					{"maxRequestAndRouteBytes", settings.maxRequestAndRouteBytes},
					{"maxDiagnosticBytes", settings.maxDiagnosticBytes},
					{"defaultRequestDeadlineMilliseconds", settings.defaultRequestDeadlineMilliseconds},
					{"maximumRequestLifetimeMilliseconds", settings.maximumRequestLifetimeMilliseconds},
					{"defaultRouteRetentionMilliseconds", settings.defaultRouteRetentionMilliseconds},
					{"terminalRequestRetentionMilliseconds", settings.terminalRequestRetentionMilliseconds}}},
				{"pinnedProfile", std::move(pinnedProfileEvidence)},
				{"admittedLatencyMilliseconds", {
					{"sampleCount", session.counters.latencyMilliseconds.size()},
					{"p50", Percentile(session.counters.latencyMilliseconds, 0.50)},
					{"p95", Percentile(session.counters.latencyMilliseconds, 0.95)},
					{"p99", Percentile(session.counters.latencyMilliseconds, 0.99)},
					{"comparableForQualification", session.counters.latencyMilliseconds.size()
						== session.counters.measurementAdmitted && !session.ledgerCapacityExceeded}}},
				{"profileReadyObservedLatencyMilliseconds", {
					{"sampleCount", session.counters.readyCoverageLatencyMilliseconds.size()},
					{"p50", Percentile(session.counters.readyCoverageLatencyMilliseconds, 0.50)},
					{"p95", Percentile(session.counters.readyCoverageLatencyMilliseconds, 0.95)},
					{"p99", Percentile(session.counters.readyCoverageLatencyMilliseconds, 0.99)},
					{"description", "At least one matching-profile coverage record was ready at command/request admission and terminal observation; does not prove query corridor readiness; diagnostic only."}}},
				// Backward-compatible name for callers that have not yet moved to the
				// explicit admitted latency contract above.
				{"latencyMilliseconds", {
					{"sampleCount", session.counters.latencyMilliseconds.size()},
					{"p50", Percentile(session.counters.latencyMilliseconds, 0.50)},
					{"p95", Percentile(session.counters.latencyMilliseconds, 0.95)},
					{"p99", Percentile(session.counters.latencyMilliseconds, 0.99)}}},
				// This subset excludes terminal failures and is therefore diagnostic
				// context only. Qualification uses the raw admitted-outcome percentile
				// above so a failure cannot disappear from the p95 calculation.
				{"successOnlyLatencyMilliseconds", {
					{"sampleCount", session.counters.readyLatencyMilliseconds.size()},
					{"p50", Percentile(session.counters.readyLatencyMilliseconds, 0.50)},
					{"p95", Percentile(session.counters.readyLatencyMilliseconds, 0.95)},
					{"p99", Percentile(session.counters.readyLatencyMilliseconds, 0.99)}}},
				{"semanticChecks", {
					{"fixtureBindings", session.fixtureBindingsObserved},
					{"warmupRetainedPopulation", session.warmupRetainedPopulationReady},
					{"workerPreparation", session.counters.schedulerPrepareFrames != 0U},
					{"renderedPresentation", !session.options.requireRenderedPresentation || session.renderedPresentationObserved},
					{"navigationDoesNotMoveOwners", session.navigationDidNotMoveOwners},
					{"cacheExpectation", cacheExpectationSatisfied},
					{"clearanceExpectation", clearanceExpectationPassed},
					{"measurementLedgerExact", session.counters.measurementFinalized == session.counters.measurementSubmitted},
					{"admittedLatencyExact", session.counters.admittedFinalized == session.counters.measurementAdmitted
						&& !session.ledgerCapacityExceeded}}},
				{"failure", std::move(failure)}
			};
		}

		[[nodiscard]] bool HasCapacityFor(const WorkloadOptions& options, std::string& error)
		{
			const NavigationSettings settings = NavigationSystem::Instance().SettingsSnapshot();
			const std::uint64_t required = static_cast<std::uint64_t>(options.population) * 4U;
			if(settings.maxAdmittedRequests < required) {
				error = "Navigation request capacity is too small for the retained cohort. Configure maxAdmittedRequests to at least population * 4 before Engine::Initialize.";
				return false;
			}
			return true;
		}

		[[nodiscard]] bool AdvanceOneCompletedFrame(Engine& engine, Session& session, std::string& error)
		{
			if(session.options.executionMode == ExecutionMode::Export) {
				// Export owns the next frame and can request detailed scheduler evidence
				// before it enters the task graph. Editor-play may decline this request;
				// the report then fails worker evidence instead of inferring it.
				Scheduling::SetEcsGraphFrameDiagnosticsRequested(true);
				// Keep the qualification's 630 seconds tied to steady-clock elapsed
				// time while still executing ordinary fixed scheduler frames. Without
				// this pacing a fast host could simulate hundreds of seconds in a few
				// real seconds and mislabel the result as a soak.
				const auto frameInterval = std::chrono::duration_cast<Clock::duration>(
					std::chrono::duration<double>(1.0 / 60.0));
				if(session.nextFrameDue == Clock::time_point{}) {
					session.nextFrameDue = Clock::now();
				}
				if(Clock::now() < session.nextFrameDue) {
					std::this_thread::sleep_until(session.nextFrameDue);
				}
				session.nextFrameDue += frameInterval;
				// Export validation owns this bounded loop, so it advances the actual
				// scheduler instead of directly updating one navigation system.
				if(!engine.Update(1.0F / 60.0F)) {
					error = "The export Engine stopped before navigation validation completed.";
					return false;
				}
			}
			++session.frames;
			ObserveRenderedPresentation(engine, session);
			// Capture scheduler state before fixture binding gates return early. A missing
			// Frame End callback is itself the evidence needed to explain root/source 0.
			ObserveFramePlanDiagnostics(engine, session);
			if(session.options.scenario == ValidationScenario::ClearanceParity) {
				if(!session.fixtureBindingsObserved
					&& !CaptureClearanceFixtureBindings(session, error) && !error.empty()) {
					return false;
				}
				if(!session.fixtureBindingsObserved) {
					return true;
				}
				if(!SubmitClearanceRoute(session, error)) {
					return false;
				}
				// Observe only data published by the completed application frame. The
				// next normal frame consumes the direct request; no system is updated
				// directly from this test-side helper.
				ObserveCounters(engine, session);
				ObserveClearanceRoute(session);
				return true;
			}
			if(!session.fixtureBindingsObserved && !CaptureFixtureBindings(session, error) && !error.empty()) {
				return false;
			}
			if(!session.fixtureBindingsObserved) {
				return true;
			}

			const Clock::time_point now = Clock::now();
			SeedWarmupCohort(session, now);
			const double wallElapsedSeconds = std::chrono::duration<double>(now - session.startedAt).count();
			const double measurementElapsedSeconds = session.measurementStarted
				? std::chrono::duration<double>(now - session.measurementStartedAt).count() : 0.0;
			const std::uint64_t elapsedWholeSeconds = static_cast<std::uint64_t>(measurementElapsedSeconds);
			if(session.measurementStarted && !session.measurementDrainStarted
				&& elapsedWholeSeconds < session.options.measurementSeconds
				&& (!session.blockerMeasurementStarted || elapsedWholeSeconds > session.lastBlockerSecond)) {
				// The fixture's blocker is authored data. Its motion still enters through
				// the typed scene mutation queue and becomes visible at a normal boundary.
				QueueMovingBlockerMutation(session, now);
				session.lastBlockerSecond = elapsedWholeSeconds;
				session.blockerMeasurementStarted = true;
			}
			ObserveCounters(engine, session);
			if(!session.measurementStarted && wallElapsedSeconds >= static_cast<double>(session.options.warmupSeconds)
				&& session.warmupRetainedPopulationReady) {
				// Freeze semantic parity evidence before churn replaces the warmed
				// destinations, then export the pristine current cache before the moving
				// blocker changes any authoritative source fingerprint.
				if(!CaptureWarmupReachability(session)) {
					error = "Warmup reachability did not retain one canonical outcome per fixture pair.";
					return false;
				}
				if(!WriteWarmupCacheArtifacts(session, error)) {
					return false;
				}
				session.measurementStarted = true;
				session.routeTransitionBaseline = session.routeTransitionCounters;
				session.asyncTimingBaseline = session.asyncTimingCounters;
				session.routeTimingBaseline = session.routeTimingCounters;
				session.tileReuseBaseline = session.tileReuseCounters;
				session.measurementStartedAt = now;
				session.nextChangeAt = now;
			}
			SubmitDueChanges(session, now);
			return true;
		}
	}

	nlohmann::json RunNavigationApplicationValidation(Engine& engine, const nlohmann::json& options)
	{
		std::string error;
		const std::optional<WorkloadOptions> parsed = ParseOptions(options, error);
		if(!parsed) {
			return Failure(std::move(error));
		}
		if(parsed->managedOutcomeProbe != ManagedOutcomeProbe::None) {
			// EditorMain consumes this host-only option before Engine::Initialize and
			// deliberately leaves normal authoring/Play controls enabled for MCP. A
			// call reaching the automated runner would otherwise silently submit an
			// unrelated native workload, so fail closed instead of manufacturing a
			// pending result or an automated success claim.
			return Failure("managedOutcomeProbe is editor-startup-only; use the normal installed editor Play/MCP workflow.");
		}
		if(ThreadPool::instance().getThreadCount() != parsed->requiredWorkerCount) {
			return Failure("The active ThreadPool worker count does not match requiredWorkerCount. Set TERRAENGINE_THREADPOOL_WORKERS before Engine::Initialize.");
		}
		if(!HasCapacityFor(*parsed, error)) {
			return Failure(std::move(error));
		}

		std::lock_guard<std::mutex> lock(sessionMutex);
		if(!activeSession || activeSession->engine != &engine) {
			if(activeSession) {
				return Failure("A different navigation application validation session is still active in this process.");
			}
			Session session{};
			session.engine = &engine;
			session.options = *parsed;
			session.startedAt = Clock::now();
			session.schedulerDiagnosticsWereEnabled = Scheduling::EcsGraphFrameDiagnosticsEnabled();
			Scheduling::SetEcsGraphFrameDiagnosticsRequested(true);
			session.schedulerDiagnosticsRequested = true;
			// Configure the copied profile catalog before this call advances an Engine
			// frame. MainEngine then captures both the project default and the fixture
			// profile in one normal protected bootstrap packet.
			EnsurePinnedFixtureProfileRegistration(session);
			activeSession = std::move(session);
		}
		else if(activeSession->options.executionMode != parsed->executionMode
			|| activeSession->options.cohort != parsed->cohort
			|| activeSession->options.population != parsed->population
			|| activeSession->options.scenario != parsed->scenario
			|| activeSession->options.clearanceProfile != parsed->clearanceProfile) {
			return Failure("A validation session cannot change scenario, profile, cohort, mode, or population while it is active.");
		}

		Session& session = *activeSession;
		const std::uint64_t remainingBudget = session.options.executionMode == ExecutionMode::EditorPlay
			? 1U
			: (session.options.frameBudget != 0U ? session.options.frameBudget
				: std::numeric_limits<std::uint64_t>::max());
		for(std::uint64_t frame = 0U; frame < remainingBudget; ++frame) {
			if(!AdvanceOneCompletedFrame(engine, session, error)) {
				ReleaseDirectRequests(session);
				const nlohmann::json report = BuildReport(session, false, false, std::move(error));
				RestoreSchedulerDiagnostics(session);
				activeSession.reset();
				return report;
			}
			const double elapsed = std::chrono::duration<double>(Clock::now() - session.startedAt).count();
			if(session.options.timeoutSeconds != 0U && elapsed > static_cast<double>(session.options.timeoutSeconds)) {
				TimeoutOutstandingMeasurementLatency(session);
				// Preserve the failure denominator before releasing direct handles.
				if(session.warmupSeeded && !session.warmupReachabilityCaptured) {
					(void)CaptureWarmupReachability(session);
				}
				ReleaseDirectRequests(session);
				const std::string failure = session.fixtureBindingWaitReason.empty()
					? "The real wall-clock validation timeout expired."
					: "The real wall-clock validation timeout expired while waiting for fixture bindings: "
						+ session.fixtureBindingWaitReason;
				const nlohmann::json report = BuildReport(session, false, false, failure);
				RestoreSchedulerDiagnostics(session);
				activeSession.reset();
				return report;
			}
			if(session.teardownStarted) {
				++session.teardownFrames;
				if(session.teardownFrames == 1U) {
					// The Stop commands were visible on the preceding normal frame. Queue
					// deactivation now so a later declared scene boundary owns cleanup.
					QueueOwnerDeactivation(session);
				}
				if(TeardownComplete(session)) {
					const bool noRequestsRemain = session.currentDirectRequests.empty();
					const std::shared_ptr<const NavigationAgentDiagnosticsSnapshot> diagnostics =
						NavigationAgentSystem::Instance().PublishedDiagnostics();
					const bool noAgentsRemain = !diagnostics || diagnostics->activeAgents == 0U;
					const bool cleanTeardown = noRequestsRemain && noAgentsRemain;
					const std::string failure = !cleanTeardown
						? "Validation teardown retained navigation resources after the declared lifecycle frames."
						: session.teardownFailure;
					const nlohmann::json report = BuildReport(session,
						cleanTeardown && session.teardownFailure.empty(), false, failure);
					RestoreSchedulerDiagnostics(session);
					activeSession.reset();
					return report;
				}
				continue;
			}
			if(session.options.scenario == ValidationScenario::ClearanceParity
				&& session.clearanceRequestTerminal) {
				// Cache artifacts are captured only after this exact profile has published
				// complete coverage and its real route reached a terminal result.
				if(!WriteWarmupCacheArtifacts(session, error)) {
					(void)NavigationSystem::Instance().ReleaseRequestResult(session.clearanceRequest);
					const nlohmann::json report = BuildReport(session, false, false, std::move(error));
					RestoreSchedulerDiagnostics(session);
					activeSession.reset();
					return report;
				}
				const NavigationSystem::SavedNavigationCacheStatus cacheStatus =
					NavigationSystem::Instance().SavedGroundCacheStatus();
				const bool cacheExpectationPassed = [&session, &cacheStatus]() {
					switch(session.options.expectedCacheMode) {
					case CacheExpectation::Absent:
						return !cacheStatus.cacheUsed && cacheStatus.rejectionReason.empty();
					case CacheExpectation::Current:
						return cacheStatus.cacheUsed && cacheStatus.rejectionReason.empty();
					case CacheExpectation::Stale:
					case CacheExpectation::Malformed:
						return !cacheStatus.cacheUsed && !cacheStatus.rejectionReason.empty()
							&& cacheStatus.rejectionReason != "SavedNavigationAwaitingAuthoritativeCapture";
					}
					return false;
				}();
				const bool expectedOutcome = session.options.clearanceProfile == ClearanceProfileSelection::Small
					? session.clearanceOutcome == NavigationOutcome::Complete
						&& session.clearanceReason == NavigationReason::None
					: session.clearanceOutcome == NavigationOutcome::Unreachable
						&& session.clearanceReason != NavigationReason::None;
				const bool rendered = !session.options.requireRenderedPresentation
					|| session.renderedPresentationObserved;
				const bool workerPrepared = session.counters.schedulerPrepareFrames != 0U;
				const bool released = NavigationSystem::Instance().ReleaseRequestResult(session.clearanceRequest);
				session.clearanceRequest = {};
				session.completedCacheStatus = cacheStatus;
				const bool succeeded = expectedOutcome && cacheExpectationPassed && rendered
					&& workerPrepared && released;
				const std::string failure = succeeded ? std::string{} :
					"Clearance semantic route, cache, render, worker, or request-release evidence failed.";
				const nlohmann::json report = BuildReport(session, succeeded, false, failure);
				RestoreSchedulerDiagnostics(session);
				activeSession.reset();
				return report;
			}
			const bool measurementComplete = session.measurementStarted
				&& std::chrono::duration<double>(Clock::now() - session.measurementStartedAt).count()
					>= static_cast<double>(session.options.measurementSeconds);
			if(measurementComplete) {
				if(!session.measurementDrainStarted) {
					// Stop pacing new work at the real measurement boundary, then give
					// every admitted command a bounded normal-frame drain interval.
					session.measurementDrainStarted = true;
					session.measurementDrainStartedAt = Clock::now();
					session.counters.pendingAtCutoff = static_cast<std::uint64_t>(std::count_if(
						session.pendingLatency.begin(), session.pendingLatency.end(),
						[](const PendingLatency& pending) { return pending.measurement && pending.active; }));
					continue;
				}
				const bool drainExpired = std::chrono::duration<double>(Clock::now()
					- session.measurementDrainStartedAt).count() >= kMeasurementDrainSeconds;
				if(HasActiveMeasurementLatency(session) && !drainExpired) {
					continue;
				}
				if(HasActiveMeasurementLatency(session)) {
					TimeoutOutstandingMeasurementLatency(session);
				}
				const bool presentationPassed = !session.options.requireRenderedPresentation
					|| session.renderedPresentationObserved;
				const bool workerPrepared = session.counters.schedulerPrepareFrames != 0U;
				const bool workerPoolObserved = session.counters.workerDispatchPeak >= session.options.requiredWorkerCount;
				const bool agentPartitionsObserved = session.options.cohort == Cohort::Direct
					|| session.counters.agentPrepareChunkMask == 0xFFU;
				const double measuredSeconds = std::chrono::duration<double>(Clock::now()
					- session.measurementStartedAt).count();
				const std::uint64_t requiredMeasurementChanges = static_cast<std::uint64_t>(session.options.changesPerSecond)
						* static_cast<std::uint64_t>(session.options.measurementSeconds);
				const std::uint64_t requiredTotalChanges = static_cast<std::uint64_t>(session.options.population)
					+ requiredMeasurementChanges;
				const bool workloadObserved = session.fixtureBindingsObserved
					&& session.eligibleStaticSourcesObserved
					&& session.warmupRetainedPopulationReady
					&& session.counters.measurementSubmitted >= requiredMeasurementChanges
					&& session.counters.submitted >= requiredTotalChanges
					&& session.counters.measurementAdmitted != 0U
					&& session.counters.retainedPeak >= session.options.population;
				const bool measuredLongEnough = measuredSeconds >= static_cast<double>(session.options.measurementSeconds);
				const bool blockerKeptPace = session.counters.movingBlockerUpdates
					>= session.options.measurementSeconds;
				const bool admittedLatencyExact = session.counters.admittedFinalized
					== session.counters.measurementAdmitted
					&& session.counters.measurementFinalized == session.counters.measurementSubmitted
					&& session.counters.latencyMilliseconds.size() == session.counters.measurementAdmitted
					&& !session.ledgerCapacityExceeded;
				const bool rawLatencyPassed = admittedLatencyExact
					&& Percentile(session.counters.latencyMilliseconds, 0.95) <= 100.0;
				const bool ownerPosesStable = session.navigationDidNotMoveOwners;
				const NavigationSystem::SavedNavigationCacheStatus cacheStatus =
					NavigationSystem::Instance().SavedGroundCacheStatus();
				const bool cacheExpectationPassed = [&session, &cacheStatus]() {
					switch(session.options.expectedCacheMode) {
					case CacheExpectation::Absent:
						return !cacheStatus.cacheUsed && cacheStatus.rejectionReason.empty();
					case CacheExpectation::Current:
						return cacheStatus.cacheUsed && cacheStatus.rejectionReason.empty();
					case CacheExpectation::Stale:
					case CacheExpectation::Malformed:
						return !cacheStatus.cacheUsed && !cacheStatus.rejectionReason.empty()
							&& cacheStatus.rejectionReason != "SavedNavigationAwaitingAuthoritativeCapture";
					}
					return false;
				}();
				const bool strictQualificationPassed = !session.options.qualificationRequested
					|| (measuredLongEnough && blockerKeptPace && rawLatencyPassed && ownerPosesStable);
				const bool succeeded = presentationPassed && workerPrepared && workerPoolObserved
					&& agentPartitionsObserved && workloadObserved && cacheExpectationPassed
					&& strictQualificationPassed;
				if(!succeeded) {
					// Failed evidence still owns live agent/direct requests. Drain them
					// through the same protected lifecycle path before reporting failure.
					session.teardownFailure = "The application host did not publish all required navigation or presentation evidence.";
					BeginTeardown(session);
					continue;
				}
				BeginTeardown(session);
				continue;
			}
		}
		return BuildReport(session, false, true);
	}
}
