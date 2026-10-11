// These tests exercise receipt/capacity policy, not simulated GPU success.
#include "TestHarness.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "Renderer/RHI/Vulkan/VulkanShadowValidationReadback.h"
namespace TerraEngine::Tests
{
	void RegisterShadowValidationReadbackTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.ShadowReadback.ControlPublicationRearmsExactNativeSample", [](TestContext& t) {
			// Exercise the same retained admission state used by RecordFace. The
			// host first captures cameras, then publishes selected geometry under
			// unchanged fixture correlation and a new protected control generation.
			Renderer::RendererEfficiencyEvidence camera;
			camera.viewType = RenderViewType::Scene; camera.viewId = 1;
			camera.fixtureCorrelation = {"owned-operation", "selected-lod-09", 0, 10, 20, 30};
			camera.fixtureControlGeneration = 40; camera.frameSerial = 100; camera.commandSerial = 101;
			camera.status = Renderer::RendererEfficiencyEvidenceStatus::Valid;
			Renderer::ShadowValidationSampleIntent retained{camera.viewType, camera.viewId};
			t.Expect(Renderer::AdmitShadowValidationSample(retained, camera), "Initial cameras-only native frame must sample.");
			auto later = camera; later.frameSerial = 102;
			t.Expect(!Renderer::AdmitShadowValidationSample(retained, later), "Unchanged complete token must freeze later frames.");
			t.Expect(Renderer::AdmitShadowValidationSample(retained, camera), "Remaining faces in the exact sampled frame remain admissible.");
			auto selected = later; selected.fixtureControlGeneration = 41; selected.commandSerial = 103;
			t.Expect(Renderer::AdmitShadowValidationSample(retained, selected), "New selected-LOD control publication must rearm despite unchanged correlation.");
			t.Expect(retained.controlGeneration == 41 && retained.frameSerial == 102, "Actual new control generation owns its newly sampled frame.");
			Renderer::RendererEfficiencyFixtureIntent intent; intent.correlation = selected.fixtureCorrelation; intent.controlGeneration = 41; intent.controlsApplied = true;
			t.Expect(!Renderer::HasRendererEfficiencyFixtureControlAcknowledgment(std::span(&camera, 1), intent), "Old completed camera generation must never acknowledge selected geometry.");
			t.Expect(Renderer::HasRendererEfficiencyFixtureControlAcknowledgment(std::span(&selected, 1), intent), "Actual terminal command evidence for the new generation acknowledges its controls.");
			auto repeated = selected; repeated.frameSerial = 104;
			t.Expect(!Renderer::AdmitShadowValidationSample(retained, repeated), "Unchanged selected generation must still sample only one frame.");
			auto next = repeated; next.fixtureCorrelation.stepFrame = 1;
			t.Expect(Renderer::AdmitShadowValidationSample(retained, next), "A genuinely new correlation admits the next native frame.");
		});
		registry.Add("Renderer.Efficiency.ShadowReadback.FixtureActionNeedsExactRealReceiptTransitions", [](TestContext& t) {
			Renderer::RendererEfficiencyFixtureActionReceipt action;
			action.injection = Renderer::RendererEfficiencyFixtureInjection::IncompletePointFaces;
			action.recorded = true; action.commandSerial = 10; action.requiredFaceMask = 0x3f; action.recordedFaceMask = 0x1f;
			t.Expect(!Renderer::MarkShadowValidationFixtureActionCompleted(action, 20, 20), "A recorded partial cube cannot manufacture a submit/completion receipt.");
			t.Expect(!Renderer::MarkShadowValidationFixtureActionSubmitted(action, 11, 20), "Wrong primary cannot qualify the recorded action.");
			t.ExpectEqual(action.submitSerial, std::uint64_t(0), "Rejected receipt leaves action atomic.");
			t.Expect(Renderer::MarkShadowValidationFixtureActionSubmitted(action, 10, 20), "Caller actual submit receipt qualifies exact command.");
			t.Expect(!Renderer::MarkShadowValidationFixtureActionCompleted(action, 20, 19), "Older fence observation cannot qualify completion.");
			t.Expect(!Renderer::MarkShadowValidationFixtureActionSubmitted(action, 10, 21), "Replay cannot replace actual accepted receipt.");
			t.Expect(Renderer::MarkShadowValidationFixtureActionCompleted(action, 20, 20), "Actual completion qualifies the failed sample action, not its pixels.");
			t.Expect(action.completed && action.recordedFaceMask == 0x1f && action.requiredFaceMask == 0x3f, "Completion preserves incomplete native cube proof.");
		});
		registry.Add("Renderer.Efficiency.ShadowReadback.RecordedAbortNeverGainsCompletionWithoutSubmit", [](TestContext& t) {
			Renderer::RendererEfficiencyFixtureActionReceipt action;
			action.injection = Renderer::RendererEfficiencyFixtureInjection::RecordedSubmitAbort;
			action.recorded = true; action.commandSerial = 10;
			t.Expect(!Renderer::MarkShadowValidationFixtureActionCompleted(action, 0, 0), "Dropped recorded primary has no queue/fence proof.");
			t.Expect(!action.submitted && !action.completed && action.submitSerial == 0 && action.completionSerial == 0, "Abort remains truthful recorded-only evidence.");
		});
		registry.Add("Renderer.Efficiency.ShadowReadback.RequiresActualSubmissionAndCompletion", [](TestContext& t) {
			Renderer::ShadowValidationReceiptTracker gate;
			t.Expect(gate.Reserve(1, 10, 16), "Native copy reservation must succeed.");
			t.Expect(gate.Complete(20, 20).empty(), "Recording is never completed evidence.");
			const std::uint64_t command = 10;
			t.Expect(gate.Confirm(std::span(&command, 1), 20), "Actual successful submit qualifies reservation.");
			t.Expect(gate.Complete(20, 19).empty(), "An older completion cannot qualify a submit.");
			t.ExpectEqual(gate.Complete(20, 20).size(), std::size_t(1), "Real completion exposes one payload.");
			t.ExpectEqual(gate.ReservedBytes(), std::uint64_t(16), "Completion cannot release staging before host copy.");
			t.Expect(gate.Release(1), "Host copy releases staging accounting.");
		});
		registry.Add("Renderer.Efficiency.ShadowReadback.FailedCommandNeverCompletes", [](TestContext& t) {
			Renderer::ShadowValidationReceiptTracker gate;
			t.Expect(gate.Reserve(1, 10, 16), "Reserve actual candidate.");
			t.ExpectEqual(gate.Discard(10).size(), std::size_t(1), "Unsubmitted failure retires its copy.");
			const std::uint64_t command = 10;
			t.Expect(!gate.Confirm(std::span(&command, 1), 20), "Discarded command has no receipt.");
			t.Expect(gate.Complete(20, 20).empty(), "Failure never manufactures pixels.");
		});
		registry.Add("Renderer.Efficiency.ShadowReadback.CapacityRetainsInflightOwnership", [](TestContext& t) {
			Renderer::ShadowValidationReceiptTracker gate(32, 2);
			t.Expect(gate.Reserve(1, 10, 32), "Full native payload fits bound.");
			t.Expect(!gate.Reserve(2, 11, 1), "Capacity rejects whole copy rather than reducing pixels.");
			const std::uint64_t command = 10;
			t.Expect(gate.Confirm(std::span(&command, 1), 20), "Submitted copy owns its memory.");
			t.Expect(gate.Discard(10).empty(), "Submitted memory cannot be freed on logical cancellation.");
			t.ExpectEqual(gate.ReservedBytes(), std::uint64_t(32), "Capacity remains reserved until actual completion.");
		});
		registry.Add("Renderer.Efficiency.ShadowReadback.ReplayedAndConflictingReceiptsRejectAtomically", [](TestContext& t) {
			Renderer::ShadowValidationReceiptTracker gate;
			t.Expect(gate.Reserve(1, 10, 16) && gate.Reserve(2, 11, 16), "Two actual copies reserve independently.");
			const std::array<std::uint64_t, 2> duplicate{10, 10};
			t.Expect(!gate.Confirm(duplicate, 20), "Duplicate actual command coverage is rejected atomically.");
			t.Expect(gate.Complete(20, 20).empty(), "Rejected receipt promotes nothing.");
			const std::array<std::uint64_t, 2> actual{10, 11};
			t.Expect(gate.Confirm(actual, 20), "Valid actual ordered coverage promotes both.");
			t.Expect(!gate.Confirm(actual, 21), "A command cannot gain a second receipt.");
			t.ExpectEqual(gate.Complete(20, 20).size(), std::size_t(2), "One real receipt completes both copies.");
			t.Expect(gate.Release(1) && gate.Release(2), "Copied memory retires once.");
			t.Expect(gate.Complete(20, 20).empty(), "Released payloads cannot be exported again.");
		});
		registry.Add("Renderer.Efficiency.ShadowReadback.QueueSuccessSurvivesReceiptAllocationFailure", [](TestContext& t) {
			Renderer::ShadowValidationReceiptTracker gate;
			t.Expect(gate.Reserve(1, 10, 16), "Native candidate reserves before real submit.");
			gate.PreserveSubmittedCommand(10, 20); // Actual queue-success receipt, not a fake GPU test.
			t.Expect(gate.Discard(10).empty(), "Allocating batch failure cannot release accepted GPU staging.");
			t.ExpectEqual(gate.Complete(20, 20).size(), std::size_t(1), "Existing real fence qualifies the preserved receipt without batching.");
			t.Expect(gate.Release(1), "Safe host copy can retire the retained allocation.");
		});
		registry.Add("Renderer.Efficiency.ShadowReadback.FullPixelCoverageAndRawDepthPreserved", [](TestContext& t) {
			const std::array<float, 6> native{1, 0, 0.75f, 1, 0.999999f, 0.25f};
			std::vector<std::uint8_t> bytes(sizeof(native)); std::memcpy(bytes.data(), native.data(), bytes.size());
			const auto original = bytes; std::vector<std::uint8_t> coverage;
			t.Expect(Renderer::DecodeShadowDepthCoverage(bytes, 3, 2, coverage), "Every native pixel decodes.");
			t.Expect(coverage == std::vector<std::uint8_t>({0, 1, 1, 0, 1, 1}), "Clear and occupied pixels have exact full coverage.");
			t.Expect(bytes == original, "Raw native float depth is never quantized or resized.");
		});
		registry.Add("Renderer.Efficiency.ShadowReadback.BadExtentAndNonfiniteDepthRejectAtomically", [](TestContext& t) {
			std::array<float, 2> native{0.5f, std::numeric_limits<float>::quiet_NaN()};
			std::vector<std::uint8_t> bytes(sizeof(native)); std::memcpy(bytes.data(), native.data(), bytes.size());
			std::vector<std::uint8_t> coverage{9};
			t.Expect(!Renderer::DecodeShadowDepthCoverage(bytes, 2, 2, coverage), "Camera dimensions cannot substitute for native shadow extent.");
			t.Expect(!Renderer::DecodeShadowDepthCoverage(bytes, 2, 1, coverage), "Nonfinite native depth is unavailable.");
			t.Expect(coverage == std::vector<std::uint8_t>({9}), "Rejected decode publishes no partial coverage.");
		});
	}
}
