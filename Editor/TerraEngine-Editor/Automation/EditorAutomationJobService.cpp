// EditorAutomationJobService.cpp - Session job state and retention policy.

#include "EditorAutomationJobService.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Editor::Automation
{
	EditorAutomationJobService::EditorAutomationJobService(std::size_t capacity)
		: maxRetainedJobs(std::max<std::size_t>(capacity, 1))
	{
	}

	AutomationJobSnapshot EditorAutomationJobService::Create(const AutomationJobCreateRequest& request)
	{
		std::scoped_lock lock(jobMutex);
		EvictTerminalJobsLocked();
		if (jobs.size() >= maxRetainedJobs)
		{
			// All retained slots belong to live jobs. Refusing admission keeps the
			// service bounded without ever discarding work a reconnect must find.
			AutomationJobSnapshot rejected;
			rejected.kind = request.kind;
			rejected.capability = request.capability;
			rejected.requestId = request.requestId;
			rejected.correlationId = request.correlationId;
			rejected.state = AutomationJobState::Failed;
			rejected.stage = "retention_capacity";
			rejected.result = nlohmann::json{{"error", "Active job capacity is full."}};
			return rejected;
		}

		AutomationJobSnapshot job;
		job.jobId = "job-" + std::to_string(nextJobId++);
		job.kind = request.kind;
		job.capability = request.capability;
		job.requestId = request.requestId;
		job.correlationId = request.correlationId;
		job.idempotencyKey = request.idempotencyKey;
		job.cancellable = request.cancellable;
		job.retentionSequence = nextRetentionSequence++;
		job.createdAt = std::chrono::system_clock::now();
		job.updatedAt = job.createdAt;
		retentionOrder.push_back(job.jobId);
		jobs.emplace(job.jobId, job);
		return job;
	}

	std::optional<AutomationJobSnapshot> EditorAutomationJobService::Get(std::string_view jobId) const
	{
		std::scoped_lock lock(jobMutex);
		const auto found = jobs.find(std::string(jobId));
		return found == jobs.end() ? std::nullopt : std::optional<AutomationJobSnapshot>(found->second);
	}

	std::vector<AutomationJobSnapshot> EditorAutomationJobService::List(std::size_t limit) const
	{
		std::scoped_lock lock(jobMutex);
		const std::size_t boundedLimit = std::min(limit, maxRetainedJobs);
		std::vector<AutomationJobSnapshot> result;
		result.reserve(std::min(boundedLimit, retentionOrder.size()));
		for (auto it = retentionOrder.rbegin(); it != retentionOrder.rend() && result.size() < boundedLimit; ++it)
		{
			if (const auto found = jobs.find(*it); found != jobs.end())
				result.push_back(found->second);
		}
		return result;
	}

	bool EditorAutomationJobService::SetState(std::string_view jobId, AutomationJobState state, std::string stage)
	{
		std::scoped_lock lock(jobMutex);
		const auto found = jobs.find(std::string(jobId));
		if (found == jobs.end() || IsTerminal(found->second.state) || IsTerminal(state))
			return false;

		AutomationJobSnapshot& job = found->second;
		if ((job.state == AutomationJobState::CancelRequested && state != AutomationJobState::CancelRequested)
			|| static_cast<int>(state) < static_cast<int>(job.state))
			return false;

		job.state = state;
		if (!stage.empty())
			job.stage = std::move(stage);
		if (!job.startedAt && (state == AutomationJobState::Preparing || state == AutomationJobState::Running))
			job.startedAt = std::chrono::system_clock::now();
		job.updatedAt = std::chrono::system_clock::now();
		return true;
	}

	bool EditorAutomationJobService::PublishProgress(std::string_view jobId, double progress, std::string stage)
	{
		if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0)
			return false;
		std::scoped_lock lock(jobMutex);
		const auto found = jobs.find(std::string(jobId));
		if (found == jobs.end() || IsTerminal(found->second.state) || progress < found->second.progress)
			return false;

		found->second.progress = progress;
		if (!stage.empty())
			found->second.stage = std::move(stage);
		found->second.updatedAt = std::chrono::system_clock::now();
		return true;
	}

	bool EditorAutomationJobService::RequestCancellation(std::string_view jobId)
	{
		std::scoped_lock lock(jobMutex);
		const auto found = jobs.find(std::string(jobId));
		if (found == jobs.end() || IsTerminal(found->second.state) || !found->second.cancellable)
			return false;
		found->second.cancelRequested = true;
		found->second.state = AutomationJobState::CancelRequested;
		found->second.stage = "cancel_requested";
		found->second.updatedAt = std::chrono::system_clock::now();
		return true;
	}

	bool EditorAutomationJobService::IsCancellationRequested(std::string_view jobId) const
	{
		std::scoped_lock lock(jobMutex);
		const auto found = jobs.find(std::string(jobId));
		return found != jobs.end() && found->second.cancelRequested;
	}

	bool EditorAutomationJobService::AddDiagnostic(std::string_view jobId, AutomationJobDiagnostic diagnostic)
	{
		std::scoped_lock lock(jobMutex);
		const auto found = jobs.find(std::string(jobId));
		if (found == jobs.end())
			return false;
		found->second.diagnostics.push_back(std::move(diagnostic));
		found->second.updatedAt = std::chrono::system_clock::now();
		return true;
	}

	bool EditorAutomationJobService::AddArtifact(std::string_view jobId, AutomationArtifact artifact)
	{
		std::scoped_lock lock(jobMutex);
		const auto found = jobs.find(std::string(jobId));
		if (found == jobs.end())
			return false;
		found->second.artifacts.push_back(std::move(artifact));
		found->second.updatedAt = std::chrono::system_clock::now();
		return true;
	}

	bool EditorAutomationJobService::CompleteSucceeded(std::string_view jobId, nlohmann::json result)
	{
		return Complete(jobId, AutomationJobState::Succeeded, std::move(result));
	}

	bool EditorAutomationJobService::CompleteFailed(std::string_view jobId, nlohmann::json result)
	{
		return Complete(jobId, AutomationJobState::Failed, std::move(result));
	}

	bool EditorAutomationJobService::CompleteCanceled(std::string_view jobId, nlohmann::json result)
	{
		return Complete(jobId, AutomationJobState::Canceled, std::move(result));
	}

	std::uint64_t EditorAutomationJobService::OldestRetainedSequence() const
	{
		std::scoped_lock lock(jobMutex);
		for (const std::string& jobId : retentionOrder)
		{
			if (const auto found = jobs.find(jobId); found != jobs.end())
				return found->second.retentionSequence;
		}
		return nextRetentionSequence;
	}

	std::size_t EditorAutomationJobService::Size() const
	{
		std::scoped_lock lock(jobMutex);
		return jobs.size();
	}

	bool EditorAutomationJobService::IsTerminal(AutomationJobState state)
	{
		return state == AutomationJobState::Succeeded || state == AutomationJobState::Failed || state == AutomationJobState::Canceled;
	}

	bool EditorAutomationJobService::Complete(std::string_view jobId, AutomationJobState state, nlohmann::json result)
	{
		std::scoped_lock lock(jobMutex);
		const auto found = jobs.find(std::string(jobId));
		if (found == jobs.end() || IsTerminal(found->second.state) || !IsTerminal(state))
			return false;
		AutomationJobSnapshot& job = found->second;
		job.state = state;
		job.stage = state == AutomationJobState::Succeeded ? "completed" : state == AutomationJobState::Canceled ? "canceled" : "failed";
		if (state == AutomationJobState::Succeeded)
			job.progress = 1.0;
		job.result = std::move(result);
		job.updatedAt = std::chrono::system_clock::now();
		job.completedAt = job.updatedAt;
		return true;
	}

	void EditorAutomationJobService::EvictTerminalJobsLocked()
	{
		while (jobs.size() >= maxRetainedJobs)
		{
			auto candidate = std::find_if(retentionOrder.begin(), retentionOrder.end(), [this](const std::string& jobId)
			{
				const auto found = jobs.find(jobId);
				return found == jobs.end() || IsTerminal(found->second.state);
			});
			if (candidate == retentionOrder.end())
				return;
			jobs.erase(*candidate);
			retentionOrder.erase(candidate);
		}
	}
}
