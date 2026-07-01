#include "TestHarness.h"

#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/RenderSystem.h"

#include <algorithm>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryHardeningDebugViewTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Hardening.DebugView.PublicModesAreUnique", [](TestContext& context) {
			std::vector<std::uint32_t> rawModes;
			for (const VirtualGeometryDebugVisualizationMode mode : GetVirtualGeometryDebugVisualizationModes())
			{
				rawModes.push_back(static_cast<std::uint32_t>(mode));
				context.Expect(std::string(ToString(mode)) != "Unknown", "Public virtual geometry visualizer modes should have stable labels.");
				context.Expect(std::string(GetVirtualGeometryDebugVisualizationLegend(mode)) != "Unknown virtual geometry visualizer mode.", "Public virtual geometry visualizer modes should have legend text.");
			}

			std::vector<std::uint32_t> sortedModes = rawModes;
			std::sort(sortedModes.begin(), sortedModes.end());
			const auto duplicate = std::adjacent_find(sortedModes.begin(), sortedModes.end());
			context.Expect(duplicate == sortedModes.end(), "The visualizer menu should not expose duplicate virtual geometry modes.");
			context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::None), sortedModes.front(), "The public mode list should include Off as the safe default.");
		});

		registry.Add("VirtualGeometry.Editor.Hardening.DebugView.InternalNumbersNormalizeOff", [](TestContext& context) {
			const std::uint32_t firstInternalCandidate = 32u;
			const VirtualGeometryDebugVisualizationMode normalized =
				NormalizeVirtualGeometryDebugVisualizationMode(static_cast<VirtualGeometryDebugVisualizationMode>(firstInternalCandidate));

			context.ExpectEqual(
				static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::None),
				static_cast<std::uint32_t>(normalized),
				"Unknown or internal debug enum values should never appear as selectable modes.");
		});

		registry.Add("VirtualGeometry.Editor.Hardening.DebugView.HiZOverlayExplainsFailOpen", [](TestContext& context) {
			const std::string failOpenText = TerraEngine::Renderer::FormatVirtualGeometryHiZOverlayStatus(
				VirtualGeometryHiZState::FailOpen,
				false,
				"projected-bounds-risk");
			const std::string missingText = TerraEngine::Renderer::FormatVirtualGeometryHiZOverlayStatus(
				VirtualGeometryHiZState::MissingPyramid,
				false,
				"missing-pyramid");
			const std::string activeText = TerraEngine::Renderer::FormatVirtualGeometryHiZOverlayStatus(
				VirtualGeometryHiZState::Active,
				true,
				"");

			context.Expect(failOpenText.find("Fail Open") != std::string::npos, "Game View overlay text should name Hi-Z fail-open state.");
			context.Expect(failOpenText.find("projected-bounds-risk") != std::string::npos, "Game View overlay text should include the fail-open reason.");
			context.Expect(missingText.find("Missing Pyramid") != std::string::npos, "Missing pyramid status should have a user-facing label.");
			context.ExpectEqual(std::string("Active"), activeText, "Active Hi-Z status should remain compact in the stats overlay.");
			context.Expect(std::string(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::HiZ)).find("fail-open") != std::string::npos, "The Hi-Z visualizer legend should explain the fail-open color.");
		});

		registry.Add("VirtualGeometry.Editor.Hardening.DebugView.CounterOverlayLabelsSource", [](TestContext& context) {
			const std::string prepared = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"Meshlets Selected",
				117u,
				VirtualGeometryCounterSource::Prepared,
				"prepared CPU estimate");
			const std::string delayed = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"Meshlets Culled",
				9u,
				VirtualGeometryCounterSource::DelayedGPUReadback,
				"GPU readback is delayed");
			const std::string unavailable = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"Meshlets Dispatched",
				0u,
				VirtualGeometryCounterSource::Unavailable,
				"no completed view stats");

			context.Expect(prepared.find("[Prepared]") != std::string::npos, "Prepared counters should be labeled in the Game View overlay.");
			context.Expect(delayed.find("[Delayed GPU Readback]") != std::string::npos, "Delayed counters should keep their source label.");
			context.Expect(delayed.find("GPU readback is delayed") != std::string::npos, "Delayed counters should include the delay reason.");
			context.Expect(unavailable.find("unavailable") != std::string::npos, "Unavailable counters should not print as authoritative zero.");
			context.Expect(unavailable.find("no completed view stats") != std::string::npos, "Unavailable counters should explain why the row has no value.");
		});

		registry.Add("VirtualGeometry.Editor.Hardening.DebugView.SceneAndGameModesAreIndependent", [](TestContext& context) {
			RenderSystem& renderer = RenderSystem::Instance();
			const VirtualGeometryQualityProfile projectDefaults = renderer.GetVirtualGeometryQualityProfile();
			renderer.ClearVirtualGeometryDebugVisualizationModes();

			renderer.SetVirtualGeometryDebugVisualizationMode(RenderViewType::Scene, 1u, VirtualGeometryDebugVisualizationMode::Cluster);
			renderer.SetVirtualGeometryDebugVisualizationMode(RenderViewType::Game, 2u, VirtualGeometryDebugVisualizationMode::HiZ);

			context.Expect(renderer.GetVirtualGeometryDebugVisualizationMode(RenderViewType::Scene, 1u) == VirtualGeometryDebugVisualizationMode::Cluster, "Scene View should keep the mode selected from its own toolbar.");
			context.Expect(renderer.GetVirtualGeometryDebugVisualizationMode(RenderViewType::Game, 2u) == VirtualGeometryDebugVisualizationMode::HiZ, "Game View should keep the mode selected from its own toolbar.");
			context.Expect(renderer.GetVirtualGeometryQualityProfile().debugVisualizationMode == projectDefaults.debugVisualizationMode, "View-local visualizer mode changes should not mutate project render settings.");

			renderer.ClearVirtualGeometryDebugVisualizationModes();
		});
	}
}
