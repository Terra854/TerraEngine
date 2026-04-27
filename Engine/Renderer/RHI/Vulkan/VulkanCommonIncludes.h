// VulkanCommonIncludes.h — Common Vulkan Backend Includes
//
// Centralizes the necessary headers and configuration for the Vulkan backend implementation.
// It includes Volk for function loading, VMA for memory management, and other standard Vulkan and GLFW headers.
// This ensures consistent compiler settings and inclusion order across all Vulkan-related translation units.

#pragma once

// Shared Vulkan/VMA include points so every translation unit uses the same
// configuration flags. VMA implementation is only built where
// VMA_IMPLEMENTATION is defined before including this header.
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif

#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif

#include <volk.h>
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>
