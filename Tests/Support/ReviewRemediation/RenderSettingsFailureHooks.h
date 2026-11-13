// Validation-only one-shot failure at the actual settings filesystem boundary.
// Install on the owner thread before the operation; never share mutable hooks
// with worker jobs. An unset hook preserves the existing production behavior.
#pragma once
#include "Core/Export.h"
#include <cstdint>

namespace TerraEngine::Tests::Review
{
	enum class SettingsFilesystemOperation : std::uint8_t { Exists, CreateDirectories };
	struct RenderSettingsFailureHook
	{
		SettingsFilesystemOperation operation = SettingsFilesystemOperation::Exists;
		std::uint32_t remaining = 1, reached = 0;
	};
	// Returns false when this build has no validation hooks. This is a missing
	// prerequisite, never evidence that a filesystem failure was handled.
	TERRAENGINE_API bool SetRenderSettingsFailureHook(RenderSettingsFailureHook* hook) noexcept;
}
