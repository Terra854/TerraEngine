// EditorProfilerService.cpp — Editor profiling service implementation
//
// Implements the profiling session management logic, including background tasks for saving and loading large capture documents. It integrates with the engine's main thread pool to ensure that capture processing doesn't block the editor UI.

#include "EditorProfilerService.h"

#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/ThreadPool/ThreadPool.h"

#include <future>

using TerraEngine::Profiling::Profiler;
using TerraEngine::Profiling::ProfilerCaptureDocument;
using TerraEngine::Profiling::ProfilerCaptureSettings;
using TerraEngine::Profiling::ProfilerSessionState;

// EditorProfilerService::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Initialize(EditorContext& context)
{
	ctx = &context;
	OnProjectOpened();
}

// EditorProfilerService::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Shutdown()
{
	OnProjectClosed();
	ctx = nullptr;
}

// EditorProfilerService::OnProjectOpened is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::OnProjectOpened()
{
	if (!ctx || !ctx->projectManager || !ctx->projectManager->HasActiveProject())
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		++projectGeneration;
		if (cancellationToken)
		{
			cancellationToken->store(true, std::memory_order_release);
		}
		cancellationToken = std::make_shared<std::atomic_bool>(false);
		captureStore.SetProjectRoot(ctx->projectManager->GetProjectRoot());
		statusText = "Profiler ready";
		lastError.clear();
	}
	RefreshCaptureList();
}

// EditorProfilerService::OnProjectClosed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::OnProjectClosed()
{
	Profiler::Instance().Clear();
	std::lock_guard<std::mutex> lock(mutex);
	++projectGeneration;
	if (cancellationToken)
	{
		cancellationToken->store(true, std::memory_order_release);
	}
	captureStore.ClearProjectRoot();
	captures.clear();
	busy = false;
	statusText = "No project open";
	lastError.clear();
}

// EditorProfilerService::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Update(float)
{
}

// EditorProfilerService::StartRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::StartRecording()
{
	if (!HasActiveProject())
	{
		std::lock_guard<std::mutex> lock(mutex);
		lastError = "Open a project before profiling.";
		statusText = lastError;
		return;
	}

	ProfilerCaptureSettings settings{};
	settings.maxFrames = 10000u;
	settings.maxDurationSeconds = 0.0;
	Profiler::Instance().StartRecording(MakeMetadata(), settings);
	std::lock_guard<std::mutex> lock(mutex);
	statusText = "Recording";
	lastError.clear();
}

// EditorProfilerService::PauseRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::PauseRecording()
{
	Profiler::Instance().PauseRecording();
	std::lock_guard<std::mutex> lock(mutex);
	statusText = "Paused";
}

// EditorProfilerService::ResumeRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::ResumeRecording()
{
	Profiler::Instance().ResumeRecording();
	std::lock_guard<std::mutex> lock(mutex);
	statusText = "Recording";
}

// EditorProfilerService::FinishCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::FinishCapture()
{
	if (!HasActiveProject())
	{
		return;
	}

	ProfilerCaptureDocument document = Profiler::Instance().FinishRecording();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (document.frames.empty())
	{
		std::lock_guard<std::mutex> lock(mutex);
		lastError = "No frames were captured.";
		statusText = lastError;
		return;
	}

	std::uint64_t generation = 0u;
	ProfilerCaptureStore storeSnapshot;
	std::shared_ptr<std::atomic_bool> cancelToken;
	{
		std::lock_guard<std::mutex> lock(mutex);
		generation = projectGeneration;
		storeSnapshot = captureStore;
		cancelToken = cancellationToken;
		busy = true;
		statusText = "Saving profiler capture";
		lastError.clear();
	}

	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	auto task = ThreadPool::instance().createTask([weakSelf, storeSnapshot, cancelToken, document, generation]() mutable {
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}

		std::filesystem::path savedPath;
		std::string error;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!storeSnapshot.SaveNewCapture(document, savedPath, error))
		{
			std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
			if (!self)
			{
				return;
			}
			std::lock_guard<std::mutex> lock(self->mutex);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
			{
				self->busy = false;
				self->lastError = error;
				self->statusText = "Failed to save capture";
			}
			return;
		}
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}

		ProfilerCaptureDocument loadedDocument;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!storeSnapshot.LoadCapture(savedPath, loadedDocument, error))
		{
			std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
			if (!self)
			{
				return;
			}
			std::lock_guard<std::mutex> lock(self->mutex);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
			{
				self->busy = false;
				self->lastError = error;
				self->statusText = "Saved capture but failed to open it";
			}
			return;
		}
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}

		std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> refreshedCaptures = storeSnapshot.ListCaptures(error, [cancelToken]() {
			return cancelToken && cancelToken->load(std::memory_order_acquire);
		});
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}
		Profiler::Instance().LoadDocument(loadedDocument);

		std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
		if (!self)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(self->mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
		{
			self->captures = std::move(refreshedCaptures);
			self->busy = false;
			self->lastError = error;
			self->statusText = "Saved and opened capture";
		}
	});
	ThreadPool::instance().enqueue(task);
}

// EditorProfilerService::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::Clear()
{
	Profiler::Instance().Clear();
	const bool hasProject = HasActiveProject();
	std::lock_guard<std::mutex> lock(mutex);
	statusText = hasProject ? "Profiler ready" : "No project open";
	lastError.clear();
}

// EditorProfilerService::RefreshCaptureList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::RefreshCaptureList()
{
	if (!HasActiveProject())
	{
		return;
	}

	std::uint64_t generation = 0u;
	ProfilerCaptureStore storeSnapshot;
	std::shared_ptr<std::atomic_bool> cancelToken;
	{
		std::lock_guard<std::mutex> lock(mutex);
		generation = projectGeneration;
		storeSnapshot = captureStore;
		cancelToken = cancellationToken;
		busy = true;
		statusText = "Refreshing capture list";
	}

	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	auto task = ThreadPool::instance().createTask([weakSelf, storeSnapshot, cancelToken, generation]() {
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}
		std::string error;
		auto refreshed = storeSnapshot.ListCaptures(error, [cancelToken]() {
			return cancelToken && cancelToken->load(std::memory_order_acquire);
		});
		std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
		if (!self)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(self->mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
		{
			self->captures = std::move(refreshed);
			self->lastError = error;
			self->statusText = error.empty() ? "Capture list refreshed" : error;
			self->busy = false;
		}
	});
	ThreadPool::instance().enqueue(task);
}

// EditorProfilerService::LoadCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorProfilerService::LoadCapture(const std::filesystem::path& capturePath)
{
	if (!HasActiveProject())
	{
		return;
	}

	std::uint64_t generation = 0u;
	ProfilerCaptureStore storeSnapshot;
	std::shared_ptr<std::atomic_bool> cancelToken;
	{
		std::lock_guard<std::mutex> lock(mutex);
		generation = projectGeneration;
		storeSnapshot = captureStore;
		cancelToken = cancellationToken;
		busy = true;
		statusText = "Opening profiler capture";
		lastError.clear();
	}

	std::weak_ptr<EditorProfilerService> weakSelf = weak_from_this();
	auto task = ThreadPool::instance().createTask([weakSelf, storeSnapshot, cancelToken, generation, capturePath]() {
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}
		ProfilerCaptureDocument document;
		std::string error;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!storeSnapshot.LoadCapture(capturePath, document, error))
		{
			std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
			if (!self)
			{
				return;
			}
			std::lock_guard<std::mutex> lock(self->mutex);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
			{
				self->lastError = error;
				self->statusText = "Failed to open capture";
				self->busy = false;
			}
			return;
		}
		if (cancelToken && cancelToken->load(std::memory_order_acquire))
		{
			return;
		}

		Profiler::Instance().LoadDocument(document);
		std::shared_ptr<EditorProfilerService> self = weakSelf.lock();
		if (!self)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(self->mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (generation == self->projectGeneration && (!cancelToken || !cancelToken->load(std::memory_order_acquire)))
		{
			self->statusText = "Loaded capture";
			self->lastError.clear();
			self->busy = false;
		}
	});
	ThreadPool::instance().enqueue(task);
}

// EditorProfilerService::GetState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerSessionState EditorProfilerService::GetState() const
{
	return Profiler::Instance().GetState();
}

// EditorProfilerService::GetStatusText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string EditorProfilerService::GetStatusText() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return statusText;
}

// EditorProfilerService::GetLastError is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string EditorProfilerService::GetLastError() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return lastError;
}

// EditorProfilerService::GetCaptureList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> EditorProfilerService::GetCaptureList() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return captures;
}

// EditorProfilerService::HasActiveProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool EditorProfilerService::HasActiveProject() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return captureStore.HasProjectRoot();
}

// EditorProfilerService::IsBusy is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool EditorProfilerService::IsBusy() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return busy;
}

// EditorProfilerService::GetProfilerDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path EditorProfilerService::GetProfilerDirectory() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return captureStore.GetProfilerDirectory();
}

// EditorProfilerService::MakeMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::Profiling::ProfilerCaptureMetadata EditorProfilerService::MakeMetadata() const
{
	TerraEngine::Profiling::ProfilerCaptureMetadata metadata;
	metadata.terraEngineVersion = "TerraEngine";
	metadata.startedAt = TerraEngine::Profiling::MakeReadableTimestamp();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx && ctx->projectManager && ctx->projectManager->HasActiveProject())
	{
		const auto root = ctx->projectManager->GetProjectRoot();
		metadata.projectName = root.filename().string();
		metadata.projectPathHint = root.string();
	}
	return metadata;
}
