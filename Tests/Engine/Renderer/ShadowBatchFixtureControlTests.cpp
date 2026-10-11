#include "TestHarness.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureShadowBatchControls.h"
#include <limits>

namespace TerraEngine::Tests
{
	void RegisterShadowBatchFixtureControlTests(TestRegistry& registry)
	{
		using namespace Renderer;
		registry.Add("Renderer.Efficiency.ShadowBatchFixtureControls.AbsentAndDeviceDefaults", [](TestContext& test) {
			std::string reason = "old diagnostic";
			test.Expect(ValidateRendererEfficiencyFixtureShadowBatchControls({}, false, reason)
				&& reason.empty(), "Ordinary rendering needs no fixture owner and clears stale diagnostics.");
			std::optional<RendererEfficiencyFixtureShadowBatchControls> controls{std::in_place};
			test.Expect(ValidateRendererEfficiencyFixtureShadowBatchControls(controls, true, reason),
				"Zero limits preserve actual device limits for an owned fixture.");
		});
		registry.Add("Renderer.Efficiency.ShadowBatchFixtureControls.OwnershipRequired", [](TestContext& test) {
			std::optional<RendererEfficiencyFixtureShadowBatchControls> controls{std::in_place};
			std::string reason;
			for (const bool inject : {false, true}) {
				controls->failInstanceUpload = inject;
				const auto before = controls;
				test.Expect(!ValidateRendererEfficiencyFixtureShadowBatchControls(controls, false, reason)
					&& !reason.empty(), "Even default or fault-only controls require an owned validation session.");
				test.Expect(controls == before, "Rejected admission never edits or consumes the control.");
			}
		});
		registry.Add("Renderer.Efficiency.ShadowBatchFixtureControls.ExactLimitsAndFaultPreserved", [](TestContext& test) {
			std::optional<RendererEfficiencyFixtureShadowBatchControls> controls{std::in_place};
			controls->maxInstancesPerGroup = 65536;
			controls->maxTaskGroupsX = 1048576;
			controls->failInstanceUpload = true;
			const auto before = controls;
			std::string reason;
			test.Expect(ValidateRendererEfficiencyFixtureShadowBatchControls(controls, true, reason)
				&& reason.empty() && controls == before, "Inclusive bounded limits and one-shot intent are retained exactly.");
			// Test each lane independently, including its largest representable value.
			for (const auto overflow : {65537U, std::numeric_limits<std::uint32_t>::max()}) {
				controls = before; controls->maxInstancesPerGroup = overflow;
				const auto rejected = controls;
				test.Expect(!ValidateRendererEfficiencyFixtureShadowBatchControls(controls, true, reason)
					&& !reason.empty() && controls == rejected, "Instance limit overflow rejects with a diagnostic and unchanged input.");
			}
			for (const auto overflow : {1048577U, std::numeric_limits<std::uint32_t>::max()}) {
				controls = before; controls->maxTaskGroupsX = overflow;
				const auto rejected = controls;
				test.Expect(!ValidateRendererEfficiencyFixtureShadowBatchControls(controls, true, reason)
					&& !reason.empty() && controls == rejected, "Task limit overflow rejects with a diagnostic and unchanged input.");
			}
		});
	}
}
