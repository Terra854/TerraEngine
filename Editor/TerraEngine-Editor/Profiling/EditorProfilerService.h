// EditorProfilerService.h — Editor profiling service interface
//
// Defines the high-level service for managing engine profiling sessions within the editor. it handles the coordination of capture recordings, session persistence via the ProfilerCaptureStore, and asynchronous loading of profile artifacts.

#pragma once

#include "ProfilerCaptureStore.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct EditorContext;

// class EditorProfilerService groups related state and operations so callers have one documented contract.
class EditorProfilerService
{
public:
	void Initialize(EditorContext& context);
	void Shutdown();
	void OnProjectOpened();
	void OnProjectClosed();
	void Update(float deltaTime);

	void StartRecording();
	void PauseRecording();
	void ResumeRecording();
	void FinishCapture();
	void Clear();
	void RefreshCaptureList();
	void LoadCapture(const std::filesystem::path& capturePath);

	TerraEngine::Profiling::ProfilerSessionState GetState() const;
	std::string GetStatusText() const;
	std::string GetLastError() const;
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> GetCaptureList() const;
	bool HasActiveProject() const;
	bool IsBusy() const;
	std::filesystem::path GetProfilerDirectory() const;

private:
	TerraEngine::Profiling::ProfilerCaptureMetadata MakeMetadata() const;
	void SetBusyStatus(bool busy, std::string status);

	EditorContext* ctx = nullptr;
	ProfilerCaptureStore captureStore;
	mutable std::mutex mutex;
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures;
	bool busy = false;
	std::string statusText = "Idle";
	std::string lastError;
	std::uint64_t projectGeneration = 0u;
};
