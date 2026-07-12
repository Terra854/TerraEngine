// EditorAutomationJobService.h - Bounded jobs retained for one editor session.

#pragma once

#include "EditorAutomationTypes.h"

#include <deque>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace TerraEngine::Editor::Automation
{
	class EditorAutomationJobService
	{
	public:
		explicit EditorAutomationJobService(std::size_t capacity = 256);

		[[nodiscard]] AutomationJobSnapshot Create(const AutomationJobCreateRequest& request);
		[[nodiscard]] std::optional<AutomationJobSnapshot> Get(std::string_view jobId) const;
		[[nodiscard]] std::vector<AutomationJobSnapshot> List(std::size_t limit = 100) const;
		[[nodiscard]] bool SetState(std::string_view jobId, AutomationJobState state, std::string stage = {});
		[[nodiscard]] bool PublishProgress(std::string_view jobId, double progress, std::string stage = {});
		[[nodiscard]] bool RequestCancellation(std::string_view jobId);
		[[nodiscard]] bool IsCancellationRequested(std::string_view jobId) const;
		[[nodiscard]] bool AddDiagnostic(std::string_view jobId, AutomationJobDiagnostic diagnostic);
		[[nodiscard]] bool AddArtifact(std::string_view jobId, AutomationArtifact artifact);
		[[nodiscard]] bool CompleteSucceeded(std::string_view jobId, nlohmann::json result = nlohmann::json::object());
		[[nodiscard]] bool CompleteFailed(std::string_view jobId, nlohmann::json result = nlohmann::json::object());
		[[nodiscard]] bool CompleteCanceled(std::string_view jobId, nlohmann::json result = nlohmann::json::object());
		[[nodiscard]] std::uint64_t OldestRetainedSequence() const;
		[[nodiscard]] std::size_t Size() const;

	private:
		[[nodiscard]] static bool IsTerminal(AutomationJobState state);
		[[nodiscard]] bool Complete(std::string_view jobId, AutomationJobState state, nlohmann::json result);
		void EvictTerminalJobsLocked();

		mutable std::mutex jobMutex;
		std::unordered_map<std::string, AutomationJobSnapshot> jobs;
		std::deque<std::string> retentionOrder;
		std::size_t maxRetainedJobs = 256;
		std::uint64_t nextJobId = 1;
		std::uint64_t nextRetentionSequence = 1;
	};
}
