// Native renderer fixtures share one friend definition across translation units.
#pragma once

#include "Renderer/RenderSystem.h"
#include <memory>
#include <utility>

namespace TerraEngine::Renderer {
	// Native-only fixture access owns complete fake contexts at a quiescent
	// boundary. Replacement uses the production proof gate, never a raw swap.
	struct RenderCaptureBackendTestAccess
	{
		static bool Attach(RenderSystem& renderer, std::unique_ptr<RHIContext> backend)
		{
			renderer.RetireCaptureJobs();
			if (!renderer.TryReleaseCaptureBackend()) return false;
			renderer.rhi = std::move(backend);
			renderer.initialized = true;
			renderer.hasLatestGameViewCamera = true;
			return true;
		}
		// Quiescent fixture teardown revokes capture delivery and settles CPU jobs
		// before an owned executor stops. Pending native ledger/owners remain
		// retained; this does not shut down the backend.
		static void RetireJobs(RenderSystem& renderer) { renderer.RetireCaptureJobs(); }
		static void PublishCamera(RenderSystem& renderer) { renderer.hasLatestGameViewCamera = true; }
	};
}

