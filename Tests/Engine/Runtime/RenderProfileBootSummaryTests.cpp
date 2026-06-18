#include "TestHarness.h"

#include "Runtime/BootSummary.h"

namespace TerraEngine::Tests {
	void RegisterRenderProfileBootSummaryTests(TestRegistry& registry) {
		registry.Add("Engine.BootSummary.IncludesRenderProfileFlags", [](TestContext& context) {
			TerraEngine::Runtime::BootSummary summary;
			summary.projectName = "Baseline";
			summary.startupScene = "Scenes/Startup.tescene";
			summary.renderProfile = "profile=Baseline shadows=high ao=medium vrs=disabled textureBudgetMb=512 resolutionScale=1 validation=on debug=off";
			summary.validationEnabled = true;
			summary.runtimeEntryAllowed = true;
			summary.runtimeEntryReason = "Ready";

			const std::string text = TerraEngine::Runtime::FormatBootSummary(summary);
			context.Expect(text.find("project=Baseline") != std::string::npos, "Boot summary should include project name.");
			context.Expect(text.find("renderProfile={profile=Baseline") != std::string::npos, "Boot summary should include render profile details.");
			context.Expect(text.find("validation=on") != std::string::npos, "Boot summary should include validation flag.");
			context.Expect(text.find("runtimeEntry=allowed") != std::string::npos, "Boot summary should include runtime entry decision.");
		});
	}
}
