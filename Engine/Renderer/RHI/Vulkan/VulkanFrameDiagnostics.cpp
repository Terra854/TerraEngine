#include "VulkanFrameDiagnostics.h"
#include "VulkanCommonIncludes.h"
#include "Renderer/Graph/RenderDiagnostics.h"

#include <sstream>
#include <utility>

namespace TerraEngine::Renderer::Vulkan {

	std::string GetVulkanVirtualGeometryStageLabel(VirtualGeometryBackendStage stage)
	{
		switch(stage)
		{
		case VirtualGeometryBackendStage::Traversal: return "Virtual geometry traversal";
		case VirtualGeometryBackendStage::Compaction: return "Virtual geometry compaction";
		case VirtualGeometryBackendStage::IndirectGeneration: return "Virtual geometry indirect generation";
		case VirtualGeometryBackendStage::PageDecode: return "Virtual geometry page decode";
		case VirtualGeometryBackendStage::TriangleSetup: return "Virtual geometry triangle setup";
		case VirtualGeometryBackendStage::HardwareRaster: return "Virtual geometry hardware raster";
		case VirtualGeometryBackendStage::ComputeRaster: return "Virtual geometry compute raster";
		case VirtualGeometryBackendStage::VisibilityResolve: return "Virtual geometry visibility resolve";
		case VirtualGeometryBackendStage::Shading: return "Virtual geometry shading";
		case VirtualGeometryBackendStage::StreamingUpdate: return "Virtual geometry streaming update";
		case VirtualGeometryBackendStage::Deformation: return "Virtual geometry deformation";
		case VirtualGeometryBackendStage::Shadow: return "Virtual geometry shadow";
		case VirtualGeometryBackendStage::Capture: return "Virtual geometry capture";
		case VirtualGeometryBackendStage::RayTracing: return "Virtual geometry ray tracing";
		}

		return "Virtual geometry unknown stage";
	}

	VulkanVirtualGeometryStageMarker MakeVulkanVirtualGeometryStageMarker(
		VirtualGeometryBackendStage stage,
		VirtualGeometryBackendRoute route,
		bool ownsLiveCommand,
		bool timestampWritesRecorded)
	{
		VulkanVirtualGeometryStageMarker marker;
		marker.stage = stage;
		marker.route = route;
		marker.ownsLiveCommand = ownsLiveCommand;
		marker.timestampWritesRecorded = timestampWritesRecorded;
		marker.label = GetVulkanVirtualGeometryStageLabel(stage);

		if(!ownsLiveCommand)
		{
			marker.availability = BackendMeasurementAvailability::Unavailable;
			marker.reason = "No live Vulkan command owns this virtual-geometry stage; CPU-prepared data is not execution evidence.";
			return marker;
		}

		if(!timestampWritesRecorded)
		{
			marker.availability = BackendMeasurementAvailability::Invalid;
			marker.reason = "The live Vulkan command did not record both virtual-geometry timestamp boundaries.";
			return marker;
		}

		marker.availability = BackendMeasurementAvailability::Pending;
		marker.reason = "The live Vulkan command is submitted, and its timestamp query is pending delayed readback.";
		return marker;
	}

	std::array<VulkanVirtualGeometryStageMarker, kVulkanVirtualGeometryStageCount>
		CreateVulkanVirtualGeometryStageMarkers()
	{
		constexpr std::array<VirtualGeometryBackendStage, kVulkanVirtualGeometryStageCount> stages = {
			VirtualGeometryBackendStage::Traversal,
			VirtualGeometryBackendStage::Compaction,
			VirtualGeometryBackendStage::IndirectGeneration,
			VirtualGeometryBackendStage::PageDecode,
			VirtualGeometryBackendStage::TriangleSetup,
			VirtualGeometryBackendStage::HardwareRaster,
			VirtualGeometryBackendStage::ComputeRaster,
			VirtualGeometryBackendStage::VisibilityResolve,
			VirtualGeometryBackendStage::Shading,
			VirtualGeometryBackendStage::StreamingUpdate,
			VirtualGeometryBackendStage::Deformation,
			VirtualGeometryBackendStage::Shadow,
			VirtualGeometryBackendStage::Capture,
			VirtualGeometryBackendStage::RayTracing
		};

		std::array<VulkanVirtualGeometryStageMarker, kVulkanVirtualGeometryStageCount> markers{};
		for(std::size_t index = 0u; index < stages.size(); ++index)
		{
			// Stage routes are deliberately None until a command-recording path
			// proves ownership.  This makes future Vulkan executors opt in rather
			// than inheriting a false route claim from CPU preparation.
			markers[index] = MakeVulkanVirtualGeometryStageMarker(
				stages[index],
				VirtualGeometryBackendRoute::None,
				false,
				false);
		}
		return markers;
	}

	BackendFrameBreakdown CreateVulkanFrameBreakdown(std::uint64_t frameId)
	{
		BackendFrameBreakdown breakdown;
		breakdown.frameId = frameId;
		breakdown.requested = true;
		return breakdown;
	}

	void RecordVulkanFrameCost(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds)
	{
		AddBackendFrameCost(breakdown, scope.category, scope.viewType, milliseconds, scope.label, scope.shared);
	}

	void RecordVulkanFrameStage(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds)
	{
		AddBackendStageTiming(
			breakdown,
			scope.stage,
			scope.viewType,
			milliseconds,
			scope.viewId,
			scope.label,
			scope.viewScoped,
			"vulkan");
	}

	void RecordVulkanFrameStall(
		BackendFrameBreakdown& breakdown,
		const VulkanFrameDiagnosticScope& scope,
		float milliseconds,
		float thresholdMilliseconds,
		std::string recommendedAction)
	{
		RecordVulkanFrameStage(breakdown, scope, milliseconds);
		const BackendStageTiming& timing = breakdown.stageTimings.back();
		if (milliseconds >= thresholdMilliseconds || scope.stage == BackendSubmitStage::ForcedIdle)
		{
			AddBackendStallDiagnostic(
				breakdown,
				BuildBackendStallDiagnostic(timing, thresholdMilliseconds, std::move(recommendedAction)));
		}
	}

	std::optional<TerraEngine::Profiling::PresentationWaitKind> ClassifyVulkanPresentationWait(
		const VulkanFrameDiagnosticScope& scope)
	{
		// Keep Vulkan synchronization ownership unchanged. The graph-level helper
		// only translates the already measured stage into a profiler category.
		return ClassifyUnavoidablePresentationWait(scope.stage);
	}

	ViewSubmitWorkItem BuildVulkanViewSubmitWorkItem(
		std::uint64_t frameId,
		const VulkanFrameDiagnosticScope& scope,
		std::uint64_t passSetHash,
		std::uint64_t overlayStateHash,
		float recordingMilliseconds,
		float submitMilliseconds,
		std::string sharedWorkKey)
	{
		return BuildViewSubmitWorkItem(
			frameId,
			scope.viewId,
			scope.viewType,
			passSetHash,
			overlayStateHash,
			recordingMilliseconds,
			submitMilliseconds,
			std::move(sharedWorkKey));
	}

	std::string FormatVulkanVersion(std::uint32_t version)
	{
		std::ostringstream stream;
		stream
			<< VK_API_VERSION_MAJOR(version)
			<< "."
			<< VK_API_VERSION_MINOR(version)
			<< "."
			<< VK_API_VERSION_PATCH(version);
		return stream.str();
	}

	VulkanFirstFailureDiagnostics BuildVulkanFirstFailureDiagnostics(
		const RendererRecoverySnapshot& snapshot,
		std::string operation,
		std::string hardwareName,
		std::string driverVersion,
		std::string apiVersion)
	{
		VulkanFirstFailureDiagnostics diagnostics;
		diagnostics.stage = snapshot.firstFailureStage;
		diagnostics.affectedView = snapshot.affectedView;
		diagnostics.result = snapshot.firstFailureResult;
		diagnostics.operation = std::move(operation);
		diagnostics.affectedResourceId = snapshot.affectedResourceId;
		diagnostics.affectedResourceName = snapshot.affectedResourceName;
		diagnostics.hardwareName = std::move(hardwareName);
		diagnostics.driverVersion = std::move(driverVersion);
		diagnostics.apiVersion = std::move(apiVersion);
		diagnostics.frameId = snapshot.frameId;
		diagnostics.renderSessionGeneration = snapshot.renderSessionGeneration;
		diagnostics.duplicateFailureCount = snapshot.duplicateFailureCount;
		return diagnostics;
	}

	void ApplyVulkanFirstFailureDiagnostics(
		RendererRecoverySnapshot& snapshot,
		const VulkanFirstFailureDiagnostics& diagnostics)
	{
		// Preserve the first root-cause fields. Duplicate Vulkan failures are
		// already counted by RendererRecovery, so this function only enriches
		// the same incident with stable hardware/session identity.
		if (!diagnostics.hardwareName.empty() && snapshot.hardwareName.empty())
		{
			snapshot.hardwareName = diagnostics.hardwareName;
		}
		if (!diagnostics.driverVersion.empty() && snapshot.driverVersion.empty())
		{
			snapshot.driverVersion = diagnostics.driverVersion;
		}
		if (!diagnostics.apiVersion.empty() && snapshot.apiVersion.empty())
		{
			snapshot.apiVersion = diagnostics.apiVersion;
		}
	}

}
