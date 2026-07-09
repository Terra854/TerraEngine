// RendererRecoveryPolicy.h - bounded attempt rules for renderer recovery.

#pragma once

#include "Core/Export.h"
#include "Renderer/RendererRecovery.h"

#include <cstdint>
#include <string>

namespace TerraEngine::Renderer
{
	struct RendererRecoveryPolicy
	{
		bool diagnosticsEnabled = true;
		bool validationEnabled = false;
		std::uint32_t maxRendererRebuildAttemptsPerIncident = 1u;
		bool fatalOnExhaustedAttempts = false;
		std::string fatalMessage = "Graphics device lost and renderer recovery failed.";
	};

	struct RendererRecoveryPolicyDecision
	{
		bool canAttempt = false;
		bool budgetExhausted = false;
		std::uint32_t nextAttemptNumber = 0u;
		std::string reason;
	};

	TERRAENGINE_API RendererRecoveryPolicy MakeEditorRendererRecoveryPolicy();
	TERRAENGINE_API RendererRecoveryPolicy MakeExportRendererRecoveryPolicy(bool validationEnabled, bool diagnosticsEnabled, std::uint32_t maxAttempts, std::string fatalMessage);
	TERRAENGINE_API RendererRecoveryPolicyDecision EvaluateRendererRecoveryPolicy(const RendererRecoverySnapshot& snapshot, const RendererRecoveryPolicy& policy, std::uint32_t completedAttemptsForIncident);
}
