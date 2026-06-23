#include "TestHarness.h"

#include "Resource/ResourceStreaming.h"

namespace TerraEngine::Tests {
	void RegisterResourceStreamingDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Resource.Streaming.Diagnostics.StringsDescribeStates", [](TestContext& context) {
			context.ExpectEqual(std::string_view("deferred"), Resource::ToString(Resource::StreamablePayloadState::Deferred), "Deferred state should be readable in diagnostics.");
			context.ExpectEqual(std::string_view("failed"), Resource::ToString(Resource::StreamablePayloadState::Failed), "Failed state should be readable in diagnostics.");
			context.ExpectEqual(std::string_view("prefab-editing"), Resource::ToString(Resource::StreamingProtectionReason::PrefabEditing), "Prefab protection should be readable in diagnostics.");
		});

		registry.Add("Resource.Streaming.Diagnostics.ClosedByDefault", [](TestContext& context) {
			const Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();

			context.Expect(!policy.diagnosticsEnabled, "Detailed streaming diagnostics should be disabled until a diagnostics surface opens.");
		});
	}
}
