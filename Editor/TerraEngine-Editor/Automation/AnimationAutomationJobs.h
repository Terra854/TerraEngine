// AnimationAutomationJobs.h - durable bounded job adapter for animation work.

#pragma once

#include "EditorAutomationJobService.h"
#include "../Animation/AnimationRecorder.h"

#include <filesystem>
#include <memory>

namespace TerraEngine::Editor::Automation
{
	class AnimationAutomationJobs final
	{
	public:
		explicit AnimationAutomationJobs(EditorAutomationJobService& jobs)
			: jobs_(jobs)
		{
		}

		[[nodiscard]] AutomationDispatchResult Start(const AutomationJobCreateRequest& request);
		[[nodiscard]] AutomationDispatchResult Cancel(std::string_view jobId);
		[[nodiscard]] AutomationDispatchResult Get(std::string_view jobId) const;
		// Recording capture is advanced by an editor-owned protected boundary. Each
		// call consumes at most one immutable AnimationSystem snapshot and mirrors a
		// copied progress/result into the durable job store for reconnecting clients.
		[[nodiscard]] AutomationDispatchResult StartRecording(
			const AutomationJobCreateRequest& request,
			const Animation::AnimationRecorderDescriptor& descriptor,
			const Animation::AnimationRecorderSnapshotBinding& binding,
			std::shared_ptr<const TerraEngine::Animation::AnimationSnapshot> initialSnapshot = {});
		[[nodiscard]] AutomationDispatchResult PumpRecording(
			std::string_view jobId,
			const TerraEngine::Animation::AnimationSnapshot* snapshot);
		// Only the editor automation protected boundary calls this overload. It
		// loads one immutable publication, never live AnimationSystem storage.
		[[nodiscard]] AutomationDispatchResult PumpRecordingFromPublishedSnapshot(
			std::string_view jobId);
		// Called once from the editor's protected post-animation boundary. It advances
		// every retained recording from fresh immutable publications; it is never tied
		// to an MCP poll, UI lifetime, or client connection.
		void PumpActiveRecordingsFromPublishedSnapshots();
		// A typed stop finalizes exactly the samples already captured. It never borrows
		// a live pose or silently extends the caller's declared range.
		[[nodiscard]] AutomationDispatchResult StopRecording(std::string_view jobId);
		// Motion database cooking keeps the existing editor authoring session as the
		// sole compiler owner. The durable job only mirrors copied progress/results
		// so reconnecting MCP clients never borrow UI or worker state.
		[[nodiscard]] AutomationDispatchResult StartMotionDatabaseBuild(
			const AutomationJobCreateRequest& request,
			const std::filesystem::path& authoringPath,
			const std::filesystem::path& outputPath);
		[[nodiscard]] AutomationDispatchResult PumpMotionDatabaseBuild(
			std::string_view jobId);
		// Project replacement is a protected boundary too. Cancel and release every
		// owned recorder before its durable store is reset so an old project cannot
		// retain a capture session or publish into a later project.
		void CancelAllRecordings() noexcept;

	private:
		// Version one uses the contract's conservative default. The discoverable
		// hard ceiling remains 64 for a future explicit project configuration, but
		// callers cannot silently consume that larger budget today.
		static constexpr std::size_t kDefaultConcurrentAnimationJobs = 16U;
		static constexpr std::size_t kHardConcurrentAnimationJobs = 64U;
		EditorAutomationJobService& jobs_;
	};
}
