#include "TestHarness.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/RHI/Vulkan/VulkanLightingDepthTiming.h"

#include <limits>

namespace TerraEngine::Tests
{
	void RegisterRendererEfficiencyEvidenceTests(TestRegistry& registry)
	{
		using namespace Renderer;
		using namespace Renderer::VulkanLightingDepthTiming;
		registry.Add("Renderer.Efficiency.Evidence.RecoveryRevokesPendingSettingsAndTiming", [](TestContext& test) {
			// Exercise the two real policies together: neither queued settings nor
			// accepted old-pool work may cross a renderer lifetime change.
			RendererEfficiencyOptionsQueue options;
			options.BindRendererSession(1);
			auto requested = options.Snapshot().options;
			requested.shadowReuse = RendererEfficiencyShadowReuse::AlwaysRefresh;
			const auto receipt = options.Queue(requested);
			TimingState timing;
			test.Expect(timing.Configure(1, 2, 3, 64, 1), "Configure actual timing policy.");
			Owner owner; owner.rendererSessionGeneration = 1; owner.backendResourceGeneration = 2;
			owner.queryPoolGeneration = 3; owner.frameSerial = 4; owner.commandSerial = 5;
			QueryRange range;
			test.Expect(timing.Begin(owner, range) && timing.End(owner), "Record old-session work.");
			const std::array commands{owner.commandSerial};
			test.Expect(timing.ConfirmSubmitted(commands, 6), "Bind actual accepted command.");
			options.BindRendererSession(7);
			test.Expect(!options.CaptureAtProtectedBoundary() && !options.Restore(receipt.priorToken).accepted,
				"Recovery discards pending settings and rejects the old restoration epoch.");
			test.Expect(!timing.Reset(7, 8, 9) && !timing.CanDestroy(), "Old accepted physical queries stay owned.");
			Result result;
			test.Expect(timing.CompleteSubmission(6, 10) && timing.Resolve(owner, {1, 2, true, true, true}, true, result), "Drain the exact old receipt.");
			test.Expect(result.status == Status::Unavailable && !QualifiesProductionSpeedProof(result), "Canceled lifetime cannot supply a speed sample.");
			test.Expect(timing.Reset(7, 8, 9), "Only settled queries permit replacement.");
		});
		registry.Add("Renderer.Efficiency.Evidence.SpeedQualificationRejectsIncompleteValues", [](TestContext& test) {
			// Start from a result produced by the policy, then test malformed
			// externally copied values through the real speed-proof gate.
			TimingState timing; timing.Configure(1, 2, 3, 64, 0.5);
			Owner owner; owner.rendererSessionGeneration = 1; owner.backendResourceGeneration = 2;
			owner.queryPoolGeneration = 3; owner.frameSerial = 4; owner.commandSerial = 5;
			QueryRange range; timing.Begin(owner, range); timing.End(owner);
			const std::array commands{owner.commandSerial}; timing.ConfirmSubmitted(commands, 6); timing.CompleteSubmission(6, 7);
			Result valid; test.Expect(timing.Resolve(owner, {0, 8, true, true, true}, true, valid) && QualifiesProductionSpeedProof(valid), "Zero begin tick is legitimate with positive elapsed work.");
			for (unsigned fault = 0; fault < 9; ++fault) {
				auto bad = valid;
				if (fault == 0) bad.durationNanoseconds = std::numeric_limits<double>::quiet_NaN();
				if (fault == 1) bad.timestampPeriodNanoseconds = std::numeric_limits<double>::infinity();
				if (fault == 2) bad.durationNanoseconds = -1;
				if (fault == 3) bad.rawTicks = 0;
				if (fault == 4) bad.status = Status::Pending;
				if (fault == 5) bad.beginAvailable = false;
				if (fault == 6) bad.completionSerial = bad.submitSerial - 1;
				if (fault == 7) bad.owner.scope = Scope::WholeView;
				if (fault == 8) bad.owner.purpose = Purpose::Oracle;
				test.Expect(!QualifiesProductionSpeedProof(bad), "Incomplete or unrelated timing cannot qualify production speed.");
			}
		});
		registry.Add("Renderer.Efficiency.Evidence.CoalescedRestoreUsesActualProtectedEpoch", [](TestContext& test) {
			RendererEfficiencyOptionsQueue queue; queue.BindRendererSession(11);
			const auto original = queue.Snapshot();
			auto first = original.options; first.shadowReuse = RendererEfficiencyShadowReuse::AlwaysRefresh;
			const auto firstReceipt = queue.Queue(first);
			auto second = first; second.lightingUploads = RendererEfficiencyLightingUploads::Full;
			const auto secondReceipt = queue.Queue(second);
			test.Expect(queue.Snapshot().settingsGeneration == original.settingsGeneration, "Ingress is not protected visibility.");
			test.Expect(queue.Restore(secondReceipt.priorToken).accepted && queue.CaptureAtProtectedBoundary(), "Restore the preceding queued value through the same boundary.");
			test.Expect(queue.Snapshot().options == first && queue.Snapshot().rendererSessionGeneration == 11, "Coalesced restoration retains exact options and backend lifetime.");
			queue.ResetForNewLifetime();
			test.Expect(!queue.Restore(firstReceipt.priorToken).accepted && queue.Snapshot().options == RendererEfficiencyOptions{}, "Ended lifetime rejects all prior tokens and restores defaults.");
		});
	}
}
