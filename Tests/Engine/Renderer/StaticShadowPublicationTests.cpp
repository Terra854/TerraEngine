// These tests exercise actual pure publication/lifetime policy, not GPU copies,
// driver completion, rasterization or shader cutout execution.
#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanStaticShadowPolicy.h"
#include <algorithm>
#include <limits>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer;
		using namespace Renderer::VulkanStaticShadow;
		StaticShadowAllocationOwner Owner(std::uint64_t id = 1, std::uint32_t layers = 1)
		{
			return {1, 2, id, 3, 4096, 126, layers, {32, 32}};
		}
		ShadowViewKey View(const StaticShadowAllocationOwner& owner)
		{
			ShadowViewKey view;
			view.rendererSessionGeneration = owner.sessionGeneration;
			view.viewId = 1; view.lightId[0] = 1;
			view.allocationGeneration = owner.allocationGeneration;
			view.regionId = owner.allocationIdentity;
			view.extent = owner.extent; view.depthFormat = owner.depthFormat;
			view.resourceKind = static_cast<ShadowResourceKind>(owner.layers == 6 ? 1 : 0);
			view.lightProjectionStamp.semanticRevision = 1;
			for(unsigned i = 0; i < 4; ++i) view.lightProjectionStamp.viewProjection[i * 5] = 1;
			view.lightProjectionStamp.cubeProjectionEvidenceComplete = true;
			for(auto& face : view.lightProjectionStamp.cubeViewProjections)
				for(unsigned i = 0; i < 4; ++i) face[i * 5] = 1;
			return view;
		}
		ShadowRefreshCandidate Candidate(const ShadowViewKey& view, std::uint64_t token = 7, std::uint64_t command = 10)
		{
			ShadowRefreshCandidate result;
			result.view = view; result.membership.reusable = true;
			result.contentToken = token; result.frameSerial = 9; result.commandSerial = command;
			result.requiredFaceMask = result.recordedFaceMask = static_cast<std::uint32_t>(view.resourceKind) == 1 ? 63 : 1;
			return result;
		}
		StaticShadowCompositionRequest Request(std::uint32_t layers = 1)
		{
			StaticShadowCompositionRequest request;
			request.source = Owner(1, layers); request.destination = Owner(2, layers);
			request.staticView = View(request.source); request.finalView = View(request.destination);
			request.staticContentToken = 7; request.currentCommandSerial = 11;
			request.requiredFaceMask = request.sourceFaceMask = layers == 6 ? 63 : 1;
			request.sourceTransferSupported = request.destinationTransferSupported = true;
			request.sourceTransferUsage = request.destinationTransferUsage = true;
			request.sameShadowPipeline = request.lessDepthComparison = true;
			request.colorWritesDisabled = request.cutoutBehaviorPreserved = true;
			return request;
		}
		bool Copy(const StaticShadowCompositionRequest& request, const ShadowCachePublicationLedger& ledger)
		{
			return BuildStaticShadowCompositionPlan(request, ledger).route == StaticShadowCompositionRoute::CacheCopyOverlay;
		}
		void Submit(TestContext& t, ShadowCachePublicationLedger& ledger, const ShadowViewKey& view)
		{
			t.Expect(ledger.Record(Candidate(view)), "Actual copied candidate records first.");
			t.Expect(ledger.ConfirmSubmission(10, 20), "Accepted submit publishes its exact token.");
		}
	}
	void RegisterStaticShadowPublicationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.StaticShadow.RegisterAtomicCharge", [](TestContext& t) {
			StaticShadowLifetimeTracker tracker; auto a = Owner();
			t.Expect(tracker.RegisterAllocation(a) && tracker.RegisterAllocation(a), "Exact registration is idempotent.");
			t.ExpectEqual(tracker.ChargedBytes(), a.actualBytes, "One physical allocation is charged once.");
			a.actualBytes++; t.Expect(!tracker.RegisterAllocation(a), "Changed size cannot relabel a charged allocation.");
			a = Owner(2); a.actualBytes = std::numeric_limits<std::uint64_t>::max();
			t.Expect(!tracker.RegisterAllocation(a), "Aggregate charge overflow rejects before mutation.");
			t.ExpectEqual(tracker.ResidentBytes(), std::uint64_t(4096), "Rejected registration leaves charge unchanged.");
		});
		registry.Add("Renderer.Efficiency.StaticShadow.RecordedRetirementAndDiscard", [](TestContext& t) {
			StaticShadowLifetimeTracker tracker; const auto a = Owner();
			t.Expect(tracker.RegisterAllocation(a) && tracker.ReserveRecordedUse(a, 10, 2), "Reference is reserved before command recording.");
			t.Expect(tracker.ReserveRecordedUse(a, 10, 2) && !tracker.ReserveRecordedUse(a, 10, 1), "Same use is idempotent; changed frame conflicts.");
			t.Expect(tracker.RequestRetirement(a) && !tracker.CanRelease(a), "Pending recorded primary blocks retirement.");
			t.Expect(!tracker.ReserveRecordedUse(a, 11, 0), "Retiring allocation rejects new recording.");
			tracker.CompleteSubmission(20); t.Expect(!tracker.CanRelease(a), "An unrelated fence cannot settle unsubmitted recording.");
			tracker.DiscardUnsubmitted(10);
			t.Expect(tracker.CanRelease(a) && tracker.ReleaseCompleted(a), "Actual abandonment settles recorded use.");
			t.ExpectEqual(tracker.ChargedBytes(), std::uint64_t(0), "Only release removes physical charge.");
		});
		registry.Add("Renderer.Efficiency.StaticShadow.AcceptedSubmitExactCompletion", [](TestContext& t) {
			StaticShadowLifetimeTracker tracker; const auto a = Owner();
			t.Expect(tracker.RegisterAllocation(a) && tracker.ReserveRecordedUse(a, 10, 0), "Prepare actual use.");
			t.Expect(tracker.PreserveSubmittedUse(10, 20) && tracker.PreserveSubmittedUse(10, 20), "Accepted submission is idempotent.");
			t.Expect(!tracker.PreserveSubmittedUse(10, 21), "Conflicting receipt cannot rewrite accepted work.");
			tracker.RequestRetirement(a); tracker.DiscardUnsubmitted(10); tracker.CompleteSubmission(21);
			t.Expect(!tracker.CanRelease(a), "Neither discard nor newer unrelated receipt releases accepted work.");
			tracker.CompleteSubmission(20); t.Expect(tracker.ReleaseCompleted(a), "Exact actual completion retires accepted work.");
			// Completion removes old use rows, but its serial cannot qualify a later
			// recorded use of the same still-resident allocation.
			StaticShadowLifetimeTracker resident;
			resident.RegisterAllocation(a); resident.ReserveRecordedUse(a, 10, 0); resident.PreserveSubmittedUse(10, 20);
			resident.ReserveRecordedUse(a, 11, 1); resident.PreserveSubmittedUse(11, 21);
			t.Expect(resident.PreserveSubmittedUse(10, 20), "Exact historical receipt remains idempotent after a newer submission.");
			resident.RequestRetirement(a); resident.CompleteSubmission(21);
			t.Expect(!resident.CanRelease(a), "Newer exact completion does not erase the older accepted use.");
			resident.CompleteSubmission(20); t.Expect(resident.ReleaseCompleted(a), "Both accepted references must settle.");
			StaticShadowLifetimeTracker stale;
			stale.RegisterAllocation(a); stale.ReserveRecordedUse(a, 10, 0); stale.PreserveSubmittedUse(10, 20); stale.CompleteSubmission(20);
			t.Expect(!stale.ReserveRecordedUse(a, 10, 0) && stale.ReserveRecordedUse(a, 11, 0)
				&& !stale.PreserveSubmittedUse(11, 20), "Completed command and receipt cannot be replayed as a fresh use.");
		});
		registry.Add("Renderer.Efficiency.StaticShadow.AtomicMultipleOwners", [](TestContext& t) {
			StaticShadowLifetimeTracker tracker; const auto a = Owner(), b = Owner(2);
			t.Expect(tracker.RegisterAllocation(a) && tracker.RegisterAllocation(b), "Register independent physical owners.");
			t.Expect(tracker.ReserveRecordedUse(a, 10, 0) && tracker.ReserveRecordedUse(b, 10, 0), "One command may reference both images.");
			t.Expect(tracker.PreserveSubmittedUse(10, 20), "One exact native receipt qualifies both references.");
			t.Expect(!tracker.PreserveSubmittedUse(10, 21), "Conflicting batch is rejected without partial reference updates.");
			tracker.RequestRetirement(a); tracker.RequestRetirement(b); tracker.CompleteSubmission(21);
			t.Expect(!tracker.CanRelease(a) && !tracker.CanRelease(b), "Rejected receipt changes neither owner.");
			tracker.CompleteSubmission(20); t.Expect(tracker.ReleaseCompleted(a) && tracker.ReleaseCompleted(b), "Exact completion settles the whole command.");
			StaticShadowLifetimeTracker accepted;
			accepted.RegisterAllocation(a); accepted.RegisterAllocation(b); accepted.ReserveRecordedUse(a, 10, 0); accepted.PreserveSubmittedUse(10, 20);
			t.Expect(!accepted.ReserveRecordedUse(b, 10, 0), "A new image reference cannot be appended after its primary was accepted.");
		});
		registry.Add("Renderer.Efficiency.StaticShadow.BudgetZeroStillCharged", [](TestContext& t) {
			StaticShadowLifetimeTracker tracker; const auto a = Owner(), b = Owner(2, 6);
			t.Expect(tracker.RegisterAllocation(a) && tracker.RegisterAllocation(b), "Whole cube allocation is one atomic charge.");
			tracker.ReserveRecordedUse(a, 10, 0); tracker.PreserveSubmittedUse(10, 20);
			tracker.ReserveRecordedUse(a, 11, 2);
			// Capture both accepted and still-recorded uses without granting release.
			const auto originalCapture = tracker.CaptureAllocations();
			t.ExpectEqual(originalCapture.size(), std::size_t(2), "Both charged physical owners are captured.");
			if(originalCapture.size() != 2) return;
			t.Expect(originalCapture[0].owner == a && originalCapture[1].owner == b
				&& !originalCapture[0].retiring && originalCapture[1].uses.empty(), "Capture preserves whole owners and idle allocation state.");
			t.Expect(originalCapture[0].uses == std::vector<StaticShadowUseReceipt>{{10, 20, 0}, {11, 0, 2}},
				"Exact native command, submission and physical frame slot are copied.");
			tracker.RequestRetirement(a); tracker.RequestRetirement(b);
			t.ExpectEqual(tracker.ResidentBytes(), std::uint64_t(0), "Budget zero stops resident admission.");
			t.ExpectEqual(tracker.RetiringBytes(), std::uint64_t(8192), "All lowering/zero retirement bytes remain charged.");
			t.Expect(tracker.ReleaseCompleted(b) && !tracker.ReleaseCompleted(a), "Only unused cube releases immediately.");
			ShadowCachePublicationLedger logical; logical.Reset(2);
			t.ExpectEqual(tracker.ChargedBytes(), std::uint64_t(4096), "Logical/session reset cannot erase physical charge.");
			auto retiringCapture = tracker.CaptureAllocations();
			t.Expect(retiringCapture.size() == 1 && retiringCapture[0].owner == a && retiringCapture[0].retiring
				&& retiringCapture[0].uses == originalCapture[0].uses, "Logical reset and retirement retain every outstanding physical reference.");
			// A consumer can modify its copied diagnostic without touching authority.
			retiringCapture.clear();
			t.ExpectEqual(tracker.CaptureAllocations().size(), std::size_t(1), "Diagnostic storage is independently owned.");
			auto stale = a; ++stale.sessionGeneration; t.Expect(!tracker.ReleaseCompleted(stale), "New session cannot release old owner by alias.");
			tracker.CompleteSubmission(20);
			const auto completedCapture = tracker.CaptureAllocations();
			t.Expect(completedCapture.size() == 1 && completedCapture[0].owner == a
				&& completedCapture[0].uses == std::vector<StaticShadowUseReceipt>{{11, 0, 2}}, "Exact completion removes only accepted uses, never the charged owner.");
			t.Expect(!tracker.ReleaseCompleted(a), "Still-recorded command remains charged after accepted completion.");
			tracker.DiscardUnsubmitted(11);
			const auto discardedCapture = tracker.CaptureAllocations();
			t.Expect(discardedCapture.size() == 1 && discardedCapture[0].uses.empty(), "Explicit discard removes only unsubmitted references.");
			t.Expect(tracker.ReleaseCompleted(a), "Old accepted owner still receives its real completion.");
			t.Expect(tracker.CaptureAllocations().empty(), "Physical release removes the diagnostic owner.");
			t.Expect(originalCapture[0].uses.size() == 2 && !originalCapture[0].retiring, "Retained earlier capture remains immutable by later tracker operations.");
		});
		registry.Add("Renderer.Efficiency.StaticShadow.TerminalAndInvalidOwners", [](TestContext& t) {
			StaticShadowLifetimeTracker tracker; auto a = Owner();
			for(unsigned field = 0; field < 9; ++field) {
				auto bad = a;
				switch(field) {case 0:bad.sessionGeneration=0;break;case 1:bad.backendGeneration=0;break;case 2:bad.allocationIdentity=0;break;case 3:bad.allocationGeneration=0;break;case 4:bad.actualBytes=0;break;case 5:bad.depthFormat=0;break;case 6:bad.layers=5;break;case 7:bad.extent[0]=0;break;default:bad.extent[1]=0;}
				t.Expect(!tracker.RegisterAllocation(bad), "Invalid physical identity/whole-layer shape cannot acquire charge.");
			}
			t.Expect(tracker.RegisterAllocation(a) && !tracker.ReserveRecordedUse(a, 10, 3), "Physical frame indices remain bounded.");
			tracker.ReserveRecordedUse(a, 10, 1); tracker.PreserveSubmittedUse(10, 20);
			t.Expect(!tracker.TerminalRelease(a, static_cast<StaticShadowTerminalProof>(255)), "Unknown terminal proof cannot free memory.");
			t.Expect(tracker.TerminalRelease(a, StaticShadowTerminalProof::DeviceLost), "Explicit terminal device loss permits teardown.");
			t.Expect(!tracker.TerminalRelease(a, StaticShadowTerminalProof::DeviceLost), "Release happens exactly once.");
			t.Expect(tracker.RegisterAllocation(Owner(3)) && tracker.TerminalRelease(Owner(3), StaticShadowTerminalProof::DeviceIdle), "Terminal idle after abandonment permits teardown.");
		});
		registry.Add("Renderer.Efficiency.StaticShadow.IndependentTokensAndOverwrite", [](TestContext& t) {
			const auto request = Request(); ShadowCachePublicationLedger staticLedger, finalLedger;
			Submit(t, staticLedger, request.staticView);
			t.Expect(Copy(request, staticLedger), "Exact submitted static source may seed a final copy.");
			t.Expect(!finalLedger.CanReuse(request.finalView, 7), "Static submission cannot authorize final content.");
			auto final = Candidate(request.finalView, 8, 11); t.Expect(finalLedger.Record(final) && finalLedger.ConfirmSubmission(11, 21), "Final uses its own actual receipt/token.");
			t.Expect(!finalLedger.CanReuse(request.finalView, 7), "Static and final tokens stay independent.");
			auto overwrite = Candidate(request.staticView, 9, 12); t.Expect(staticLedger.Record(overwrite), "Same-frame new writer blocks old physical content.");
			t.Expect(!Copy(request, staticLedger), "Old static owner cannot copy while B is pending.");
			// Retiring one image must not invalidate another view's independent image.
			ShadowCachePublicationLedger regions;
			StaticShadowLifetimeTracker physical;
			const auto ownerA = Owner(31, 6), ownerB = Owner(32, 6);
			const auto viewA = View(ownerA), viewB = View(ownerB);
			t.Expect(physical.RegisterAllocation(ownerA) && physical.RegisterAllocation(ownerB), "Independent images remain physically charged.");
			t.Expect(regions.Record(Candidate(viewA, 31, 31)) && regions.ConfirmSubmission(31, 41)
				&& regions.Record(Candidate(viewB, 32, 32)) && regions.ConfirmSubmission(32, 42), "Both physical regions have actual accepted owners.");
			auto aliasA = viewA; aliasA.faceIndex = 5; aliasA.viewId = 9;
			// Cube layers overlap physically even when their logical view differs.
			regions.InvalidatePhysicalRegion(aliasA);
			t.Expect(!regions.CanReuse(viewA, 31) && !regions.Find(viewA)
				&& !regions.FindSubmittedPhysicalOwner(viewA), "Exact physical revocation removes A's logical and submitted ownership.");
			const auto submittedB = regions.FindSubmittedPhysicalOwner(viewB);
			t.Expect(regions.CanReuse(viewB, 32) && submittedB && submittedB->contentToken == 32
				&& submittedB->commandSerial == 32 && submittedB->submitSerial == 42, "B retains its exact accepted token and native receipts.");
			t.Expect(regions.Record(Candidate(viewA, 33, 33)) && regions.Record(Candidate(viewB, 34, 34)), "Separate pending writers are recorded.");
			regions.InvalidatePhysicalRegion(aliasA);
			t.Expect(!regions.HasRecordedCandidate(viewA, 33, 33) && !regions.Find(viewA)
				&& regions.HasRecordedCandidate(viewB, 34, 34), "Only A's pending physical writer is revoked.");
			// A newly recorded writer cannot recycle an earlier accepted submit serial.
			t.Expect(regions.Record(Candidate(viewA, 35, 35)) && !regions.ConfirmSubmission(35, 41), "Region invalidation preserves the global accepted submission clock.");
			t.Expect(regions.HasRecordedCandidate(viewB, 34, 34) && regions.ConfirmSubmission(34, 43)
				&& regions.CanReuse(viewB, 34), "B's pending token remains publishable under its actual new receipt.");
			t.Expect(regions.Record(Candidate(viewA, 36, 36)) && regions.ConfirmSubmission(36, 44)
				&& regions.CanReuse(viewA, 36), "Revocation also removes stale blocking markers so genuinely new A can publish.");
			t.ExpectEqual(physical.ChargedBytes(), std::uint64_t(8192), "Logical region revocation never releases native image charges.");
		});
		registry.Add("Renderer.Efficiency.StaticShadow.FreshSamePrimaryAndCube", [](TestContext& t) {
			auto request = Request(6); ShadowCachePublicationLedger ledger;
			request.sourceRecordedCommandSerial = request.currentCommandSerial = 10;
			t.Expect(ledger.Record(Candidate(request.staticView)), "All six faces have recorded in the same primary.");
			t.Expect(Copy(request, ledger), "Exact complete pending source can copy before its shared submit.");
			auto wrong = request; wrong.currentCommandSerial = 11; t.Expect(!Copy(wrong, ledger), "Different primary cannot borrow unsubmitted source.");
			wrong = request; ++wrong.staticContentToken; t.Expect(!Copy(wrong, ledger), "Pending command alone cannot authorize another token.");
			wrong = request; wrong.sourceFaceMask = 31; t.Expect(!Copy(wrong, ledger), "Five faces never authorize cube composition.");
			ledger.Discard(10); t.Expect(!Copy(request, ledger), "Failed/skipped submit cannot leave pending authorization.");
			ShadowCachePublicationLedger partial;
			auto incomplete = Candidate(request.staticView); incomplete.recordedFaceMask = 31;
			t.Expect(!partial.Record(incomplete) && !Copy(request, partial), "Actual five-face recording cannot authorize the sixth face by metadata.");
			ShadowCachePublicationLedger failed;
			t.Expect(failed.Record(Candidate(request.staticView)) && !failed.ConfirmSubmission(10, 20, false)
				&& !Copy(request, failed), "Actual failed submit removes fresh same-command authorization.");
		});
		registry.Add("Renderer.Efficiency.StaticShadow.TransferAndPipelineFailClosed", [](TestContext& t) {
			const auto request = Request(); ShadowCachePublicationLedger ledger; Submit(t, ledger, request.staticView);
			t.Expect(Copy(request, ledger), "Complete same-format depth copy is supported.");
			for(unsigned field = 0; field < 15; ++field) {
				auto bad = request;
				switch(field) {case 0:bad.sourceTransferSupported=false;break;case 1:bad.destinationTransferSupported=false;break;case 2:bad.sourceTransferUsage=false;break;case 3:bad.destinationTransferUsage=false;break;case 4:bad.sameShadowPipeline=false;break;case 5:bad.lessDepthComparison=false;break;case 6:bad.colorWritesDisabled=false;break;case 7:bad.cutoutBehaviorPreserved=false;break;case 8:++bad.destination.depthFormat;break;case 9:++bad.destination.extent[0];break;case 10:bad.destination.layers=6;break;case 11:++bad.source.allocationGeneration;break;case 12:++bad.destination.sessionGeneration;break;case 13:bad.requiredFaceMask=63;break;default:bad.destination=bad.source;bad.finalView=bad.staticView;}
				t.Expect(!Copy(bad, ledger), "Missing transfer/pipeline/owner proof preserves combined rendering.");
			}
		});
		registry.Add("Renderer.Efficiency.StaticShadow.CpuLessOverlayCutoutParity", [](TestContext& t) {
			const auto request = Request(); ShadowCachePublicationLedger ledger; Submit(t, ledger, request.staticView);
			t.Expect(Copy(request, ledger), "CPU reference exercises the selected copy-plus-LESS policy.");
			struct Fragment {unsigned pixel; float depth; bool survivesCutout;};
			const std::array<Fragment, 4> staticFragments{{{0,.4f,true},{1,.2f,false},{2,.6f,true},{3,.5f,true}}};
			const std::array<Fragment, 5> dynamicFragments{{{0,.3f,true},{1,.7f,true},{2,.8f,true},{3,.5f,true},{0,.1f,false}}};
			std::array<float,4> combined{1,1,1,1}, cached=combined;
			const auto overlay=[](auto& depth,const auto& fragments){for(const auto& fragment:fragments) if(fragment.survivesCutout && fragment.depth<depth[fragment.pixel]) depth[fragment.pixel]=fragment.depth;};
			overlay(combined,staticFragments); overlay(combined,dynamicFragments);
			overlay(cached,staticFragments); auto copied=cached; overlay(copied,dynamicFragments);
			t.Expect(copied==combined && copied==std::array<float,4>{.3f,.7f,.6f,.5f}, "Full static copy and identical LESS/discard semantics preserve exact overlapping depth.");
		});
	}
}
