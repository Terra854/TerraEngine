// RendererRecoveryPolicy.h - terminal renderer failure reporting policy.

#pragma once

#include "Core/Export.h"

#include <string>

namespace TerraEngine::Renderer
{
	struct RendererRecoveryPolicy
	{
		bool diagnosticsEnabled = true;
		bool validationEnabled = false;
		std::string fatalMessage = "Graphics device lost. Restart the application to restore rendering.";
	};

	TERRAENGINE_API RendererRecoveryPolicy MakeEditorRendererRecoveryPolicy();
	TERRAENGINE_API RendererRecoveryPolicy MakeExportRendererRecoveryPolicy(
		bool validationEnabled,
		bool diagnosticsEnabled,
		std::string fatalMessage);
}
