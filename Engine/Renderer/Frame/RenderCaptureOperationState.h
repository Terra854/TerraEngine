// Private capture admission record shared by lifecycle retirement and the capture facade.
#pragma once
#include "Renderer/RenderSystem.h"
#include "Renderer/SyntheticCapture.h"
#include <chrono>

struct RenderSystem::CaptureOperation
{
	TerraEngine::Renderer::RenderCaptureRequest original;
	std::string admissionId;
	TerraEngine::Renderer::SyntheticCaptureJob synthetic;
	bool syntheticStarted = false;
	bool activeCharged = false;
	bool clientTerminal = false;
	bool backendPending = false;
	bool logicalRevoked = false;
	RHIContext* backendOwner = nullptr;
	bool physicalSettled = false;
	std::string publicResultId;
	TerraEngine::Renderer::RenderCaptureResult delivery;
	TerraEngine::Renderer::RenderCaptureResult timeoutResult;
	std::chrono::steady_clock::time_point deadline;
};
