// AnimationAutomationJobs.cpp - reconnect reads retained snapshots by job ID.

#include "AnimationAutomationJobs.h"

#include "Editor/TerraEngine-Editor/Windows/MotionDatabaseAuthoringHost.h"
#include "Engine/Animation/AnimationSystem.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		struct RecordingSession final
		{
			std::shared_ptr<Animation::AnimationRecorder> recorder;
			Animation::AnimationRecorderSnapshotBinding binding;
			TerraEngine::Animation::AnimationGuid skeletonAssetId{};
			TerraEngine::Animation::AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
			std::uint32_t boundGeneration = 0U;
			std::int64_t boundTick = 0;
			std::vector<std::uint64_t> boundJoints;
			TerraEngine::Animation::AnimationGeneration lastPublicationGeneration = 0U;
			TerraEngine::Animation::AnimationFixedTick lastObservedTick = 0;
			TerraEngine::Animation::AnimationFixedTick firstTick = 0;
			TerraEngine::Animation::AnimationFixedTick lastTick = 0;
			std::uint32_t requestedSampleRate = TerraEngine::Animation::kAnimationFixedTicksPerSecond;
		};

		struct MotionDatabaseBuildSession final
		{
			// These objects have no window owner. Their lifetime is instead tied to the
			// durable job service, which prevents a closed or reconfigured editor panel
			// from cancelling an MCP-requested worker build.
			std::unique_ptr<TerraEngine::Editor::Animation::MotionDatabaseProductionAuthoringHost> host;
			std::unique_ptr<TerraEngine::AssetPipeline::MotionDatabaseAuthoringSession> session;
			TerraEngine::AssetPipeline::MotionDatabaseAuthoringDocument document;
			std::filesystem::path outputPath;
		};

		// EditorAutomationService creates a lightweight adapter for each request.
		// Keying sessions by the shared durable store preserves recording ownership
		// across those adapters without making the recorder global to every project.
		std::mutex g_recordingMutex;
		std::unordered_map<EditorAutomationJobService*,
			std::unordered_map<std::string, RecordingSession>> g_recordingSessions;
		std::mutex g_motionDatabaseMutex;
		std::unordered_map<EditorAutomationJobService*,
			std::unordered_map<std::string, MotionDatabaseBuildSession>> g_motionDatabaseSessions;

		bool IsTerminal(AutomationJobState state)
		{
			return state == AutomationJobState::Succeeded
				|| state == AutomationJobState::Failed
				|| state == AutomationJobState::Canceled;
		}

		std::string_view JobStateName(AutomationJobState state)
		{
			switch (state)
			{
			case AutomationJobState::Queued: return "queued";
			case AutomationJobState::Preparing: return "preparing";
			case AutomationJobState::Running: return "running";
			case AutomationJobState::CancelRequested: return "cancel_requested";
			case AutomationJobState::Succeeded: return "succeeded";
			case AutomationJobState::Failed: return "failed";
			case AutomationJobState::Canceled: return "canceled";
			}
			return "unknown";
		}

		nlohmann::json RecordingResultJson(const Animation::AnimationRecorderResult& result)
		{
			return {
				{"outcome", static_cast<std::uint32_t>(result.outcome)},
				{"published", result.published},
				{"candidateValidated", result.candidateValidated},
				{"destinationPath", result.destinationPath.generic_string()},
				{"capturedSamples", result.capturedSamples},
				{"capturedEvents", result.capturedEvents},
				{"retainedKeyCount", result.retainedKeyCount},
				{"removedKeyCount", result.removedKeyCount},
				{"diagnostic", result.diagnostic}
			};
		}

		double RecordingProgress(const Animation::AnimationRecorderProgress& progress)
		{
			return std::clamp(static_cast<double>(progress.overallNormalized), 0.0, 1.0);
		}

		std::string MotionDatabaseStatusName(TerraEngine::Animation::AnimationStatus status)
		{
			return std::string(TerraEngine::Animation::ToString(status));
		}
	}

	AutomationDispatchResult AnimationAutomationJobs::Start(const AutomationJobCreateRequest& request)
	{
		AutomationDispatchResult result;
		if (request.kind.empty() || request.capability.empty() || request.idempotencyKey.empty())
		{
			result.error = ErrorCategory::Validation;
			result.message = "A durable animation job needs a kind, capability, and idempotency key.";
			return result;
		}
		std::size_t activeAnimationJobs = 0U;
		for (const AutomationJobSnapshot& retained : jobs_.List(256U))
		{
			if (!IsTerminal(retained.state) && retained.capability.starts_with("animation."))
			{
				++activeAnimationJobs;
			}
		}
		if (activeAnimationJobs >= kDefaultConcurrentAnimationJobs)
		{
			result.error = ErrorCategory::QueueFull;
			result.message = "The default limit of 16 concurrent animation jobs has been reached; the version-one provider does not silently raise it toward the hard ceiling of 64.";
			return result;
		}

		const AutomationJobSnapshot job = jobs_.Create(request);
		if (job.jobId.empty())
		{
			result.error = ErrorCategory::QueueFull;
			result.message = job.result.value("error", "The bounded editor job store is full.");
			return result;
		}
		result.ok = true;
		result.message = "Created a durable animation job.";
		result.data = {
			{"jobId", job.jobId},
			{"state", JobStateName(job.state)},
			{"correlationId", job.correlationId},
			{"retentionSequence", job.retentionSequence}
		};
		return result;
	}

	AutomationDispatchResult AnimationAutomationJobs::Cancel(std::string_view jobId)
	{
		AutomationDispatchResult result;
		const std::optional<AutomationJobSnapshot> retained = jobs_.Get(jobId);
		if (!retained)
		{
			result.error = ErrorCategory::MissingTarget;
			result.message = "The retained animation job was not found.";
			return result;
		}
		// Animation job tools share the editor's bounded job store with export,
		// profiler, physics, and scripting work. Do not let a coarse animation
		// capability inspect or cancel a job owned by another provider.
		if (!retained->capability.starts_with("animation."))
		{
			result.error = ErrorCategory::MissingTarget;
			result.message = "The retained animation job was not found.";
			return result;
		}
		if (retained->cancelRequested)
		{
			result.ok = true;
			result.message = "Replayed the retained animation cancellation request.";
			result.data = {{"jobId", jobId}, {"cancelRequested", true}, {"idempotencyDisposition", "replay"}};
			return result;
		}
		{
			std::scoped_lock recordingLock(g_recordingMutex);
			const auto owner = g_recordingSessions.find(&jobs_);
			if(owner != g_recordingSessions.end())
			{
				const auto recording = owner->second.find(std::string(jobId));
				if(recording != owner->second.end())
				{
					(void)recording->second.recorder->Cancel();
				}
			}
		}
		{
			std::scoped_lock motionLock(g_motionDatabaseMutex);
			const auto owner = g_motionDatabaseSessions.find(&jobs_);
			if(owner != g_motionDatabaseSessions.end())
			{
				const auto motion = owner->second.find(std::string(jobId));
				if(motion != owner->second.end())
				{
					// The session's cancellation token is observed by source resolution and
					// cooking work before its atomic publish callback can run.
					motion->second.session->CancelBuild();
				}
			}
		}
		if (IsTerminal(retained->state) || !retained->cancellable || !jobs_.RequestCancellation(jobId))
		{
			result.error = ErrorCategory::Conflict;
			result.message = "The retained animation job is no longer cancellable.";
			return result;
		}
		result.ok = true;
		result.message = "Requested animation job cancellation at its next safe boundary.";
		result.data = {{"jobId", jobId}, {"cancelRequested", true}, {"idempotencyDisposition", "new"}};
		return result;
	}

	AutomationDispatchResult AnimationAutomationJobs::Get(std::string_view jobId) const
	{
		// Job reads are the reconnect-safe polling boundary for motion builds. The
		// pump performs no synchronous cooking; it only observes a completed worker
		// future and performs the session's checked atomic publication step.
		(void)const_cast<AnimationAutomationJobs*>(this)->PumpMotionDatabaseBuild(jobId);
		AutomationDispatchResult result;
		const std::optional<AutomationJobSnapshot> job = jobs_.Get(jobId);
		if (!job)
		{
			result.error = ErrorCategory::MissingTarget;
			result.message = "The retained animation job was not found.";
			return result;
		}
		// Return the same non-enumerating error for foreign jobs so clients cannot
		// use this provider as a side channel into unrelated editor work.
		if (!job->capability.starts_with("animation."))
		{
			result.error = ErrorCategory::MissingTarget;
			result.message = "The retained animation job was not found.";
			return result;
		}
		result.ok = true;
		result.message = "Read the retained animation job snapshot.";
		nlohmann::json diagnostics = nlohmann::json::array();
		for (const AutomationJobDiagnostic& diagnostic : job->diagnostics)
		{
			diagnostics.push_back({
				{"severity", static_cast<std::uint32_t>(diagnostic.severity)},
				{"category", diagnostic.category},
				{"message", diagnostic.message},
				{"details", diagnostic.details}
			});
		}
		nlohmann::json artifacts = nlohmann::json::array();
		for (const AutomationArtifact& artifact : job->artifacts)
		{
			artifacts.push_back({
				{"artifactId", artifact.artifactId},
				{"kind", artifact.kind},
				{"displayPath", artifact.displayPath},
				{"mimeType", artifact.mimeType},
				{"metadata", artifact.metadata}
			});
		}
		result.data = {
			{"jobId", job->jobId},
			{"state", JobStateName(job->state)},
			{"progress", job->progress},
			{"stage", job->stage},
			{"cancelRequested", job->cancelRequested},
			{"cancellable", job->cancellable},
			{"retentionSequence", job->retentionSequence},
			{"diagnostics", std::move(diagnostics)},
			{"artifacts", std::move(artifacts)},
			{"result", job->result}
		};
		return result;
	}

	AutomationDispatchResult AnimationAutomationJobs::StartMotionDatabaseBuild(
		const AutomationJobCreateRequest& request,
		const std::filesystem::path& authoringPath,
		const std::filesystem::path& outputPath)
	{
		AutomationDispatchResult result;
		if(request.capability != "animation.motion_database.build"
			|| authoringPath.extension() != ".json" || outputPath.extension() != ".temotiondb")
		{
			result.error = ErrorCategory::Validation;
			result.message = "Motion database builds require an authoring JSON sidecar and a .temotiondb output.";
			return result;
		}
		TerraEngine::AssetPipeline::MotionDatabaseAuthoringDocument document;
		const TerraEngine::Animation::AnimationStatus loaded =
			TerraEngine::AssetPipeline::LoadMotionDatabaseAuthoringDocument(authoringPath, document);
		if(loaded != TerraEngine::Animation::AnimationStatus::Ok)
		{
			result.error = loaded == TerraEngine::Animation::AnimationStatus::MissingAsset
				? ErrorCategory::MissingTarget : ErrorCategory::Validation;
			result.message = "The motion database authoring sidecar could not be loaded: "
				+ MotionDatabaseStatusName(loaded) + ".";
			return result;
		}
		auto host = std::make_unique<TerraEngine::Editor::Animation::MotionDatabaseProductionAuthoringHost>();
		auto session = std::make_unique<TerraEngine::AssetPipeline::MotionDatabaseAuthoringSession>(
			host->GetMotionDatabaseCompileExecutor());
		const TerraEngine::Animation::AnimationStatus queued = session->RequestBuild(document,
			host->CreateMotionDatabaseSourceResolver(document), outputPath);
		if(queued != TerraEngine::Animation::AnimationStatus::Queued)
		{
			result.error = queued == TerraEngine::Animation::AnimationStatus::MissingAsset
				? ErrorCategory::MissingTarget : ErrorCategory::Validation;
			result.message = "The motion database build was rejected before worker admission: "
				+ MotionDatabaseStatusName(queued) + ".";
			return result;
		}
		// Preserve the externally reported authoring identity before moving the
		// immutable input document into the job-owned session record.
		const std::uint64_t authoringGeneration = document.authoringGeneration;
		const AutomationDispatchResult started = Start(request);
		if(!started.ok)
		{
			session->CancelBuild();
			return started;
		}
		const std::string jobId = started.data.value("jobId", std::string{});
		(void)jobs_.SetState(jobId, AutomationJobState::Running, "motion_database_build");
		{
			std::scoped_lock motionLock(g_motionDatabaseMutex);
			g_motionDatabaseSessions[&jobs_].emplace(jobId, MotionDatabaseBuildSession{
				std::move(host), std::move(session), std::move(document), outputPath});
		}
		AutomationDispatchResult accepted = started;
		accepted.message = "Queued the copied motion database build for worker cooking and checked atomic publication.";
		accepted.data["authoringGeneration"] = authoringGeneration;
		accepted.data["outputPath"] = outputPath.generic_string();
		accepted.data["visibilityBoundary"] = "motion_database_worker_then_atomic_publish";
		return accepted;
	}

	AutomationDispatchResult AnimationAutomationJobs::PumpMotionDatabaseBuild(
		std::string_view jobId)
	{
		AutomationDispatchResult result;
		std::scoped_lock motionLock(g_motionDatabaseMutex);
		const auto owner = g_motionDatabaseSessions.find(&jobs_);
		if(owner == g_motionDatabaseSessions.end()) return result;
		const auto found = owner->second.find(std::string(jobId));
		if(found == owner->second.end()) return result;
		MotionDatabaseBuildSession& build = found->second;
		if(jobs_.IsCancellationRequested(jobId)) build.session->CancelBuild();
		const TerraEngine::AssetPipeline::MotionDatabaseBuildProgress progress = build.session->GetBuildProgress();
		(void)jobs_.PublishProgress(jobId, std::clamp(static_cast<double>(progress.Fraction()), 0.0, 1.0),
			"motion_database_build");
		const TerraEngine::Animation::AnimationStatus status = build.session->Poll(build.document);
		if(status == TerraEngine::Animation::AnimationStatus::NotReady || status == TerraEngine::Animation::AnimationStatus::Queued)
		{
			result.ok = true;
			return result;
		}
		if(status == TerraEngine::Animation::AnimationStatus::Ok)
		{
			const std::optional<TerraEngine::AssetPipeline::MotionDatabasePublishedRecord> published =
				build.session->GetPublished();
			if(!published || !published->asset)
			{
				(void)jobs_.CompleteFailed(jobId, {{"status", "missing_published_record"}});
			}
			else
			{
				build.host->NotifyMotionDatabasePublished(build.outputPath, published->asset->assetId);
				(void)jobs_.CompleteSucceeded(jobId, {{"status", "published"},
					{"assetId", TerraEngine::ToHexString(published->asset->assetId.value)},
					{"databaseGeneration", published->generation},
					{"outputPath", build.outputPath.generic_string()}});
			}
		}
		else if(status == TerraEngine::Animation::AnimationStatus::Cancelled)
		{
			(void)jobs_.CompleteCanceled(jobId, {{"status", "cancelled"},
				{"lastGoodPreserved", true}});
		}
		else
		{
			(void)jobs_.CompleteFailed(jobId, {{"status", MotionDatabaseStatusName(status)},
				{"lastGoodPreserved", true}});
		}
		owner->second.erase(found);
		if(owner->second.empty()) g_motionDatabaseSessions.erase(owner);
		result.ok = true;
		return result;
	}

	AutomationDispatchResult AnimationAutomationJobs::StartRecording(
		const AutomationJobCreateRequest& request,
		const Animation::AnimationRecorderDescriptor& descriptor,
		const Animation::AnimationRecorderSnapshotBinding& binding,
		std::shared_ptr<const TerraEngine::Animation::AnimationSnapshot> initialSnapshot)
	{
		if(request.capability != "animation.recording.finalize")
		{
			AutomationDispatchResult result;
			result.error = ErrorCategory::Validation;
			result.message = "Recording jobs must use the animation.recording.finalize capability.";
			return result;
		}
		const AutomationDispatchResult started = Start(request);
		if(!started.ok) return started;
		const std::string jobId = started.data.value("jobId", std::string{});
		auto recorder = std::make_shared<Animation::AnimationRecorder>();
		if(recorder->Begin(descriptor) != Animation::AnimationAuthoringStatus::Ok)
		{
			const Animation::AnimationRecorderResult recorderResult = recorder->Result();
			(void)jobs_.CompleteFailed(jobId, RecordingResultJson(recorderResult));
			// Return the durable ID even for a descriptor failure. Callers can then
			// read the retained diagnostic using the same job-get flow as a completed
			// recording instead of losing the outcome at the start boundary.
			AutomationDispatchResult failed = started;
			failed.ok = false;
			failed.error = ErrorCategory::Validation;
			failed.message = recorderResult.diagnostic.empty()
				? "The bounded recording descriptor was rejected."
				: recorderResult.diagnostic;
			failed.data["state"] = "failed";
			failed.data["result"] = RecordingResultJson(recorderResult);
			return failed;
		}
		// Admission retains the approval-time immutable snapshot as sample zero. A
		// recording cannot begin from a guessed generation, and later frames must be
		// strictly newer before they are copied into its private staging directory.
		if(!initialSnapshot || initialSnapshot->animator != binding.animator
			|| initialSnapshot->publicationGeneration != binding.expectedPublicationGeneration
			|| initialSnapshot->tick != descriptor.firstTick)
		{
			(void)recorder->Cancel();
			(void)jobs_.CompleteFailed(jobId, {{"status", "approval_snapshot_mismatch"}});
			return {false, ErrorCategory::StaleTarget,
				"Recording approval must supply the immutable first sample at the declared firstTick.", {"jobId", jobId}};
		}
		if(recorder->CapturePublishedSnapshot(*initialSnapshot, binding) != Animation::AnimationAuthoringStatus::Ok)
		{
			const Animation::AnimationRecorderResult failed = recorder->Result();
			(void)jobs_.CompleteFailed(jobId, RecordingResultJson(failed));
			return {false, ErrorCategory::Validation,
				"The immutable approval snapshot did not satisfy the typed recording channels.", {"jobId", jobId}};
		}
		(void)jobs_.SetState(jobId, AutomationJobState::Running, "recording_capture");
		std::scoped_lock recordingLock(g_recordingMutex);
		g_recordingSessions[&jobs_].emplace(jobId, RecordingSession{
			std::move(recorder), binding, descriptor.skeletonAssetId, descriptor.compatibilityFingerprint,
			binding.expectedPublicationGeneration, initialSnapshot->tick, binding.jointStableIds,
			binding.expectedPublicationGeneration, initialSnapshot->tick, descriptor.firstTick, descriptor.lastTick,
			descriptor.sampleRate});
		return started;
	}

	AutomationDispatchResult AnimationAutomationJobs::PumpRecording(
		std::string_view jobId, const TerraEngine::Animation::AnimationSnapshot* snapshot)
	{
		AutomationDispatchResult result;
		std::shared_ptr<Animation::AnimationRecorder> recorder;
		Animation::AnimationRecorderSnapshotBinding binding;
		std::uint32_t boundGeneration = 0U; std::int64_t boundTick = 0; std::vector<std::uint64_t> boundJoints;
		TerraEngine::Animation::AnimationGeneration lastPublicationGeneration = 0U;
		TerraEngine::Animation::AnimationFixedTick lastObservedTick = 0;
		TerraEngine::Animation::AnimationFixedTick firstTick = 0;
		TerraEngine::Animation::AnimationFixedTick lastTick = 0;
		std::uint32_t requestedSampleRate = TerraEngine::Animation::kAnimationFixedTicksPerSecond;
		{
			std::scoped_lock recordingLock(g_recordingMutex);
			const auto owner = g_recordingSessions.find(&jobs_);
			if(owner == g_recordingSessions.end())
			{
				result.error = ErrorCategory::MissingTarget;
				result.message = "The active recording session was not found.";
				return result;
			}
			const auto session = owner->second.find(std::string(jobId));
			if(session == owner->second.end())
			{
				result.error = ErrorCategory::MissingTarget;
				result.message = "The active recording session was not found.";
				return result;
			}
			recorder = session->second.recorder;
			binding = session->second.binding;
			boundGeneration = session->second.boundGeneration; boundTick = session->second.boundTick; boundJoints = session->second.boundJoints;
			lastPublicationGeneration = session->second.lastPublicationGeneration;
			lastObservedTick = session->second.lastObservedTick;
			firstTick = session->second.firstTick;
			lastTick = session->second.lastTick;
			requestedSampleRate = session->second.requestedSampleRate;
		}
		const bool cancellationRequested = jobs_.IsCancellationRequested(jobId);
		if(cancellationRequested) (void)recorder->Cancel();
		Animation::AnimationAuthoringStatus status = Animation::AnimationAuthoringStatus::Ok;
		if(snapshot == nullptr && !cancellationRequested)
		{
			// A missing, stale, or mismatched publication cannot be repaired by waiting:
			// fail the complete session before it can stage a partial replacement clip.
			status = Animation::AnimationAuthoringStatus::Stale;
		}
		else if(snapshot != nullptr && !cancellationRequested)
		{
			if(snapshot->publicationGeneration <= lastPublicationGeneration || snapshot->tick <= lastObservedTick)
			{
				// Repeated frame publications are harmless. A regressing generation/tick is
				// rejected by the caller that supplies the immutable publication details.
				result.ok = true;
				return result;
			}
			const bool reachedRangeEnd = snapshot->tick >= lastTick;
			const bool inRange = snapshot->tick >= firstTick && snapshot->tick <= lastTick;
			const std::uint64_t elapsed = inRange
				? static_cast<std::uint64_t>(snapshot->tick - firstTick) : 0U;
			// A rational phase accumulator avoids truncating 60 / rate. For example,
			// 24 Hz chooses fixed ticks 0, 3, 5, 8... deterministically. Rates above
			// the 60 Hz immutable-publication cadence capture every available snapshot.
			const std::uint64_t fixedRate = TerraEngine::Animation::kAnimationFixedTicksPerSecond;
			const bool sampleDue = inRange && (elapsed == 0U || requestedSampleRate >= fixedRate
				|| (elapsed * requestedSampleRate) / fixedRate
					> ((elapsed - 1U) * requestedSampleRate) / fixedRate);
			if(!sampleDue)
			{
				// A fresh publication between requested sample instants is observed but not
				// staged. Event channels follow the same selected-sample contract: this
				// format has no event-only record, so no synthetic numeric key is invented.
				std::scoped_lock recordingLock(g_recordingMutex);
				if(auto owner = g_recordingSessions.find(&jobs_); owner != g_recordingSessions.end())
					if(auto current = owner->second.find(std::string(jobId)); current != owner->second.end()) {
						current->second.lastPublicationGeneration = snapshot->publicationGeneration;
						current->second.lastObservedTick = snapshot->tick;
					}
				if(reachedRangeEnd)
				{
					// Reaching or passing an unaligned end tick closes the range without
					// sending that non-sample publication back through the recorder after
					// it has already finalized its transactional output.
					status = recorder->Finalize();
				}
				else
				{
					result.ok = true;
					return result;
				}
			}
			else
			{
				binding.expectedPublicationGeneration = snapshot->publicationGeneration;
				status = recorder->CapturePublishedSnapshot(*snapshot, binding);
				if(status == Animation::AnimationAuthoringStatus::Ok)
				{
					std::scoped_lock recordingLock(g_recordingMutex);
					if(auto owner = g_recordingSessions.find(&jobs_); owner != g_recordingSessions.end())
						if(auto current = owner->second.find(std::string(jobId)); current != owner->second.end())
						{
							current->second.lastPublicationGeneration = snapshot->publicationGeneration;
							current->second.lastObservedTick = snapshot->tick;
						}
				}
			}
		}
		if(status == Animation::AnimationAuthoringStatus::Ok && snapshot != nullptr && snapshot->tick >= lastTick
			&& recorder->Progress().active)
		{
			status = recorder->Finalize();
		}
		const Animation::AnimationRecorderProgress progress = recorder->Progress();
		const Animation::AnimationRecorderResult recorderResult = recorder->Result();
		auto resultWithBinding = [&]() {
			nlohmann::json value = RecordingResultJson(recorderResult);
			value["boundPublicationGeneration"] = boundGeneration;
			value["boundTick"] = boundTick;
			value["boundJointStableIds"] = boundJoints;
			return value;
		};
		(void)jobs_.PublishProgress(jobId, RecordingProgress(progress), "recording_" + std::to_string(static_cast<std::uint32_t>(progress.stage)));
		if(status == Animation::AnimationAuthoringStatus::Canceled || recorderResult.outcome == Animation::AnimationRecorderOutcome::Canceled)
		{
			(void)jobs_.CompleteCanceled(jobId, resultWithBinding());
		}
		else if(recorderResult.outcome == Animation::AnimationRecorderOutcome::Failed)
		{
			(void)jobs_.CompleteFailed(jobId, resultWithBinding());
		}
		else if(recorderResult.outcome == Animation::AnimationRecorderOutcome::Published)
		{
			(void)jobs_.CompleteSucceeded(jobId, resultWithBinding());
		}
		else if(status != Animation::AnimationAuthoringStatus::Ok)
		{
			// A stale binding or malformed copied pose cannot become valid by waiting;
			// fail closed and preserve both the source clip and prior destination.
			nlohmann::json failure = resultWithBinding();
			if(failure.value("diagnostic", std::string{}).empty())
			{
				failure["diagnostic"] = "The immutable recording snapshot no longer satisfies its copied binding.";
			}
			(void)jobs_.CompleteFailed(jobId, std::move(failure));
		}
		if(recorderResult.outcome != Animation::AnimationRecorderOutcome::None
			|| status != Animation::AnimationAuthoringStatus::Ok)
		{
			std::scoped_lock recordingLock(g_recordingMutex);
			const auto owner = g_recordingSessions.find(&jobs_);
			if(owner != g_recordingSessions.end()) owner->second.erase(std::string(jobId));
		}
		result.ok = status == Animation::AnimationAuthoringStatus::Ok
			|| status == Animation::AnimationAuthoringStatus::Canceled;
		if(!result.ok) result.error = ErrorCategory::Validation;
		result.message = recorderResult.diagnostic;
		result.data = resultWithBinding();
		result.data["progress"] = RecordingProgress(progress);
		return result;
	}

	AutomationDispatchResult AnimationAutomationJobs::PumpRecordingFromPublishedSnapshot(
		std::string_view jobId)
	{
		Animation::AnimationRecorderSnapshotBinding binding;
		TerraEngine::Animation::AnimationGuid expectedSkeleton{};
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibility = 0U;
		TerraEngine::Animation::AnimationGeneration lastGeneration = 0U;
		TerraEngine::Animation::AnimationFixedTick lastTick = 0;
		{
			std::scoped_lock recordingLock(g_recordingMutex);
			const auto owner = g_recordingSessions.find(&jobs_);
			if(owner == g_recordingSessions.end()) {
				AutomationDispatchResult result;
				result.error = ErrorCategory::MissingTarget;
				result.message = "The active recording session was not found.";
				return result;
			}
			const auto session = owner->second.find(std::string(jobId));
			if(session == owner->second.end()) {
				AutomationDispatchResult result;
				result.error = ErrorCategory::MissingTarget;
				result.message = "The active recording session was not found.";
				return result;
			}
			binding = session->second.binding;
			expectedSkeleton = session->second.skeletonAssetId;
			expectedCompatibility = session->second.compatibilityFingerprint;
			lastGeneration = session->second.lastPublicationGeneration;
			lastTick = session->second.lastObservedTick;
		}
		if(jobs_.IsCancellationRequested(jobId))
		{
			// Cancellation is a terminal safe boundary, so it must be published even
			// while paused Play Mode keeps exposing the same immutable fixed tick.
			// Passing no sample lets PumpRecording retain the exact approval binding,
			// discard staged output, and complete the durable job as canceled.
			return PumpRecording(jobId, nullptr);
		}
		const std::shared_ptr<const TerraEngine::Animation::AnimationAuthorityFrame> authority =
			TerraEngine::Animation::AnimationSystem::Instance().LoadPublishedAuthorityFrame();
		const TerraEngine::Animation::AnimationAuthorityRow* published = authority
			? authority->Find(binding.animator) : nullptr;
		if(!published || !published->snapshot || published->key.Handle() != binding.animator
			|| published->skeleton.assetId != expectedSkeleton
			|| published->skeletonCompatibilityFingerprint != expectedCompatibility
			|| published->jointStableIds != binding.jointStableIds)
		{
			return PumpRecording(jobId, nullptr);
		}
		const auto& snapshot = published->snapshot;
		// Paused Play Mode can republish the same completed fixed simulation tick
		// while editor-only work updates the publication generation. That is not a
		// recording sample. A later fixed tick may also retain the same pose generation
		// when the actor holds an unchanged pose; that later tick is a valid held sample.
		// Reject only a real regression in either monotonic clock. Treating equality as
		// stale used to erase an approved session while its Stop approval was waiting.
		if(snapshot->tick == lastTick) {
			AutomationDispatchResult duplicate; duplicate.ok = true; return duplicate;
		}
		if(snapshot->tick < lastTick || snapshot->publicationGeneration < lastGeneration) {
			return PumpRecording(jobId, nullptr);
		}
		return PumpRecording(jobId, snapshot.get());
	}

	void AnimationAutomationJobs::PumpActiveRecordingsFromPublishedSnapshots()
	{
		std::vector<std::string> jobIds;
		{
			std::scoped_lock recordingLock(g_recordingMutex);
			if(const auto owner = g_recordingSessions.find(&jobs_); owner != g_recordingSessions.end())
				for(const auto& [jobId, session] : owner->second) { (void)session; jobIds.push_back(jobId); }
		}
		for(const std::string& jobId : jobIds) (void)PumpRecordingFromPublishedSnapshot(jobId);
	}

	AutomationDispatchResult AnimationAutomationJobs::StopRecording(std::string_view jobId)
	{
		std::shared_ptr<Animation::AnimationRecorder> recorder;
		std::uint32_t boundGeneration = 0U;
		std::int64_t boundTick = 0;
		std::vector<std::uint64_t> boundJoints;
		{
			std::scoped_lock recordingLock(g_recordingMutex);
			const auto owner = g_recordingSessions.find(&jobs_);
			if(owner == g_recordingSessions.end() || !owner->second.contains(std::string(jobId)))
				return {false, ErrorCategory::MissingTarget, "The active recording session was not found.", {}};
			const RecordingSession& session = owner->second.at(std::string(jobId));
			recorder = session.recorder;
			boundGeneration = session.boundGeneration;
			boundTick = session.boundTick;
			boundJoints = session.boundJoints;
		}
		const Animation::AnimationAuthoringStatus status = recorder->Finalize();
		const Animation::AnimationRecorderResult recorderResult = recorder->Result();
		auto resultWithBinding = [&]() {
			nlohmann::json value = RecordingResultJson(recorderResult);
			value["boundPublicationGeneration"] = boundGeneration;
			value["boundTick"] = boundTick;
			value["boundJointStableIds"] = boundJoints;
			return value;
		};
		if(recorderResult.outcome == Animation::AnimationRecorderOutcome::Published)
			(void)jobs_.CompleteSucceeded(jobId, resultWithBinding());
		else if(recorderResult.outcome == Animation::AnimationRecorderOutcome::Canceled)
			(void)jobs_.CompleteCanceled(jobId, resultWithBinding());
		else
			(void)jobs_.CompleteFailed(jobId, resultWithBinding());
		{
			std::scoped_lock recordingLock(g_recordingMutex);
			if(auto owner = g_recordingSessions.find(&jobs_); owner != g_recordingSessions.end()) owner->second.erase(std::string(jobId));
		}
		AutomationDispatchResult result; result.ok = status == Animation::AnimationAuthoringStatus::Ok;
		result.error = result.ok ? ErrorCategory::Internal : ErrorCategory::Validation;
		result.message = result.ok ? "Finalized the recording at the explicit typed stop boundary." : recorderResult.diagnostic;
		result.data = resultWithBinding(); result.data["jobId"] = jobId; return result;
	}

	void AnimationAutomationJobs::CancelAllRecordings() noexcept
	{
		{
			std::scoped_lock recordingLock(g_recordingMutex);
			const auto owner = g_recordingSessions.find(&jobs_);
			if(owner != g_recordingSessions.end())
			{
				for(auto& [jobId, session] : owner->second)
				{
					// Cancellation is intentionally best-effort here: the durable store is
					// about to reset, while releasing the recorder prevents old captures from
					// retaining copied pose data beyond the project lifetime.
					(void)jobId;
					(void)session.recorder->Cancel();
				}
				g_recordingSessions.erase(owner);
			}
		}
		{
			std::scoped_lock motionLock(g_motionDatabaseMutex);
			const auto owner = g_motionDatabaseSessions.find(&jobs_);
			if(owner != g_motionDatabaseSessions.end())
			{
				for(auto& [jobId, session] : owner->second)
				{
					// Project reset must not let a detached worker publish into the next
					// project generation. The session retains the last known-good output.
					(void)jobId;
					session.session->CancelBuild();
				}
				g_motionDatabaseSessions.erase(owner);
			}
		}
	}
}
