#pragma once
#include <filesystem>

namespace TerraEngine::Tests
{
	// Validation-only packaged launcher. Runs the actual managed controller and
	// saves exact native observations/captures at its first waiting checkpoint.
	// Returns2 for truthful pending quality/motion/reset proof,1 for failure.
	// This host never writes a temporal-aa-ack file or turns diagnostics into AA
	// acceptance. The caller owns package discovery before entering this branch.
	int RunPostProcessTemporalRuntimeFixture(const std::filesystem::path& evidenceDirectory);
}
