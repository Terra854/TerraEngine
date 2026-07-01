#include "TestHarness.h"

#include "Engine/Renderer/RenderTypes.h"

#include <algorithm>
#include <array>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		bool LegendMentions(const char* legend, const std::string& token)
		{
			// Keep the test simple and readable: visual legends are user-facing
			// text, so a substring check is enough to prove each mode explains
			// its important color/state vocabulary.
			return legend != nullptr && std::string(legend).find(token) != std::string::npos;
		}

		bool HasMode(VirtualGeometryDebugVisualizationMode mode)
		{
			const auto& modes = GetVirtualGeometryDebugVisualizationModes();
			return std::find(modes.begin(), modes.end(), mode) != modes.end();
		}
	}

	void RegisterVirtualGeometryDebugViewTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.DebugView.ExposesExpectedVisualizerModes", [](TestContext& context) {
			context.ExpectEqual(static_cast<std::size_t>(9u), GetVirtualGeometryDebugVisualizationModes().size(), "The VG toolbar should expose only the nine public visualizer modes.");
			for (const VirtualGeometryDebugVisualizationMode mode : GetVirtualGeometryDebugVisualizationModes())
			{
				const std::string label = ToString(mode);
				context.Expect(!label.empty(), "Every virtual geometry visualizer mode should have a Game View label.");
				context.Expect(label != "Unknown", "Known virtual geometry visualizer modes should not report Unknown.");
				context.Expect(std::string(GetVirtualGeometryDebugVisualizationLegend(mode)) != "Unknown virtual geometry visualizer mode.", "Known virtual geometry visualizer modes should have a legend entry.");
			}

			context.ExpectEqual(std::string("Hi-Z"), std::string(ToString(VirtualGeometryDebugVisualizationMode::HiZ)), "The Game View visualizer inventory should include the Hi-Z mode.");
			context.ExpectEqual(std::string("Meshlet Id"), std::string(ToString(VirtualGeometryDebugVisualizationMode::MeshletId)), "The Game View visualizer inventory should include the meshlet identity mode.");
		});

		registry.Add("VirtualGeometry.Editor.DebugView.UnknownNumericModesNormalizeToOff", [](TestContext& context) {
			context.ExpectEqual(
				static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::None),
				static_cast<std::uint32_t>(NormalizeVirtualGeometryDebugVisualizationMode(static_cast<VirtualGeometryDebugVisualizationMode>(12u))),
				"Obsolete numeric visualizer modes should fail closed to Off.");
		});

		registry.Add("VirtualGeometry.Editor.DebugView.ModeChangesInvalidateVisibleCommandCache", [](TestContext& context) {
			const VirtualGeometryQualityProfile baseProfile = MakeDefaultVirtualGeometryQualityProfile();
			const std::uint64_t baseSignature = BuildVirtualGeometryQualityProfileSignature(baseProfile);

			for (const VirtualGeometryDebugVisualizationMode mode : GetVirtualGeometryDebugVisualizationModes())
			{
				VirtualGeometryQualityProfile profile = baseProfile;
				profile.debugVisualizationMode = mode;
				const std::uint64_t signature = BuildVirtualGeometryQualityProfileSignature(profile);
				if (mode == VirtualGeometryDebugVisualizationMode::None)
				{
					context.ExpectEqual(baseSignature, signature, "The Off visualizer mode should match the default quality profile signature.");
				}
				else
				{
					context.Expect(baseSignature != signature, "Changing a virtual geometry visualizer mode should invalidate cached visible draw packets immediately.");
				}
			}
		});

		registry.Add("VirtualGeometry.Editor.DebugView.ExposesAdaptiveClusterModes", [](TestContext& context) {
			const std::array<VirtualGeometryDebugVisualizationMode, 4u> adaptiveModes = {
				VirtualGeometryDebugVisualizationMode::LOD,
				VirtualGeometryDebugVisualizationMode::Cluster,
				VirtualGeometryDebugVisualizationMode::MeshletId,
				VirtualGeometryDebugVisualizationMode::Error
			};

			for (const VirtualGeometryDebugVisualizationMode mode : adaptiveModes)
			{
				context.Expect(HasMode(mode), "Adaptive detail, cluster identity, density, and error visualizers should be selectable in Scene View and Game View.");
			}
			context.Expect(LegendMentions(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::Cluster), "cluster"), "Cluster mode should explain its selected cluster identity colors.");
			context.Expect(LegendMentions(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::Error), "red"), "Error mode should document its red fail-open color.");
		});

		registry.Add("VirtualGeometry.Editor.DebugView.ExposesResidencyMode", [](TestContext& context) {
			context.Expect(HasMode(VirtualGeometryDebugVisualizationMode::Residency), "Residency mode should stay selectable for page-state debugging.");
			context.Expect(LegendMentions(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::Residency), "resident"), "Residency mode should explain resident page colors.");
			context.Expect(LegendMentions(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::Residency), "missing"), "Residency mode should explain missing page colors.");
		});

		registry.Add("VirtualGeometry.Editor.DebugView.ExposesHiZAndCullingModes", [](TestContext& context) {
			context.Expect(HasMode(VirtualGeometryDebugVisualizationMode::HiZ), "Hi-Z mode should be selectable for active/inactive history validation.");
			context.Expect(HasMode(VirtualGeometryDebugVisualizationMode::Culling), "Culling mode should be selectable for adaptive culling diagnostics.");
			context.ExpectEqual(std::string("Culling"), std::string(ToString(VirtualGeometryDebugVisualizationMode::Culling)), "The toolbar should expose the Culling label.");

			context.Expect(LegendMentions(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::HiZ), "Cyan"), "Hi-Z mode should document trusted history color.");
			context.Expect(LegendMentions(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::HiZ), "gray"), "Hi-Z mode should document inactive history color.");
			context.Expect(LegendMentions(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::Culling), "frustum"), "Culling mode should explain frustum rejection color.");
			context.Expect(LegendMentions(GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode::Culling), "cone culling is disabled"), "Culling mode should explain why cone rejection is unavailable for imported mesh safety.");
		});
	}
}
