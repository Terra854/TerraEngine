// AnimationRuntimeOperations.h - typed runtime animation automation adapter.

#pragma once

#include "AnimationAutomationTransactions.h"
#include "../Animation/AnimationRecorder.h"

#include "Engine/Animation/Diagnostics/AnimationDiagnostics.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace TerraEngine::Editor::Automation { class AnimationAutomationJobs; }

namespace TerraEngine::Editor::Automation
{
	// Holds the automation service's opt-in detailed diagnostics request between
	// RPC calls. A request cannot advance the editor frame itself, so a local
	// RAII value would be released before the next protected publication can
	// produce the requested copied rows.
	class AnimationAutomationDetailedDiagnosticsLease final
	{
	public:
		[[nodiscard]] bool EnsureRequested() noexcept;
		[[nodiscard]] bool IsRequested() const noexcept;
		void Reset() noexcept;

	private:
		std::optional<TerraEngine::Animation::AnimationDetailedDiagnosticsRequest> request_;
	};

	// Runtime operations consume bounded JSON but publish only through typed engine
	// queues or immutable read surfaces. No caller receives live animator storage.
	[[nodiscard]] AutomationDispatchResult ExecuteAnimationRuntimeOperation(
		std::string_view operation,
		const nlohmann::json& arguments,
		AnimationAutomationTransactions& transactions,
		std::uint64_t generation,
		const std::function<AutomationDispatchResult()>& protectedCommit = {},
		AnimationAutomationJobs* recordingJobs = nullptr,
		const std::filesystem::path& recordingAssetRoot = {},
		AnimationAutomationDetailedDiagnosticsLease* detailedDiagnosticsLease = nullptr);
	// Copy one already-published runtime snapshot into a recorder. This adapter is
	// deliberately separate from JSON command handling so automation/job owners
	// can call it only at their declared protected capture boundary.
	[[nodiscard]] AutomationDispatchResult CaptureAnimationRecordingSnapshot(
		Animation::AnimationRecorder& recorder,
		const Animation::AnimationRecorderSnapshotBinding& binding);
}
