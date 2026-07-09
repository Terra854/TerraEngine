// RendererRecoveryPolicy.cpp - bounded renderer recovery policy helpers.

#include "Renderer/RendererRecoveryPolicy.h"

#include <algorithm>

namespace TerraEngine::Renderer
{
	RendererRecoveryPolicy MakeEditorRendererRecoveryPolicy()
	{
		RendererRecoveryPolicy policy;
		policy.diagnosticsEnabled = true;
		policy.validationEnabled = false;
		policy.maxRendererRebuildAttemptsPerIncident = 2u;
		policy.fatalOnExhaustedAttempts = false;
		policy.fatalMessage = "Renderer recovery failed. Save safe project data and inspect diagnostics.";
		return policy;
	}

	RendererRecoveryPolicy MakeExportRendererRecoveryPolicy(bool validationEnabled, bool diagnosticsEnabled, std::uint32_t maxAttempts, std::string fatalMessage)
	{
		RendererRecoveryPolicy policy;
		policy.validationEnabled = validationEnabled;
		policy.diagnosticsEnabled = diagnosticsEnabled;
		policy.maxRendererRebuildAttemptsPerIncident = std::max<std::uint32_t>(1u, maxAttempts);
		policy.fatalOnExhaustedAttempts = true;
		policy.fatalMessage = fatalMessage.empty()
			? "Graphics device lost and renderer recovery failed."
			: std::move(fatalMessage);
		return policy;
	}

	RendererRecoveryPolicyDecision EvaluateRendererRecoveryPolicy(const RendererRecoverySnapshot& snapshot, const RendererRecoveryPolicy& policy, std::uint32_t completedAttemptsForIncident)
	{
		RendererRecoveryPolicyDecision decision;
		if (snapshot.state == RendererRecoveryStateKind::Active || snapshot.state == RendererRecoveryStateKind::RebuildSucceeded)
		{
			decision.reason = "Renderer is active; no rebuild is required.";
			return decision;
		}
		if (snapshot.state == RendererRecoveryStateKind::RebuildInProgress)
		{
			decision.reason = "A renderer rebuild session is already running.";
			return decision;
		}

		decision.nextAttemptNumber = completedAttemptsForIncident + 1u;
		decision.budgetExhausted = completedAttemptsForIncident >= policy.maxRendererRebuildAttemptsPerIncident;
		decision.canAttempt = !decision.budgetExhausted;
		decision.reason = decision.canAttempt
			? "Renderer rebuild is allowed by the bounded recovery policy."
			: "Renderer rebuild budget is exhausted for this incident.";
		return decision;
	}
}
