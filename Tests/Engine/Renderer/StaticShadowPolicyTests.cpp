#include "TestHarness.h"
#include "Renderer/ShadowWorkPlan.h"
#include <limits>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer;
		ShadowViewKey StaticView(unsigned id = 1)
		{
			ShadowViewKey view; view.rendererSessionGeneration = 1; view.allocationGeneration = 2;
			view.lightId[0] = static_cast<std::uint8_t>(id); view.extent = {512, 512};
			view.depthFormat = 126; view.lightProjectionStamp.viewProjection[0] = 1; return view;
		}
		ShadowCasterStampInput StaticInput(unsigned id)
		{
			ShadowCasterStampInput input; auto& row = input.semantic;
			row.objectId[0] = static_cast<std::uint8_t>(id); row.meshId[0] = 1;
			row.lifetimeGeneration = 2; row.meshletCount = 3; row.evidenceMask = kCompleteShadowCasterEvidence;
			row.staticSupport = {true, true, true, true, true, true}; return input;
		}
		StaticShadowAdmissionRequest Request(unsigned id, std::uint64_t bytes, double priority = 0)
		{
			return {StaticView(id), priority, bytes, 0, 1};
		}
		StaticShadowAllocationCharge Charge(const StaticShadowAdmissionRequest& request,
			StaticShadowChargeState state = StaticShadowChargeState::Resident)
		{
			return {request.view, 5, request.queriedDepthBytes, request.queriedAuxBytes, request.retainedLightPriority, state};
		}
	}
	void RegisterStaticShadowPolicyTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.ExplicitSupport", [](TestContext& test) {
			auto row = StaticInput(1).semantic;
			test.Expect(ClassifyStaticShadowCaster(row).eligible, "Complete explicit native/static proof admits the caster.");
			for (unsigned lane = 0; lane < 6; ++lane) {
				auto changed = row;
				switch (lane) { case 0: changed.staticSupport.authoredStatic = false; break;
				case 1: changed.staticSupport.knownDefaultShadowAbi = false; break;
				case 2: changed.staticSupport.ownerPublicationsComplete = false; break;
				case 3: changed.staticSupport.materialShadowSupported = false; break;
				case 4: changed.staticSupport.geometrySupported = false; break;
				case 5: changed.staticSupport.deformationAbsent = false; break; }
				test.Expect(!ClassifyStaticShadowCaster(changed).eligible, "Missing authoring, custom, geometry or deformation proof stays dynamic.");
			}
			row.evidenceMask = 0;
			test.Expect(!ClassifyStaticShadowCaster(row).eligible, "Support claims do not replace full semantic evidence.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.P1ProofUnchangedAndSupportSemantic", [](TestContext& test) {
			auto a = StaticInput(1); auto b = a; b.semantic.staticSupport = {};
			test.Expect(IsShadowCasterStampReusable(b.semantic), "Ordinary whole-shadow proof does not require static support.");
			auto ma = NormalizeShadowCasterMembership(std::span(&a, 1)); auto mb = NormalizeShadowCasterMembership(std::span(&b, 1));
			test.Expect(a.semantic != b.semantic && ma.digest != mb.digest, "Exact equality and digest include captured static support.");
			for (unsigned lane = 0; lane < 6; ++lane) {
				b = a;
				switch (lane) { case 0: b.semantic.staticSupport.authoredStatic = false; break;
				case 1: b.semantic.staticSupport.knownDefaultShadowAbi = false; break;
				case 2: b.semantic.staticSupport.ownerPublicationsComplete = false; break;
				case 3: b.semantic.staticSupport.materialShadowSupported = false; break;
				case 4: b.semantic.staticSupport.geometrySupported = false; break;
				case 5: b.semantic.staticSupport.deformationAbsent = false; break; }
				mb = NormalizeShadowCasterMembership(std::span(&b, 1));
				test.Expect(a.semantic != b.semantic && ma.digest != mb.digest, "Every individual support bit participates in equality and digest.");
			}
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.UnknownDynamicKeepsKnownStatic", [](TestContext& test) {
			std::array inputs{StaticInput(3), StaticInput(1), StaticInput(2)};
			inputs[1].semantic.evidenceMask = 0; inputs[1].semantic.staticSupport = {};
			auto plan = BuildStaticShadowPartition(StaticView(), inputs);
			test.Expect(plan.valid && plan.staticMembership.reusable && plan.staticMembership.rows.size() == 2,
				"Unknown dynamic semantic proof does not block independently proven static rows.");
			test.Expect(!plan.allMembership.reusable && !plan.dynamicMembership.reusable
				&& plan.allMembership.rows.size() == 3 && plan.dynamicMembership.rows.size() == 1,
				"Unknown rows remain complete coverage, never a reusable prefix.");
			test.Expect(plan.staticSourceIndices == std::vector<std::uint32_t>({0, 2})
				&& plan.dynamicSourceIndices == std::vector<std::uint32_t>({1}), "Original draw indices survive membership sorting.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.AtomicCategoryTransition", [](TestContext& test) {
			std::array inputs{StaticInput(1), StaticInput(2)};
			auto old = BuildStaticShadowPartition(StaticView(), inputs);
			inputs[0].semantic.staticSupport.authoredStatic = false;
			auto next = BuildStaticShadowPartition(StaticView(), inputs);
			test.Expect(old.valid && next.valid && old.staticMembership.rows.size() == 2
				&& next.staticSourceIndices == std::vector<std::uint32_t>({1})
				&& next.dynamicSourceIndices == std::vector<std::uint32_t>({0})
				&& !ExactShadowCasterMembershipEqual(old.staticMembership, next.staticMembership), "One category transition atomically changes static and dynamic coverage.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.MalformedCoverageAtomic", [](TestContext& test) {
			for (unsigned fault = 0; fault < 6; ++fault) {
				std::array inputs{StaticInput(1), StaticInput(2)};
				auto& row = inputs[1].semantic;
				if (fault == 0) inputs[1] = inputs[0];
				if (fault == 1) row.objectId = {};
				if (fault == 2) row.worldPosition.offsetMetres[0] = 32768;
				if (fault == 3) row.rotation[0] = std::numeric_limits<double>::quiet_NaN();
				if (fault == 4) row.firstMeshlet = std::numeric_limits<std::uint64_t>::max();
				if (fault == 5) row.payloadByteOffset = std::numeric_limits<std::uint64_t>::max(), row.payloadByteSize = 2;
				auto plan = BuildStaticShadowPartition(StaticView(), inputs);
				test.Expect(!plan.valid && plan.staticSourceIndices.empty() && plan.dynamicSourceIndices.empty()
					&& plan.allSourceIndices == std::vector<std::uint32_t>({0, 1}), "Malformed coverage publishes no partial optimized partition and retains all originals.");
			}
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.EmptyStatic", [](TestContext& test) {
			auto input = StaticInput(1); input.semantic.staticSupport.authoredStatic = false;
			auto plan = BuildStaticShadowPartition(StaticView(), std::span(&input, 1));
			test.Expect(plan.valid && plan.staticSourceIndices.empty() && plan.staticMembership.reusable
				&& plan.dynamicSourceIndices.size() == 1, "Empty static coverage leaves the complete dynamic-only path.");
			plan = BuildStaticShadowPartition(StaticView(), {});
			test.Expect(plan.valid && plan.allSourceIndices.empty(), "Empty selected coverage is structurally valid.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.ExactRecordedCandidate", [](TestContext& test) {
			ShadowCachePublicationLedger ledger; auto input = StaticInput(1);
			ShadowRefreshCandidate candidate; candidate.view = StaticView(); candidate.membership = NormalizeShadowCasterMembership(std::span(&input, 1));
			candidate.contentToken = 7; candidate.frameSerial = 8; candidate.commandSerial = 9;
			test.Expect(ledger.Record(candidate) && ledger.HasRecordedCandidate(candidate.view, 7, 9), "Only the exact recorded same-command candidate qualifies.");
			test.Expect(!ledger.HasRecordedCandidate(StaticView(2), 7, 9) && !ledger.HasRecordedCandidate(candidate.view, 8, 9)
				&& !ledger.HasRecordedCandidate(candidate.view, 7, 10), "Other keys, tokens or commands cannot borrow the pending write.");
			ledger.Discard(9); test.Expect(!ledger.HasRecordedCandidate(candidate.view, 7, 9), "Discard revokes the exact candidate.");
			candidate.commandSerial = 10; candidate.contentToken = 11; test.Expect(ledger.Record(candidate), "A fresh exact candidate can record.");
			auto later = candidate; later.commandSerial = 12; later.contentToken = 13;
			test.Expect(ledger.Record(later) && !ledger.HasRecordedCandidate(candidate.view, 11, 10)
				&& ledger.HasRecordedCandidate(later.view, 13, 12), "Later physical recording prevents copying an older pending writer.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.PriorityAndStableViewOrder", [](TestContext& test) {
			std::array requests{Request(3, 10, 1), Request(2, 10, 2), Request(1, 10, 1)};
			auto plan = BuildStaticShadowAdmissionPlan(20, 20, requests, {});
			test.Expect(plan.valid && plan.admittedRequestIndices == std::vector<std::uint32_t>({1, 2})
				&& plan.deferredRequestIndices == std::vector<std::uint32_t>({0}), "Finite retained priority then stable light identity defines deterministic admission.");
			requests[0] = Request(1, 10, 1); requests[0].view.viewId = 2; requests[1] = Request(1, 10, 1); requests[1].view.viewId = 1;
			plan = BuildStaticShadowAdmissionPlan(10, 10, std::span(requests.data(), 2), {});
			test.Expect(plan.valid && plan.admittedRequestIndices == std::vector<std::uint32_t>({1}), "Qualified views with one light have a stable complete key order.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.QueryAuxAndRetiringCharges", [](TestContext& test) {
			auto request = Request(1, 30); request.queriedAuxBytes = 10;
			auto retiring = Charge(Request(2, 30), StaticShadowChargeState::Retiring);
			auto plan = BuildStaticShadowAdmissionPlan(60, 60, std::span(&request, 1), std::span(&retiring, 1));
			test.Expect(plan.valid && plan.retiringBytes == 30 && plan.chargedBytes == 30
				&& plan.admittedRequestIndices.empty(), "Retiring actual bytes and queried auxiliary bytes remain charged.");
			plan = BuildStaticShadowAdmissionPlan(70, 70, std::span(&request, 1), std::span(&retiring, 1));
			test.Expect(plan.valid && plan.newReservationBytes == 40 && plan.admittedRequestIndices.size() == 1, "Exact query reservation fits inclusive available budget.");
			std::array charges{Charge(Request(3, 10), StaticShadowChargeState::Reserved),
				Charge(Request(4, 20)), retiring};
			plan = BuildStaticShadowAdmissionPlan(100, 100, std::span(&request, 1), charges);
			test.Expect(plan.valid && plan.reservedBytes == 10 && plan.residentBytes == 20
				&& plan.retiringBytes == 30 && plan.chargedBytes == 60 && plan.newReservationBytes == 40,
				"Queried reservations, actual residents and retiring allocations all count independently.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.LoweringDrainsWithoutAdmission", [](TestContext& test) {
			auto request = Request(1, 40); auto charge = Charge(request);
			auto plan = BuildStaticShadowAdmissionPlan(20, 64, std::span(&request, 1), std::span(&charge, 1));
			test.Expect(plan.valid && plan.draining && plan.effectiveBytes == 64 && plan.chargedBytes == 40
				&& plan.admittedRequestIndices.empty() && plan.retireChargeIndices == std::vector<std::uint32_t>({0}), "Lowered request retains the physical charge and stops all admissions while draining.");
			plan = BuildStaticShadowAdmissionPlan(20, 20, {}, {});
			test.Expect(plan.valid && !plan.draining && plan.effectiveBytes == 20, "After physical release the requested bound becomes effective.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.ZeroAndDefaultBudget", [](TestContext& test) {
			auto request = Request(1, 1); auto plan = BuildStaticShadowAdmissionPlan(0, kStaticShadowDefaultBudgetBytes, std::span(&request, 1), {});
			test.Expect(plan.valid && plan.admittedRequestIndices.empty() && plan.effectiveBytes == 0, "Explicit zero admits no static allocation.");
			request.queriedDepthBytes = kStaticShadowDefaultBudgetBytes;
			plan = BuildStaticShadowAdmissionPlan(kStaticShadowDefaultBudgetBytes, kStaticShadowDefaultBudgetBytes, std::span(&request, 1), {});
			test.Expect(plan.valid && plan.admittedRequestIndices.size() == 1, "The 64 MiB default accepts an exact-bound whole allocation.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.CubeAtomicReservation", [](TestContext& test) {
			auto request = Request(1, 60); request.view.resourceKind = static_cast<ShadowResourceKind>(1);
			request.view.lightProjectionStamp.cubeProjectionEvidenceComplete = true;
			for (auto& matrix : request.view.lightProjectionStamp.cubeViewProjections) matrix[0] = 1;
			request.requiredFaceMask = 0x3f;
			auto plan = BuildStaticShadowAdmissionPlan(59, 59, std::span(&request, 1), {});
			test.Expect(plan.valid && plan.admittedRequestIndices.empty(), "Six-layer allocation never partially fits five faces.");
			request.requiredFaceMask = 1; plan = BuildStaticShadowAdmissionPlan(100, 100, std::span(&request, 1), {});
			test.Expect(!plan.valid, "Incomplete cube mask cannot reserve a physical owner.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.InvalidBudgetInputs", [](TestContext& test) {
			auto request = Request(1, 1);
			request.retainedLightPriority = std::numeric_limits<double>::infinity();
			test.Expect(!BuildStaticShadowAdmissionPlan(10, 10, std::span(&request, 1), {}).valid, "Nonfinite priority is rejected rather than normalized.");
			request = Request(1, std::numeric_limits<std::uint64_t>::max()); request.queriedAuxBytes = 1;
			test.Expect(!BuildStaticShadowAdmissionPlan(10, 10, std::span(&request, 1), {}).valid, "Overflowed query sums cannot admit.");
			std::array requests{Request(1, 1), Request(1, 1)};
			test.Expect(!BuildStaticShadowAdmissionPlan(10, 10, requests, {}).valid, "Duplicate exact requested owner is rejected.");
			auto charge = Charge(Request(1, 20));
			test.Expect(!BuildStaticShadowAdmissionPlan(0, 19, {}, std::span(&charge, 1)).valid,
				"A too-small previous effective bound cannot hide a retained physical charge.");
			std::array duplicateCharges{charge, charge};
			test.Expect(!BuildStaticShadowAdmissionPlan(100, 100, {}, duplicateCharges).valid, "Duplicated physical owners cannot double-count a reservation.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.ActualReconciliation", [](TestContext& test) {
			auto request = Request(1, 30); request.queriedAuxBytes = 10; std::string reason;
			test.Expect(ReconcileStaticShadowAllocation(request, 30, 10, 50, 50, reason), "Actual VMA and auxiliary bytes fit their reservation and effective inclusive bound.");
			test.Expect(!ReconcileStaticShadowAllocation(request, 31, 10, 50, 50, reason) && !reason.empty(), "Oversized actual allocation never publishes.");
			test.Expect(!ReconcileStaticShadowAllocation(request, 30, 10, 51, 50, reason), "Already over-bound physical charges cannot publish a new owner.");
		});
		registry.Add("Renderer.Efficiency.StaticShadowPolicy.CurrentViewRetention", [](TestContext& test) {
			auto request = Request(1, 20); std::array charges{Charge(request), Charge(Request(2, 20))};
			auto plan = BuildStaticShadowAdmissionPlan(60, 60, std::span(&request, 1), charges);
			test.Expect(plan.valid && plan.retainedChargeIndices == std::vector<std::uint32_t>({0})
				&& plan.retireChargeIndices == std::vector<std::uint32_t>({1}) && plan.admittedRequestIndices.empty(), "Only exact currently requested owners remain resident; stale projections retire without freeing bytes.");
			request.view.lightProjectionStamp.semanticRevision++;
			plan = BuildStaticShadowAdmissionPlan(40, 40, std::span(&request, 1), std::span(charges.data(), 1));
			test.Expect(plan.valid && plan.retireChargeIndices.size() == 1 && plan.newReservationBytes == 20, "Projection replacement charges old and new owners until release.");
		});
	}
}
