// RenderWorkPacket.h - extended render packet planning helpers.
//
// The base packet type lives in RenderGraph.h because graph validation needs
// it. This header adds backend-recording batch metadata without exposing any
// API-specific command-buffer handles to renderer-facing code.

#pragma once

#include "RenderGraph.h"

namespace TerraEngine::Renderer {

	enum class RenderBackendWorkKind
	{
		Geometry,
		Shadow,
		UI,
		Culling,
		Descriptor,
		Upload,
		Transition,
		Validation,
		Stats
	};

	struct BackendRecordingBatch
	{
		std::string batchId;
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		std::string passId;
		RenderBackendWorkKind workKind = RenderBackendWorkKind::Geometry;
		std::size_t begin = 0u;
		std::size_t end = 0u;
		bool workerEligible = false;
		std::string fallbackReason;
	};

	struct BackendRecordingBatchPlan
	{
		std::vector<BackendRecordingBatch> batches;
		std::size_t workerBatchCount = 0u;
		std::size_t serialBatchCount = 0u;
		std::vector<std::string> serializedReasons;
	};

	TERRAENGINE_API const char* ToString(RenderBackendWorkKind workKind);
}
