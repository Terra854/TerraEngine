#include "TestHarness.h"

#include "Resource/ResourceStreaming.h"

namespace TerraEngine::Tests {
	void RegisterSceneStreamingDeferredLoadTests(TestRegistry& registry)
	{
		registry.Add("Scene.Streaming.DeferredLoad.PolicyRegistersDeferredPayloads", [](TestContext& context) {
			Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();
			policy.deferScenePayloadsOnLoad = true;

			context.Expect(Resource::StateAfterRegistration(policy.deferScenePayloadsOnLoad) == Resource::StreamablePayloadState::Deferred, "Scene references should be able to register metadata without loading payloads.");
		});

		registry.Add("Scene.Streaming.DeferredLoad.DisabledStreamingPreservesRegistration", [](TestContext& context) {
			Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();
			policy.enabled = false;
			policy.deferScenePayloadsOnLoad = false;

			context.Expect(Resource::StateAfterRegistration(policy.deferScenePayloadsOnLoad) == Resource::StreamablePayloadState::Registered, "Disabled scene deferral should leave legacy registration behavior available.");
		});
	}
}
