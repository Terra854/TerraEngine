// Headless lifecycle guards exercise the production typed request/receipt checks.
// They do not pretend to create a Vulkan device or prove GPU completion.
#include "TestHarness.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyDeviceRecreation.h"

namespace TerraEngine::EditorTests
{
	void RegisterRendererEfficiencyDeviceRecreationTests(TestRegistry& registry)
	{
		const auto request = [] {
			RendererEfficiencyDeviceRecreationRequest row;
			row.operationId = "owned-op"; row.fixtureStepId = "device-recovery";
			row.fixtureId = "fixture"; row.fixtureHash = "actual-marker-hash";
			row.projectRoot = "owned-project"; row.projectGeneration = 3;
			row.expectedRendererSessionGeneration = 2; row.expectedBackendResourceGeneration = 7;
			return row;
		};
		registry.Add("Renderer.Efficiency.DeviceRecreation.IncompleteOwnerRejected", [request](TestContext& t) {
			auto row = request();
			t.Expect(IsCompleteRendererEfficiencyDeviceRecreationRequest(row), "Owned request is structurally complete.");
			row.fixtureHash.clear(); t.Expect(!IsCompleteRendererEfficiencyDeviceRecreationRequest(row), "Missing marker cannot authorize teardown.");
			row = request(); row.projectGeneration = 0; t.Expect(!IsCompleteRendererEfficiencyDeviceRecreationRequest(row), "Absent project lifetime rejects.");
			row = request(); row.expectedRendererSessionGeneration = 0; t.Expect(!IsCompleteRendererEfficiencyDeviceRecreationRequest(row), "Absent native lifetime rejects.");
		});
		registry.Add("Renderer.Efficiency.DeviceRecreation.StaleBackendRejectedBeforeDetach", [request](TestContext& t) {
			const auto row = request();
			t.Expect(MatchesRendererEfficiencyDeviceRecreationBackend(row, 2, 7), "Exact current native lifetime is accepted.");
			t.Expect(!MatchesRendererEfficiencyDeviceRecreationBackend(row, 3, 7), "Old renderer session cannot tear down the new device.");
			t.Expect(!MatchesRendererEfficiencyDeviceRecreationBackend(row, 2, 8), "Old presentation resources cannot authorize teardown.");
		});
		registry.Add("Renderer.Efficiency.DeviceRecreation.CompletionRequiresIdleRebindAndFreshNativeLifetime", [request](TestContext& t) {
			RendererEfficiencyDeviceRecreationReceipt row; row.request = request();
			row.oldRendererSessionGeneration = 2; row.oldBackendResourceGeneration = 7;
			row.newRendererSessionGeneration = 3; row.newBackendResourceGeneration = 8;
			row.status = RendererEfficiencyDeviceRecreationStatus::Pending;
			t.Expect(!HasCompletedRendererEfficiencyDeviceRecreation(row), "Queued request is never a native recovery receipt.");
			row.status = RendererEfficiencyDeviceRecreationStatus::Completed; row.idleProven = true;
			t.Expect(!HasCompletedRendererEfficiencyDeviceRecreation(row), "Device recreation without UI rebind cannot complete.");
			row.rebound = true; t.Expect(HasCompletedRendererEfficiencyDeviceRecreation(row), "All real lifetime/rebind fields qualify completion.");
			row.newRendererSessionGeneration = 2; t.Expect(!HasCompletedRendererEfficiencyDeviceRecreation(row), "Same numeric device lifetime cannot fabricate recovery.");
			row.newRendererSessionGeneration = 3; row.status = RendererEfficiencyDeviceRecreationStatus::Unavailable;
			t.Expect(!HasCompletedRendererEfficiencyDeviceRecreation(row), "Unavailable teardown never becomes a passed recovery.");
		});
	}
}
