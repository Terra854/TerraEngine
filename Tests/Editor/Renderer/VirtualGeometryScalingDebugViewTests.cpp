#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderSystem.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests {

	namespace
	{
		std::string ReadSourceForDebugViewContract(const char* path)
		{
			// Source-layout tests protect editor-facing diagnostics from losing
			// labels while the runtime packet path keeps evolving underneath.
			const auto read = [](const std::string& candidate) {
				std::ifstream file(candidate, std::ios::binary);
				if (!file)
				{
					return std::string();
				}
				std::ostringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			};

			std::string text = read(path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../") + path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../../") + path);
			if (!text.empty())
			{
				return text;
			}

			const std::string thisFile = __FILE__;
			const auto resolveFromMarker = [&](const std::string& marker) {
				const std::size_t markerPosition = thisFile.find(marker);
				if (markerPosition == std::string::npos)
				{
					return std::string();
				}
				return read(thisFile.substr(0u, markerPosition) + path);
			};
			text = resolveFromMarker("Tests\\Editor\\Renderer\\VirtualGeometryScalingDebugViewTests.cpp");
			if (!text.empty())
			{
				return text;
			}
			return resolveFromMarker("Tests/Editor/Renderer/VirtualGeometryScalingDebugViewTests.cpp");
		}
	}

	void RegisterVirtualGeometryScalingDebugViewTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Diagnostics.Foundation.ShadowSelectionsRemainSeparate", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			ShadowVisibilitySelection shadow;
			shadow.shadowViewId = 3u;
			shadow.candidatePackets = 20u;
			shadow.selectedPackets = 12u;
			shadow.reusedCameraHiZ = false;
			diagnostics.shadowVisibilitySelections.push_back(shadow);
			diagnostics.shadowViewCount = static_cast<std::uint32_t>(diagnostics.shadowVisibilitySelections.size());

			context.ExpectEqual(1u, diagnostics.shadowViewCount, "Debug view diagnostics should expose shadow-view VG selections separately.");
			context.Expect(!diagnostics.shadowVisibilitySelections.front().reusedCameraHiZ, "Shadow VG diagnostics should not report camera Hi-Z reuse.");
		});

		registry.Add("VirtualGeometry.Editor.Diagnostics.US2.VisualizerIdentitySurvivesGpuPacketSnapshots", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility;
			visibility.valid = true;
			visibility.frameId = 7300u;
			visibility.outputId = 2u;
			visibility.viewType = RenderViewType::Scene;
			visibility.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::Cluster;
			visibility.hiZValidationState.state = VirtualGeometryHiZState::Active;
			visibility.hiZValidationState.pyramidReady = true;
			visibility.hiZValidationState.failOpen = false;

			VirtualGeometryClusterPacket cpuPacket;
			cpuPacket.packetId = 0u;
			cpuPacket.viewId = visibility.outputId;
			cpuPacket.viewType = visibility.viewType;
			cpuPacket.objectId = TerraEngine::GenerateUUID();
			cpuPacket.meshId = TerraEngine::GenerateUUID();
			cpuPacket.materialId = TerraEngine::GenerateUUID();
			cpuPacket.selectedLod = 1u;
			cpuPacket.clusterId = 44u;
			cpuPacket.meshletOffset = 88u;
			cpuPacket.meshletCount = 5u;
			cpuPacket.debugIdentity = 123u;
			visibility.clusterPackets.push_back(cpuPacket);
			visibility.gpuPacketRecords = RenderSystem::BuildGpuVirtualGeometryPacketRecords(visibility.clusterPackets, visibility.frameId, visibility.debugVisualizationMode);
			visibility.gpuBatchDescriptors = RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(visibility.gpuPacketRecords, visibility.frameId, visibility.debugVisualizationMode);
			visibility.gpuPacketFrameSets.push_back(RenderSystem::BuildGpuVirtualGeometryPacketFrameSet(visibility.viewType, visibility.outputId, visibility.frameId, glm::uvec2(1280u, 720u), visibility.debugVisualizationMode, 0u, 1u, 0u, 1u));

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::Cluster), static_cast<std::uint32_t>(diagnostics.debugVisualizationMode), "Debug diagnostics should preserve the selected visualizer mode.");
			context.ExpectEqual(1u, diagnostics.gpuPacketRecordCount, "Debug diagnostics should expose the GPU packet record count used by Cluster mode.");
			context.ExpectEqual(1u, diagnostics.gpuValidFrameSetCount, "Debug diagnostics should label a valid GPU packet frame set.");
			context.Expect(diagnostics.hiZActive, "Hi-Z visualizer diagnostics should still report active Hi-Z state through the GPU packet path.");
			context.Expect(std::string(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::Cluster)).find("cluster") != std::string::npos, "Cluster visualizer legend should remain the editor-facing identity for packet debug colors.");
		});

		registry.Add("VirtualGeometry.Editor.Diagnostics.US6.OverlayLabelsGpuPacketSources", [](TestContext& context) {
			const std::string readback = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"VG GPU Packet Records",
				9u,
				VirtualGeometryCounterSource::GPUReadback,
				"");
			const std::string delayed = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"VG GPU Packet Records",
				3u,
				VirtualGeometryCounterSource::DelayedGPUReadback,
				"waiting for readback");
			const std::string unavailable = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"VG GPU Packet Records",
				0u,
				VirtualGeometryCounterSource::Unavailable,
				"no packet table");

			context.Expect(readback.find("[GPU Readback]") != std::string::npos, "GPU packet overlay rows should label authoritative readback counters.");
			context.Expect(delayed.find("[Delayed GPU Readback]") != std::string::npos, "GPU packet overlay rows should label delayed readback counters.");
			context.Expect(delayed.find("waiting for readback") != std::string::npos, "Delayed GPU packet overlay rows should keep the delay reason.");
			context.Expect(unavailable.find("unavailable") != std::string::npos, "Unavailable GPU packet overlay rows should not look like real zeros.");
			context.Expect(unavailable.find("no packet table") != std::string::npos, "Unavailable GPU packet overlay rows should keep their reason.");
		});

		registry.Add("VirtualGeometry.Editor.Diagnostics.US6.ProfilingAndOverlaysExposeGpuPacketCounters", [](TestContext& context) {
			const std::string renderSystem = ReadSourceForDebugViewContract("Engine/Renderer/RenderSystem.cpp");
			const std::string profiler = ReadSourceForDebugViewContract("Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp");
			const std::string gameView = ReadSourceForDebugViewContract("Editor/TerraEngine-Editor/Windows/GameViewWindow.cpp");
			const std::string sceneView = ReadSourceForDebugViewContract("Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp");

			context.Expect(renderSystem.find("RenderSystem: VG Packet Upload Planning") != std::string::npos, "Profiler scopes should name VG packet upload planning.");
			context.Expect(renderSystem.find("RenderSystem: VG Compaction Dispatch Planning") != std::string::npos, "Profiler scopes should name VG compaction dispatch planning.");
			context.Expect(renderSystem.find("RenderSystem: VG Generated Submission Validation") != std::string::npos, "Profiler scopes should name generated submission validation.");
			context.Expect(renderSystem.find("RenderSystem: VG Fallback Submission") != std::string::npos, "Profiler scopes should name fail-open fallback submission work.");
			context.Expect(profiler.find("ProfilerVirtualGeometryPacketRows") != std::string::npos, "Profiler details should include a VG packet diagnostic table.");
			context.Expect(profiler.find("Virtual Geometry Packet Diagnostics") != std::string::npos, "Profiler details should label the VG packet diagnostic tree.");
			context.Expect(gameView.find("VG GPU Packet Records") != std::string::npos, "Game View overlay should expose GPU packet records.");
			context.Expect(gameView.find("VG Generated Submissions") != std::string::npos, "Game View overlay should expose generated submission counters.");
			context.Expect(sceneView.find("VG GPU Packet Records") != std::string::npos, "Scene View overlay should expose GPU packet records.");
			context.Expect(sceneView.find("VG Generated Submissions") != std::string::npos, "Scene View overlay should expose generated submission counters.");
		});
	}
}
