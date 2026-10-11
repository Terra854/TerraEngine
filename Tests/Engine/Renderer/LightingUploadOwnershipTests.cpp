// LightingUploadOwnershipTests.cpp — CPU policy evidence for physical light regions.
#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanLightingUploadPlan.h"
#include <memory>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::VulkanLightingUpload;
		PhysicalOwner Owner()
		{
			PhysicalOwner owner;
			owner.allocationId = 11; owner.allocationGeneration = 2; owner.rendererSessionGeneration = 3;
			// RenderViewType's native values are Game=0 and Scene=1. The forward
			// declaration keeps this policy-only test independent of renderer state.
			owner.viewType = static_cast<RenderViewType>(1); owner.outputId = 7;
			return owner;
		}
		MappingBounds Bounds(bool coherent = false)
		{
			MappingBounds bounds;
			bounds.allocationSizeBytes = kBlockBytes * 8; bounds.mappedSizeBytes = bounds.allocationSizeBytes;
			bounds.nonCoherentAtomSize = 256; bounds.coherent = coherent;
			return bounds;
		}
		std::unique_ptr<CanonicalBlock> Payload()
		{
			auto block = std::make_unique<CanonicalBlock>(); block->activeCount = 2;
			block->records[0][0] = 0x3f800000U; block->records[1][0] = 0x40000000U;
			return block;
		}
		UploadReceipt Success(const DirtyRangePlan& plan)
		{
			UploadReceipt receipt;
			receipt.owner = plan.owner; receipt.expectedMirrorRevision = plan.expectedMirrorRevision;
			receipt.frameSlotSafe = true; receipt.ownerCurrent = true;
			receipt.mapSucceeded = true; receipt.writeSucceeded = true; receipt.flushSucceeded = true;
			return receipt;
		}
		void Seed(TestContext& context, const PhysicalOwner& owner, const CanonicalBlock& block, Mirror& mirror,
			bool coherent = false)
		{
			// Large canonical blocks/plans live on the heap to keep the Windows
			// default test stack bounded. These receipts are synthetic CPU inputs,
			// never claims that a Vulkan map, flush or fence actually completed.
			auto plan = std::make_unique<DirtyRangePlan>();
			context.Expect(BuildLightingDirtyRangePlan(owner, Bounds(coherent), block, mirror, *plan), "initial owner plan must build");
			context.Expect(CommitLightingUpload(*plan, Success(*plan), mirror), "qualified successful initialization must commit");
		}
		void ExpectFailureKeepsLastSuccess(TestContext& context, unsigned failureLane)
		{
			auto mirror = std::make_unique<Mirror>(); auto old = Payload(); const auto owner = Owner();
			Seed(context, owner, *old, *mirror);
			auto desired = std::make_unique<CanonicalBlock>(*old); desired->activeCount = 1; desired->records[0][0] = 0x40400000U;
			desired->records[1] = {};
			auto plan = std::make_unique<DirtyRangePlan>();
			context.Expect(BuildLightingDirtyRangePlan(owner, Bounds(), *desired, *mirror, *plan), "changed owner payload must plan");
			auto receipt = Success(*plan);
			if (failureLane == 0) receipt.mapSucceeded = false;
			if (failureLane == 1) receipt.writeSucceeded = false;
			if (failureLane == 2) receipt.flushSucceeded = false;
			const auto successfulRevision = mirror->revision;
			context.Expect(!CommitLightingUpload(*plan, receipt, *mirror), "failed upload cannot publish a successful mirror");
			context.Expect(mirror->owner == owner && mirror->block == *old, "failure retains exact last successful owner, bytes and count");
			context.Expect(!mirror->initialized && mirror->revision > successfulRevision, "possible partial writes revoke initialized proof");
			auto retry = std::make_unique<DirtyRangePlan>();
			context.Expect(BuildLightingDirtyRangePlan(owner, Bounds(), *desired, *mirror, *retry), "failed candidate remains retryable");
			context.Expect(retry->fullInitialization && retry->logicalBytes == kBlockBytes, "retry reinitializes possibly changed physical storage");
			context.Expect(CommitLightingUpload(*retry, Success(*retry), *mirror), "successful retry publishes candidate");
			context.Expect(mirror->initialized && mirror->block == *desired, "only retry success advances payload");
		}
	}
	void RegisterLightingUploadOwnershipTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.LightingOwnership.AllFrameSnapshotPurposeRegionsWarmIndependently", [](TestContext& context) {
			auto block = Payload();
			// Match the existing three frames, eight snapshot regions and two
			// separate buffer purposes without creating any GPU resources.
			for (std::uint32_t frame = 0; frame < 3; ++frame) for (std::uint32_t slot = 0; slot < 8; ++slot)
				for (const auto purpose : { Purpose::FinalLight, Purpose::AnalysisInput }) {
					auto mirror = std::make_unique<Mirror>(); auto owner = Owner();
					owner.frameSlot = frame; owner.snapshotSlot = slot; owner.purpose = purpose; owner.regionOffsetBytes = slot * kBlockBytes;
					Seed(context, owner, *block, *mirror);
					auto stable = std::make_unique<DirtyRangePlan>();
					context.Expect(BuildLightingDirtyRangePlan(owner, Bounds(), *block, *mirror, *stable), "warm exact region builds");
					context.Expect(!stable->fullInitialization && stable->logicalBytes == 0 && stable->physicalFlushBytes == 0,
						"stable initialized physical owner writes and flushes nothing");
				}
		});
		registry.Add("Renderer.Efficiency.LightingOwnership.EachOwnerIdentityLaneRequiresInitialization", [](TestContext& context) {
			auto block = Payload(); auto mirror = std::make_unique<Mirror>(); const auto original = Owner();
			Seed(context, original, *block, *mirror);
			for (unsigned lane = 0; lane < 9; ++lane) {
				auto owner = original;
				if (lane == 0) ++owner.allocationId;
				if (lane == 1) ++owner.allocationGeneration;
				if (lane == 2) ++owner.rendererSessionGeneration;
				if (lane == 3) ++owner.frameSlot;
				if (lane == 4) ++owner.snapshotSlot;
				if (lane == 5) owner.purpose = Purpose::AnalysisInput;
				if (lane == 6) owner.viewType = static_cast<RenderViewType>(0);
				if (lane == 7) ++owner.outputId;
				if (lane == 8) owner.regionOffsetBytes = kBlockBytes;
				auto plan = std::make_unique<DirtyRangePlan>();
				context.Expect(BuildLightingDirtyRangePlan(owner, Bounds(), *block, *mirror, *plan), "changed physical identity builds");
				context.Expect(plan->fullInitialization && plan->logicalBytes == kBlockBytes, "another owner cannot borrow prior initialized bytes");
				context.Expect(mirror->owner == original && mirror->block == *block, "planning never changes successful ownership");
			}
		});
		registry.Add("Renderer.Efficiency.LightingOwnership.SceneGameReassignmentCannotBorrowViewMirror", [](TestContext& context) {
			auto mirror = std::make_unique<Mirror>(); auto block = Payload(); auto scene = Owner();
			Seed(context, scene, *block, *mirror);
			auto game = scene; game.viewType = static_cast<RenderViewType>(0); game.outputId = 0;
			auto plan = std::make_unique<DirtyRangePlan>();
			context.Expect(BuildLightingDirtyRangePlan(game, Bounds(), *block, *mirror, *plan) && plan->fullInitialization,
				"same physical slot reassigned to Game requires complete initialization");
			context.Expect(CommitLightingUpload(*plan, Success(*plan), *mirror), "Game owner may publish after success");
			context.Expect(BuildLightingDirtyRangePlan(scene, Bounds(), *block, *mirror, *plan) && plan->fullInitialization,
				"returning Scene cannot reuse its old logical ownership");
		});
		registry.Add("Renderer.Efficiency.LightingOwnership.CoherentSuccessDoesNotRequireOrInventFlush", [](TestContext& context) {
			auto mirror = std::make_unique<Mirror>(); auto block = Payload(); auto plan = std::make_unique<DirtyRangePlan>();
			context.Expect(BuildLightingDirtyRangePlan(Owner(), Bounds(true), *block, *mirror, *plan), "coherent plan builds");
			context.Expect(plan->logicalBytes == kBlockBytes && plan->flushRanges.empty() && plan->physicalFlushBytes == 0,
				"coherent host writes count bytes without claiming a physical flush");
			auto receipt = Success(*plan); receipt.flushSucceeded = false;
			context.Expect(CommitLightingUpload(*plan, receipt, *mirror), "coherent successful writes commit without a flush operation");
			context.Expect(mirror->initialized && mirror->block == *block, "coherent success owns exact bytes");
		});
		registry.Add("Renderer.Efficiency.LightingOwnership.FailedMapRetainsSuccessfulContent", [](TestContext& context) { ExpectFailureKeepsLastSuccess(context, 0); });
		registry.Add("Renderer.Efficiency.LightingOwnership.FailedWriteRetainsSuccessfulContent", [](TestContext& context) { ExpectFailureKeepsLastSuccess(context, 1); });
		registry.Add("Renderer.Efficiency.LightingOwnership.FailedNonCoherentFlushRetainsSuccessfulContent", [](TestContext& context) { ExpectFailureKeepsLastSuccess(context, 2); });
		registry.Add("Renderer.Efficiency.LightingOwnership.UnsafeFrameSlotCannotPublish", [](TestContext& context) {
			auto mirror = std::make_unique<Mirror>(); auto block = Payload(); Seed(context, Owner(), *block, *mirror);
			auto desired = std::make_unique<CanonicalBlock>(*block); desired->records[0][0] = 0x40400000U;
			auto plan = std::make_unique<DirtyRangePlan>();
			context.Expect(BuildLightingDirtyRangePlan(Owner(), Bounds(), *desired, *mirror, *plan), "candidate planning needs no GPU wait");
			auto receipt = Success(*plan); receipt.frameSlotSafe = false;
			context.Expect(!CommitLightingUpload(*plan, receipt, *mirror), "missing existing fence/idle safety cannot authorize slot reuse");
			context.Expect(mirror->block == *block, "unsafe slot never advances successful content");
		});
		registry.Add("Renderer.Efficiency.LightingOwnership.ReceiptOwnerAndRevisionMustMatchPlan", [](TestContext& context) {
			auto block = Payload();
			for (unsigned lane = 0; lane < 3; ++lane) {
				auto mirror = std::make_unique<Mirror>(); Seed(context, Owner(), *block, *mirror);
				auto desired = std::make_unique<CanonicalBlock>(*block); desired->records[0][0] = 0x40400000U;
				auto plan = std::make_unique<DirtyRangePlan>();
				context.Expect(BuildLightingDirtyRangePlan(Owner(), Bounds(), *desired, *mirror, *plan), "candidate builds");
				auto receipt = Success(*plan);
				if (lane == 0) ++receipt.owner.allocationGeneration;
				if (lane == 1) ++receipt.expectedMirrorRevision;
				if (lane == 2) receipt.ownerCurrent = false;
				context.Expect(!CommitLightingUpload(*plan, receipt, *mirror), "wrong/currently retired receipt cannot publish candidate");
				context.Expect(mirror->block == *block, "receipt rejection preserves successful bytes");
			}
		});
		registry.Add("Renderer.Efficiency.LightingOwnership.StaleSuccessAndFailureCannotChangeNewerProof", [](TestContext& context) {
			auto mirror = std::make_unique<Mirror>(); auto block = Payload(); Seed(context, Owner(), *block, *mirror);
			auto oldPlan = std::make_unique<DirtyRangePlan>(); auto oldDesired = std::make_unique<CanonicalBlock>(*block);
			oldDesired->records[0][0] = 0x40400000U;
			context.Expect(BuildLightingDirtyRangePlan(Owner(), Bounds(), *oldDesired, *mirror, *oldPlan), "old plan builds");
			auto newer = std::make_unique<CanonicalBlock>(*block); newer->records[0][0] = 0x40800000U;
			Seed(context, Owner(), *newer, *mirror); const auto revision = mirror->revision;
			for (bool succeeded : { false, true }) {
				auto receipt = Success(*oldPlan); receipt.flushSucceeded = succeeded;
				context.Expect(!CommitLightingUpload(*oldPlan, receipt, *mirror), "stale attempt cannot commit");
				context.Expect(mirror->initialized && mirror->revision == revision && mirror->block == *newer,
					"stale failure cannot invalidate or overwrite newer successful proof");
			}
		});
		registry.Add("Renderer.Efficiency.LightingOwnership.RecoveryResetRejectsOldPlanAndReinitializes", [](TestContext& context) {
			auto mirror = std::make_unique<Mirror>(); auto block = Payload(); const auto owner = Owner();
			Seed(context, owner, *block, *mirror); auto oldPlan = std::make_unique<DirtyRangePlan>();
			context.Expect(BuildLightingDirtyRangePlan(owner, Bounds(), *block, *mirror, *oldPlan), "pre-recovery plan builds");
			const auto revision = mirror->revision; InvalidateLightingMirror(*mirror);
			context.Expect(!mirror->initialized && mirror->revision > revision && mirror->block == *block && mirror->owner == owner,
				"reset revokes safety proof while retaining last successful diagnostics");
			context.Expect(!CommitLightingUpload(*oldPlan, Success(*oldPlan), *mirror), "old-session plan cannot undo recovery reset");
			auto recovered = owner; ++recovered.rendererSessionGeneration; ++recovered.allocationGeneration;
			auto plan = std::make_unique<DirtyRangePlan>();
			context.Expect(BuildLightingDirtyRangePlan(recovered, Bounds(), *block, *mirror, *plan) && plan->fullInitialization,
				"new allocation/session starts uninitialized even for identical bytes");
			context.Expect(CommitLightingUpload(*plan, Success(*plan), *mirror) && mirror->owner == recovered,
				"only current successful replacement restores initialized proof");
		});
	}
}
