#pragma once

#include <filesystem>
#include <string>

namespace TerraEngine::Editor::Automation
{
	// Copied install identity is prepared once on a worker, never during rendering.
	// Closure hashes identify actual sealed bytes even when the checkout was dirty.
	struct RendererEfficiencyInstallIdentity
	{
		std::filesystem::path installRoot;
		std::string revision, installedBinaryHash, installedShaderClosureHash;
		std::string installedAssetClosureHash, installedClosureHash;
	};
	// Production entry point derives its root from the running editor executable.
	// No requested identity or caller-supplied install root is trusted here.
	bool PrepareRendererEfficiencyInstallIdentity(RendererEfficiencyInstallIdentity& output, std::string& error);
	// Detached, non-loading IO verifier. The explicit-root overload supports small
	// fixture tests; it does not certify that the caller is the installed editor.
	// Both entry points leave output unchanged on any rejection.
	bool VerifyRendererEfficiencyInstallIdentity(const std::filesystem::path& installRoot,
		RendererEfficiencyInstallIdentity& output, std::string& error);
}
