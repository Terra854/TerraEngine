#pragma once

#include <cstdint>

enum class RHIBackend
{
	None,
	Vulkan
};

struct RHIConfig
{
	RHIBackend backend = RHIBackend::Vulkan;
#ifdef NDEBUG
	bool enableValidationLayers = false;
#else
	bool enableValidationLayers = true;
#endif
};
