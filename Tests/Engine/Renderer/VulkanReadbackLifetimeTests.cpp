#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanReadbackLifecycle.h"

#include <array>
#include <new>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Renderer::Vulkan;
		bool rejectAdmissionConstruction = false;
		struct AdmissionRecord
		{
			ReadbackLifecycle lifecycle;
			bool deliveryReady = false;
			bool sceneView = false;
			bool dedicatedTarget = false;
			AdmissionRecord() { if (rejectAdmissionConstruction) throw std::bad_alloc(); }
		};

		// Count the four per-operation owners separately. The command pool and
		// source image belong to the context; this operation retains dependencies
		// on them, but must never destroy either context-owned native object.
		struct CountedAdapter final : ReadbackLifecycleAdapter
		{
			ReadbackFenceObservation fence = ReadbackFenceObservation::NotReady;
			ReadbackTerminalReason terminal = ReadbackTerminalReason::Completed;
			std::array<unsigned, 4> destroyed{};
			unsigned queries = 0;
			unsigned deliveries = 0;
			unsigned commandPoolDependencyReleases = 0;
			unsigned sourceDependencyReleases = 0;
			unsigned payloadConversions = 0;
			unsigned publicationFailures = 0;
			bool sourceReplaced = false;
			bool mapFailed = false;
			bool encodeFailed = false;
			bool payloadSucceeded = false;
			ReadbackFenceObservation QueryFence() override { ++queries; return fence; }
			void PublishTerminal(ReadbackTerminalReason reason) override
			{
				// Delivery allocation failure is an adapter failure. The controller
				// cannot mark terminal before this call commits successfully.
				if (publicationFailures != 0) { --publicationFailures; throw std::bad_alloc(); }
				if (reason == ReadbackTerminalReason::Completed) {
					++payloadConversions;
					payloadSucceeded = !sourceReplaced && !mapFailed && !encodeFailed;
				}
				terminal = reason;
				++deliveries;
			}
			void RetireNative() noexcept override
			{
				for (auto& count : destroyed) ++count;
				++commandPoolDependencyReleases;
				++sourceDependencyReleases;
			}
		};

		void ExpectOwners(TestContext& context, const CountedAdapter& adapter, unsigned count)
		{
			constexpr std::array<const char*, 4> names = { "staging buffer", "allocation", "command buffer", "fence" };
			for (std::size_t index = 0; index != names.size(); ++index)
				context.ExpectEqual(count, adapter.destroyed[index], std::string(names[index]) + " retirement count");
			context.ExpectEqual(count, adapter.commandPoolDependencyReleases, "Command-pool dependency retirement count");
			context.ExpectEqual(count, adapter.sourceDependencyReleases, "Source-image blocker retirement count");
		}
	}

	void RegisterVulkanReadbackLifetimeTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ReadbackLifetime.IdentityFourSelectorsBackpressureAndReuse", [](TestContext& context) {
			ReadbackOperationSlots<AdmissionRecord, kReadbackOperationCapacity> slots;
			unsigned acceptedSubmissions = 0;
			// Scene/game crossed with ordinary/dedicated are the four supported
			// selectors. Capacity admits all simultaneously; a fifth request must
			// reserve storage before its adapter could enter the native queue.
			for (bool scene : { false, true }) for (bool dedicated : { false, true }) {
				auto& operation = slots.emplace_back();
				operation.lifecycle = ReadbackLifecycle(ReadbackKind::VisibilityIdentity);
				operation.sceneView = scene;
				operation.dedicatedTarget = dedicated;
				operation.lifecycle.AcceptSubmission();
				++acceptedSubmissions;
			}
			bool rejectedBeforeSubmit = false;
			try { slots.emplace_back(); ++acceptedSubmissions; }
			catch (const std::length_error&) { rejectedBeforeSubmit = true; }
			context.Expect(rejectedBeforeSubmit, "Fifth identity request must hit bounded backpressure before submission.");
			context.ExpectEqual(4u, acceptedSubmissions, "All four selectors should fit, without accepting the fifth request.");
			for (bool scene : { false, true }) for (bool dedicated : { false, true }) {
				unsigned matches = 0;
				for (const auto& operation : slots)
					if (operation.sceneView == scene && operation.dedicatedTarget == dedicated) ++matches;
				context.ExpectEqual(1u, matches, "Each identity selector keeps its own admitted slot.");
			}
			for (auto iterator = slots.begin(); iterator != slots.end();) {
				CountedAdapter adapter;
				adapter.fence = ReadbackFenceObservation::Completed;
				iterator->lifecycle.Poll(adapter, false);
				ExpectOwners(context, adapter, 1);
				iterator = slots.erase(iterator); // Completed delivery has been drained.
			}
			auto& retry = slots.emplace_back();
			retry.lifecycle.AcceptSubmission();
			++acceptedSubmissions;
			context.ExpectEqual(5u, acceptedSubmissions, "A later identity admission can reuse a drained slot.");
			context.Expect(retry.lifecycle.BlocksSourceImage(), "Reused ownership must retain its new source dependency.");
		});
		registry.Add("Renderer.ReadbackLifetime.ReservationFailurePrecedesSubmission", [](TestContext& context) {
			ReadbackOperationSlots<AdmissionRecord, 4> slots;
			AdmissionRecord* prepared = nullptr;
			CountedAdapter adapter;
			unsigned acceptedSubmissions = 0;
			rejectAdmissionConstruction = true;
			bool allocationRejected = false;
			try {
				ScopedReadbackAdmission admission([&]() noexcept {
					adapter.RetireNative();
					if (prepared) slots.erase(prepared);
				});
				prepared = &slots.emplace_back();
				++acceptedSubmissions;
				admission.Accept(prepared->lifecycle);
			} catch (const std::bad_alloc&) { allocationRejected = true; }
			rejectAdmissionConstruction = false;
			context.Expect(allocationRejected, "Production slot construction must expose metadata allocation failure before queue submission.");
			context.ExpectEqual(0u, acceptedSubmissions, "Failed reservation cannot accept native work.");
			ExpectOwners(context, adapter, 1);
			context.ExpectEqual(std::size_t{0}, slots.size(), "Failed construction cannot consume the bounded slot.");
			context.Expect(slots.emplace_back().lifecycle.NativeState() == ReadbackNativeState::Prepared, "Healthy admission can retry the same slot.");
		});
		for (const auto kind : { ReadbackKind::Capture, ReadbackKind::VisibilityIdentity }) {
			const std::string suffix = kind == ReadbackKind::Capture ? "Capture" : "Identity";
			for (bool metadataThrows : { false, true }) {
				const std::string failure = metadataThrows ? "MetadataFailure" : "RejectedSubmit";
				registry.Add("Renderer.ReadbackLifetime.ScopedAdmissionRollback." + suffix + "." + failure,
					[kind, metadataThrows](TestContext& context) {
					ReadbackOperationSlots<AdmissionRecord, kReadbackOperationCapacity> slots;
					AdmissionRecord* prepared = nullptr;
					CountedAdapter adapter;
					unsigned submitAttempts = 0;
					bool metadataRejected = false;
					try {
						// This is the same scoped production algorithm as both native
						// paths: reserve, fallible metadata, submit, noexcept accept.
						ScopedReadbackAdmission admission([&]() noexcept {
							adapter.RetireNative();
							if (prepared) slots.erase(prepared);
						});
						prepared = &slots.emplace_back();
						prepared->lifecycle = ReadbackLifecycle(kind);
						if (metadataThrows) throw std::bad_alloc();
						++submitAttempts; // Rejected native submit; never call Accept.
					} catch (const std::bad_alloc&) { metadataRejected = true; }
					context.ExpectEqual(metadataThrows, metadataRejected, "Fallible metadata follows the exception rollback path.");
					context.ExpectEqual(metadataThrows ? 0u : 1u, submitAttempts, "Metadata failure must precede native submission.");
					ExpectOwners(context, adapter, 1);
					context.ExpectEqual(std::size_t{0}, slots.size(), "Rejected admission releases its reserved delivery/owner slot.");
					prepared = nullptr;
					{
						ScopedReadbackAdmission admission([&]() noexcept {
							adapter.RetireNative();
							if (prepared) slots.erase(prepared);
						});
						prepared = &slots.emplace_back();
						prepared->lifecycle = ReadbackLifecycle(kind);
						++submitAttempts;
						admission.Accept(prepared->lifecycle);
					}
					ExpectOwners(context, adapter, 1);
					context.Expect(prepared->lifecycle.BlocksSourceImage(), "Accepted retry survives guard destruction with all native dependencies.");
					adapter.fence = ReadbackFenceObservation::Completed;
					prepared->lifecycle.Poll(adapter, false);
					ExpectOwners(context, adapter, 2);
					context.ExpectEqual(1u, adapter.deliveries, "Accepted retry completes once after genuine adapter completion observation.");
					slots.erase(prepared);
				});
			}
		}
		registry.Add("Renderer.ReadbackLifetime.AcceptedTransferUsesStableReservedStorage", [](TestContext& context) {
			static_assert(noexcept(std::declval<ReadbackLifecycle&>().AcceptSubmission()));
			static_assert(noexcept(std::declval<ReadbackLifecycleAdapter&>().RetireNative()));
			ReadbackOperationSlots<AdmissionRecord, 4> slots;
			auto* first = &slots.emplace_back();
			first->lifecycle.AcceptSubmission();
			for (unsigned index = 1; index != 4; ++index) slots.emplace_back().lifecycle.AcceptSubmission();
			bool fullRejected = false;
			try { slots.emplace_back(); } catch (const std::length_error&) { fullRejected = true; }
			context.Expect(fullRejected, "Bounded capture/identity owner storage must reject excess accepted-work reservations.");
			context.Expect(&*slots.begin() == first, "Other admissions cannot relocate an accepted operation.");
			CountedAdapter adapter;
			first->lifecycle.Poll(adapter, true);
			ExpectOwners(context, adapter, 0);
			context.Expect(first->lifecycle.BlocksSourceImage(), "Exhausting bookkeeping cannot lose previously accepted native ownership.");
		});
		registry.Add("Renderer.ReadbackLifetime.RetiredDeliveryKeepsReservedSlotUntilDrain", [](TestContext& context) {
			ReadbackOperationSlots<AdmissionRecord, 4> slots;
			auto* operation = &slots.emplace_back();
			CountedAdapter adapter;
			operation->lifecycle.AcceptSubmission();
			adapter.fence = ReadbackFenceObservation::Completed;
			operation->lifecycle.Poll(adapter, false);
			operation->deliveryReady = true;
			ExpectOwners(context, adapter, 1);
			context.ExpectEqual(std::size_t{1}, slots.size(), "Native retirement must not discard CPU terminal delivery storage.");
			context.Expect(!operation->lifecycle.BlocksSourceImage(), "Retired delivery storage must not block old images.");
			operation->deliveryReady = false;
			slots.erase(operation);
			context.ExpectEqual(std::size_t{0}, slots.size(), "The caller can release bookkeeping after draining the result.");
		});
		registry.Add("Renderer.ReadbackLifetime.BeforeDeadlineRetainsAllOwners", [](TestContext& context) {
			ReadbackLifecycle operation;
			CountedAdapter adapter;
			operation.AcceptSubmission();
			operation.Poll(adapter, false);
			ExpectOwners(context, adapter, 0);
			context.Expect(operation.BlocksSourceImage(), "Incomplete accepted copy must block old-image destruction.");
			context.ExpectEqual(0u, adapter.deliveries, "Before-deadline copy must not emit terminal delivery.");
		});
		registry.Add("Renderer.ReadbackLifetime.TimeoutThenCompletion", [](TestContext& context) {
			ReadbackLifecycle operation;
			CountedAdapter adapter;
			operation.AcceptSubmission();
			operation.Poll(adapter, true);
			operation.Poll(adapter, true);
			ExpectOwners(context, adapter, 0);
			context.Expect(operation.BlocksSourceImage(), "Timed-out copy must retain its exact source-image blocker.");
			context.ExpectEqual(1u, adapter.deliveries, "Timeout delivery must be exactly once.");
			adapter.fence = ReadbackFenceObservation::Completed;
			operation.Poll(adapter, true);
			operation.Poll(adapter, true);
			ExpectOwners(context, adapter, 1);
			context.ExpectEqual(1u, adapter.deliveries, "Late completion must not publish another result.");
			context.ExpectEqual(0u, adapter.payloadConversions, "Late completion after timeout must not encode discarded pixels.");
		});
		for (const auto kind : { ReadbackKind::Capture, ReadbackKind::VisibilityIdentity }) {
			const std::string suffix = kind == ReadbackKind::Capture ? "Capture" : "Identity";
			registry.Add("Renderer.ReadbackLifetime.QueryErrorRetainsOwners." + suffix, [kind](TestContext& context) {
				ReadbackLifecycle operation(kind);
				CountedAdapter adapter;
				operation.AcceptSubmission();
				adapter.fence = ReadbackFenceObservation::QueryError;
				operation.Poll(adapter, false);
				ExpectOwners(context, adapter, 0);
				context.Expect(operation.BlocksSourceImage(), "Ordinary query error cannot prove that accepted GPU work released the image.");
				operation.Poll(adapter, false);
				context.ExpectEqual(1u, adapter.deliveries, "Repeated query failure must not duplicate terminal results.");
				adapter.fence = ReadbackFenceObservation::Completed;
				operation.Poll(adapter, false);
				ExpectOwners(context, adapter, 1);
				context.ExpectEqual(1u, adapter.deliveries, "Completion after query failure only settles native ownership.");
			});
			registry.Add("Renderer.ReadbackLifetime.DeviceLossRetiresExactlyOnce." + suffix, [kind](TestContext& context) {
				ReadbackLifecycle operation(kind);
				CountedAdapter adapter;
				operation.AcceptSubmission();
				adapter.fence = ReadbackFenceObservation::DeviceLost;
				operation.Poll(adapter, true);
				operation.Cancel(adapter, ReadbackRetirementProof::DeviceLost, true);
				operation.Poll(adapter, true);
				ExpectOwners(context, adapter, 1);
				context.ExpectEqual(1u, adapter.deliveries, "Device loss must publish exactly once.");
				context.Expect(!operation.BlocksSourceImage(), "Terminal device-loss proof releases the blocker.");
			});
			registry.Add("Renderer.ReadbackLifetime.NoProofCancelThenComplete." + suffix, [kind](TestContext& context) {
				ReadbackLifecycle operation(kind);
				CountedAdapter adapter;
				operation.AcceptSubmission();
				operation.Cancel(adapter, ReadbackRetirementProof::None, true);
				operation.Cancel(adapter, ReadbackRetirementProof::None, true);
				ExpectOwners(context, adapter, 0);
				context.Expect(operation.BlocksSourceImage(), "Client cancellation cannot release an accepted native copy.");
				adapter.fence = ReadbackFenceObservation::Completed;
				operation.Poll(adapter, true);
				ExpectOwners(context, adapter, 1);
				context.ExpectEqual(1u, adapter.deliveries, "Canceled operation must not deliver again after completion.");
			});
		}
		registry.Add("Renderer.ReadbackLifetime.FailedIdleRetainsInfrastructureDependencies", [](TestContext& context) {
			ReadbackLifecycle operation;
			CountedAdapter adapter;
			operation.AcceptSubmission();
			operation.Cancel(adapter, ReadbackRetirementProof::None, false);
			ExpectOwners(context, adapter, 0);
			context.Expect(operation.BlocksSourceImage(), "Failed idle must retain allocator, pool and source-image dependencies.");
			context.ExpectEqual(0u, adapter.deliveries, "Failed idle cannot fabricate client completion.");
			operation.Cancel(adapter, ReadbackRetirementProof::DeviceIdle, true);
			operation.Cancel(adapter, ReadbackRetirementProof::DeviceIdle, true);
			ExpectOwners(context, adapter, 1);
			context.ExpectEqual(1u, adapter.deliveries, "Proven-idle shutdown publishes pending cancellation once.");
		});
		registry.Add("Renderer.ReadbackLifetime.RejectedSubmissionReleasesOnlyPreparedOwners", [](TestContext& context) {
			ReadbackLifecycle operation;
			CountedAdapter adapter;
			operation.RejectSubmission(adapter);
			operation.RejectSubmission(adapter);
			operation.Poll(adapter, false);
			ExpectOwners(context, adapter, 1);
			context.ExpectEqual(0u, adapter.queries, "Rejected work must never query an unsubmitted fence.");
			context.Expect(!operation.BlocksSourceImage(), "Rejected submission has no live source dependency.");
		});
		for (unsigned payloadCase = 0; payloadCase != 4; ++payloadCase) {
			constexpr std::array<const char*, 4> names = { "Healthy", "SourceReplaced", "MapFailure", "EncodeFailure" };
			registry.Add(std::string("Renderer.ReadbackLifetime.CompletedPayload.") + names[payloadCase], [payloadCase](TestContext& context) {
				ReadbackLifecycle operation;
				CountedAdapter adapter;
				adapter.sourceReplaced = payloadCase == 1;
				adapter.mapFailed = payloadCase == 2;
				adapter.encodeFailed = payloadCase == 3;
				operation.AcceptSubmission();
				operation.Poll(adapter, false);
				context.ExpectEqual(0u, adapter.payloadConversions, "Incomplete fence must not map or encode payload.");
				adapter.fence = ReadbackFenceObservation::Completed;
				operation.Poll(adapter, false);
				ExpectOwners(context, adapter, 1);
				context.ExpectEqual(1u, adapter.deliveries, "Payload success or explicit failure must deliver exactly once.");
				context.ExpectEqual(payloadCase == 0, adapter.payloadSucceeded, "Payload adapter must preserve its conversion outcome.");
			});
		}
		registry.Add("Renderer.ReadbackLifetime.DeliveryFailureDoesNotMarkTerminal", [](TestContext& context) {
			ReadbackLifecycle operation;
			CountedAdapter adapter;
			operation.AcceptSubmission();
			adapter.publicationFailures = 1;
			bool rejected = false;
			try { operation.Poll(adapter, true); } catch (const std::bad_alloc&) { rejected = true; }
			context.Expect(rejected, "Delivery allocation failure must reach the protected caller.");
			context.Expect(operation.ClientState() == ReadbackClientState::Pending, "Failed delivery cannot mark the client terminal.");
			ExpectOwners(context, adapter, 0);
			operation.Poll(adapter, true);
			context.ExpectEqual(1u, adapter.deliveries, "Successful retry commits one terminal result.");
			adapter.fence = ReadbackFenceObservation::Completed;
			operation.Poll(adapter, true);
			ExpectOwners(context, adapter, 1);
		});
		registry.Add("Renderer.ReadbackLifetime.TimeoutAndCancelSerializeOneTerminal", [](TestContext& context) {
			// Production holds deviceStateMutex around both decisions; model their
			// two protected orderings instead of racing unsynchronized test objects.
			for (bool timeoutFirst : { false, true }) {
				ReadbackLifecycle operation;
				CountedAdapter adapter;
				operation.AcceptSubmission();
				if (timeoutFirst) operation.Poll(adapter, true);
				operation.Cancel(adapter, ReadbackRetirementProof::None, true);
				operation.Poll(adapter, true);
				ExpectOwners(context, adapter, 0);
				operation.Cancel(adapter, ReadbackRetirementProof::DeviceIdle, true);
				ExpectOwners(context, adapter, 1);
				context.ExpectEqual(1u, adapter.deliveries, "Protected timeout/cancel order must produce one terminal outcome.");
			}
		});
	}
}

#if defined(TERRAENGINE_READBACK_LIFETIME_STANDALONE)
// Parent-only red/green checkpoint: compile this same case file and production
// controller with Tests/Engine and Engine include roots; no Vulkan driver needed.
int main()
{
	TerraEngine::Tests::TestRegistry registry;
	TerraEngine::Tests::RegisterVulkanReadbackLifetimeTests(registry);
	return registry.RunAll(std::cout);
}
#endif
