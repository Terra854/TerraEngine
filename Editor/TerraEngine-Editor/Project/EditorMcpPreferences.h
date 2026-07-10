// EditorMcpPreferences.h - Shared MCP preference defaults and validation.
//
// The editor preferences model, bridge lifecycle, and Preferences UI all use
// these limits so a saved port is interpreted consistently at every boundary.

#pragma once

#include <cstdint>

namespace TerraEngine::Editor::Preferences
{
	inline constexpr std::uint16_t kDefaultEditorMcpListenPort = 8765;
	inline constexpr std::uint16_t kMinimumEditorMcpListenPort = 1024;
	inline constexpr std::uint16_t kMaximumEditorMcpListenPort = 65535;

	inline bool IsValidEditorMcpListenPort(std::int64_t port)
	{
		// User-configured ports exclude the privileged/system range and port zero.
		// Port zero asks the OS for a random ephemeral port, which would break the
		// stable endpoint promised by this preference.
		return port >= kMinimumEditorMcpListenPort && port <= kMaximumEditorMcpListenPort;
	}
}
