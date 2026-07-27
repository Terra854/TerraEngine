// AnimationPackValidator.h - fail-closed validation for packaged animation data.

#pragma once

#include "AnimationExportDependencies.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace TerraEngine::Export
{
	struct AnimationPackEvidence final
	{
		std::string rule;
		std::string path;
		bool passed = false;
		std::string message;
		std::string expected;
		std::string actual;
	};

	struct AnimationPackValidationReport final
	{
		bool accepted = false;
		std::filesystem::path archivePath;
		std::uint32_t checkedArchiveEntries = 0U;
		std::uint32_t checkedAnimationAssets = 0U;
		std::uint32_t checkedDependencyEdges = 0U;
		std::vector<AnimationPackEvidence> evidence;

		[[nodiscard]] std::string SerializeJson() const;
	};

	class AnimationPackValidator final
	{
	public:
		// Opens the final TEPAK and validates its real metadata and reconstructed
		// bytes. This is the publication gate used by ExportPipeline.
		[[nodiscard]] static AnimationPackValidationReport ValidateArchive(
			const std::filesystem::path& archivePath);

		// Lightweight staging preflight retained for focused closure diagnostics.
		[[nodiscard]] static AnimationPackValidationReport Validate(
			const AnimationExportClosure& closure,
			std::uint32_t formatMajor,
			bool cpuFallbackAvailable);

		[[nodiscard]] static bool WriteEvidence(
			const AnimationPackValidationReport& report,
			const std::filesystem::path& evidencePath,
			std::string& outError);
	};
}
