#pragma once
#include "VirtualGeometrySelection.h"
#include "Renderer/RHI/RHIDefinitions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer
{
	enum class VirtualGeometryGeneratedWorkKind : std::uint8_t
	{
		MeshTasks,
		ComputeRaster,
		IndexedFallback
	};

	// The renderer selects only routes the active backend has actually exposed.
	// A prepared CPU packet stream is never relabelled as device-authored work.
	enum class VirtualGeometrySubmissionRoute : std::uint8_t
	{
		ProcessorOracle,
		VulkanPreparedFallback,
		VulkanDeviceAuthored
	};

	struct VirtualGeometryBackendSelection
	{
		VirtualGeometrySubmissionRoute route = VirtualGeometrySubmissionRoute::ProcessorOracle;
		VirtualGeometryGeneratedWorkKind generatedWorkKind = VirtualGeometryGeneratedWorkKind::IndexedFallback;
		bool deviceAuthoredWorkAvailable = false;
		bool delayedCounterReadbackAvailable = false;
		std::uint32_t commandCapacity = 0u;
		std::string fallbackReason;
	};

	[[nodiscard]] VirtualGeometryBackendSelection SelectVirtualGeometryBackend(
		const BackendCapabilityProfile &capabilities);

	struct VirtualGeometryTraversalPlan
	{
		std::uint32_t instanceCount = 0u;
		std::uint32_t requestCapacity = 0u;
		std::uint64_t expectedGeneration = 0u;
		bool failOpenOnInvalid = true;
	};

	struct VirtualGeometryCompactionPlan
	{
		std::uint32_t candidatePacketCount = 0u;
		std::uint32_t outputCapacity = 0u;
		bool failOpenOnInvalid = true;
	};

	struct VirtualGeometryGeneratedWorkPlan
	{
		VirtualGeometryGeneratedWorkKind kind = VirtualGeometryGeneratedWorkKind::MeshTasks;
		std::uint32_t commandCount = 0u;
		std::uint32_t commandCapacity = 0u;
		bool usesIndirectCount = false;
		bool zeroWork = true;
	};

	struct VirtualGeometryReplayPlan
	{
		std::uint64_t deterministicHash = 0u;
		std::uint64_t sourceFrameId = 0u;
		std::uint64_t commandGeneration = 0u;
		std::uint32_t expectedAuthoritativeCounter = 0u;
	};

	struct VirtualGeometryGpuPlanOptions
	{
		std::uint32_t packetCapacity = 0u;
		std::uint64_t frameId = 0u;
		std::uint64_t generation = 0u;
		VirtualGeometryGeneratedWorkKind generatedWorkKind = VirtualGeometryGeneratedWorkKind::MeshTasks;
		bool usesIndirectCount = false;
	};

	struct VirtualGeometryGpuPlan
	{
		std::vector<std::uint64_t> packets;
		std::uint32_t capacity = 0;
		bool overflow = false;
		std::uint64_t deterministicHash = 0;
		// These nested records are immutable worker output. The protected Vulkan
		// boundary translates them into API commands without reading scene data or
		// rebuilding a per-cluster CPU submission list.
		std::uint64_t frameId = 0u;
		std::uint64_t generation = 0u;
		bool failOpen = false;
		bool valid = false;
		VirtualGeometryTraversalPlan traversal;
		VirtualGeometryCompactionPlan compaction;
		VirtualGeometryGeneratedWorkPlan generatedWork;
		VirtualGeometryReplayPlan replay;
		std::string rejectionReason;

		[[nodiscard]] bool IsComplete() const;
	};

	[[nodiscard]] VirtualGeometryGpuPlan BuildVirtualGeometryGpuPlan(
		const VirtualGeometrySelectionResult &selection,
		const VirtualGeometryGpuPlanOptions &options);
	[[nodiscard]] VirtualGeometryGpuPlan BuildVirtualGeometryGpuPlan(const VirtualGeometrySelectionResult &, std::uint32_t capacity);
} // namespace TerraEngine::Renderer
