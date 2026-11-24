#pragma once

#include <filesystem>

namespace TerraEngine::Tests
{
	// Test-build-only launcher path. Normal package discovery has already run;
	// this host owns the ordinary Engine lifecycle and bounded capture evidence.
	int RunPostProcessColorRuntimeFixture(const std::filesystem::path& evidenceDirectory);
}
