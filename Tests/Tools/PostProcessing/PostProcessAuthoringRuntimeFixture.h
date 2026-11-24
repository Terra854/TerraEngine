#pragma once
#include <filesystem>

namespace TerraEngine::Tests
{
	// BUILD_TESTING launcher branch after normal exported-package discovery.
	// Owns the real Engine lifecycle and native camera captures, never acceptance
	// synthesized from managed settings or another process's image evidence.
	int RunPostProcessAuthoringRuntimeFixture(const std::filesystem::path& evidenceDirectory);
}
