// RendererScratchDiagnostics.cpp - Preserve episode facts across reclamation.
#include "RendererScratchDiagnostics.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include <nlohmann/json.hpp>

namespace TerraEngine::Editor::Automation {
	namespace {
		const char* OutcomeName(Renderer::RenderFrameScratchOutcome outcome) noexcept
		{
			switch(outcome) {
			case Renderer::RenderFrameScratchOutcome::None: return "None";
			case Renderer::RenderFrameScratchOutcome::Active: return "Active";
			case Renderer::RenderFrameScratchOutcome::Completed: return "Completed";
			case Renderer::RenderFrameScratchOutcome::Failed: return "Failed";
			case Renderer::RenderFrameScratchOutcome::Cancelled: return "Cancelled";
			}
			return "Unknown";
		}
		nlohmann::json Episode(const Renderer::RenderFrameScratchDiagnostics& value)
		{
			// No inferred zero, recomputed peak or current arena query replaces the
			// terminal copy. Failed/cancelled episodes retain their actual pressure.
			return {{"lifetimeId", value.lifetimeId}, {"episodeId", value.episodeId}, {"frameId", value.frameId},
				{"capacityBytes", value.capacityBytes}, {"usedBytes", value.usedBytes}, {"peakBytes", value.peakBytes},
				{"lifetimeHighWaterBytes", value.lifetimeHighWaterBytes}, {"reuseAllocationCount", value.reuseAllocationCount},
				{"fallbackAllocationCount", value.fallbackAllocationCount}, {"resetCount", value.resetCount},
				{"outcome", OutcomeName(value.outcome)}, {"outcomeCode", static_cast<unsigned>(value.outcome)}};
		}
	}
	nlohmann::json SerializeRendererScratchDiagnostics(const Renderer::RenderDiagnosticsSummary& summary)
	{
		const auto& reclamation = summary.frameScratchReclamation;
		return {{"latest", Episode(summary.frameScratch)}, {"previous", Episode(summary.previousFrameScratch)},
			{"reclamation", {{"lifetimeId", reclamation.lifetimeId},
				{"totalFallbackAllocationCount", reclamation.totalFallbackAllocationCount},
				{"totalFallbackDeallocationCount", reclamation.totalFallbackDeallocationCount},
				{"outstandingFallbackAllocationCount", reclamation.outstandingFallbackAllocationCount},
				{"outstandingFallbackBytes", reclamation.outstandingFallbackBytes}, {"resetCount", reclamation.resetCount}}}};
	}
}
