// RendererRecoveryPolicy.cpp - terminal renderer failure reporting policy.

#include "Renderer/RendererRecoveryPolicy.h"

#include <utility>

namespace TerraEngine::Renderer
{
	RendererRecoveryPolicy MakeEditorRendererRecoveryPolicy()
	{
		RendererRecoveryPolicy policy;
		policy.diagnosticsEnabled = true;
		policy.validationEnabled = false;
		policy.fatalMessage = "Graphics device lost. The editor will now close.";
		return policy;
	}

	RendererRecoveryPolicy MakeExportRendererRecoveryPolicy(
		bool validationEnabled,
		bool diagnosticsEnabled,
		std::string fatalMessage)
	{
		RendererRecoveryPolicy policy;
		policy.validationEnabled = validationEnabled;
		policy.diagnosticsEnabled = diagnosticsEnabled;
		policy.fatalMessage = fatalMessage.empty()
			? "Graphics device lost. Restart the application to restore rendering."
			: std::move(fatalMessage);
		return policy;
	}
}
