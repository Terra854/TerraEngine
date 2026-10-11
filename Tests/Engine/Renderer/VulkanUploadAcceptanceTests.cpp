// These CPU tests qualify publication policy; they do not simulate native GPU completion.
#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanUploadAcceptance.h"
namespace TerraEngine::Tests {
	void RegisterVulkanUploadAcceptanceTests(TestRegistry& registry) {
		using Renderer::Vulkan::UploadAcceptance;
		registry.Add("Renderer.Efficiency.Upload.CompleteCohortRequiresDescriptorAndHealthyDevice", [](TestContext& t) {
			UploadAcceptance proof(2, true); proof.RecordCopy(true);
			t.Expect(!proof.CanPublish(true, true), "One successful stream cannot publish a two-stream mesh.");
			proof.RecordCopy(true);
			t.Expect(!proof.CanPublish(false, true) && !proof.CanPublish(true, false), "Missing descriptor idle or terminal loss blocks publication.");
			t.Expect(proof.CanPublish(true, true), "All actual completed streams and descriptor proof authorize publication.");
		});
		registry.Add("Renderer.Efficiency.Upload.FailedCopyPoisonsWholeCohort", [](TestContext& t) {
			UploadAcceptance proof(2, true); proof.RecordCopy(true); proof.RecordCopy(false); proof.RecordCopy(true);
			t.Expect(!proof.CanPublish(true, true), "A retry cannot relabel an ambiguous earlier upload as accepted.");
		});
		registry.Add("Renderer.Efficiency.Upload.MissingMappedBytesAndExcessCopiesReject", [](TestContext& t) {
			UploadAcceptance unmapped(1, false); unmapped.RecordCopy(true);
			UploadAcceptance excess(1, true); excess.RecordCopy(true); excess.RecordCopy(true);
			UploadAcceptance empty(0, true);
			t.Expect(!unmapped.CanPublish(true, true) && !excess.CanPublish(true, true) && !empty.CanPublish(true, true), "Incomplete or inconsistent upload cohorts never produce cache owners.");
		});
	}
}
