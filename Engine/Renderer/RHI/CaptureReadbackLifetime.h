#pragma once

#include <cstdint>

namespace TerraEngine::Renderer
{
	// Released is proof only for a previously acknowledged asynchronous admission
	// on this same backend instance. Before admission it only indicates support.
	enum class CaptureReadbackOwnership : std::uint8_t { Unsupported, Retained, Released };
}
