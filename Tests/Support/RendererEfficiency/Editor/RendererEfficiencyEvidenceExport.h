#pragma once

#include "Renderer/RenderTypes.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <span>
#include <string>

namespace TerraEngine::Editor::Automation
{
	// Purpose distinguishes real control frame zero from later native residency
	// completion tokens. It never rewrites a supplied native step-frame identity.
	enum class RendererEfficiencyTransitionPurpose { Control, ResidencyBaseline, ResidencyReupload };
	// The protected host captures this identity before dispatching detached CPU
	// export work. It revalidates ownership again before accepting the result.
	struct RendererEfficiencyEvidenceExportContext
	{
		std::filesystem::path projectRoot, outputDirectory;
		std::string fixtureId, fixtureHash, operationId, fixtureStepId, stage = "P1";
		// Warmup/Measured are normal samples. Transition is a separate nested
		// receipt, retaining the original native correlation for its typed purpose.
		std::string runId, mode, measurementPhase = "Measured";
		RendererEfficiencyTransitionPurpose transitionPurpose = RendererEfficiencyTransitionPurpose::Control;
		// Captured from the host's frozen warmup+measured counts. Residency uses
		// this actual native baseline token; Control and normal phases keep zero.
		std::uint64_t residencyBaselineStepFrame = 0;
		std::uint64_t runIndex = 0, seed = 0, settingsGeneration = 0;
		std::uint64_t mutationGeneration = 0, rendererSessionGeneration = 0;
	};
	struct RendererEfficiencyEvidenceExportResult
	{
		nlohmann::json frames = nlohmann::json::array();
		nlohmann::json artifacts = nlohmann::json::array();
		// Internal CPU validation diagnostic, never a GPU or acceptance counter.
		std::uint64_t colorEncodingCount = 0;
	};
	// Job-safe, off the renderer hot path: only immutable CPU bytes are consumed.
	// No GPU wait, submission, native handle, scene mutation, or acceptance result
	// is manufactured. Rejection leaves output unchanged; orphaned complete files
	// from an interrupted export are harmless until a successful row references them.
	// LightingDepth preserves actual production/oracle query owners and unrounded
	// durations separately. An unavailable query never becomes a speed metric;
	// copied bounds/input/coverage are observations, not CPU-generated GPU proof.
	// Available shadow batching receipts must cover retained eligible owner spans
	// and partition actual submitted segments into recorded groups/singletons.
	// Missing legacy metadata stays unavailable; upload failure may still retain
	// complete actual singleton recording without a grouped instance allocation.
	bool ExportRendererEfficiencyEvidence(const RendererEfficiencyEvidenceExportContext& context,
		std::span<const Renderer::RendererEfficiencyEvidence> evidence,
		RendererEfficiencyEvidenceExportResult& output, std::string& error);
}
