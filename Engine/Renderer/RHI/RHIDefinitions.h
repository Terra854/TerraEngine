// RHIDefinitions.h — Rendering Hardware Interface Definitions
//
// Defines the cross-backend constants and enumerations used to abstract graphics hardware functionality.
// It provides a unified set of types for formats, usage flags, and resource states that are mapped to specific APIs like Vulkan.
// This ensures that the high-level renderer remains decoupled from the underlying hardware implementation.

#pragma once

#include <cstdint>

enum class RHIBackend
{
	None,
	Vulkan
};

// struct RHIConfig groups related state and operations so callers have one documented contract.
struct RHIConfig
{
	RHIBackend backend = RHIBackend::Vulkan;
#ifdef NDEBUG
	bool enableValidationLayers = false;
#else
	bool enableValidationLayers = true;
#endif
};
