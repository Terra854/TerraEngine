// Build/install this target, then run its watched PowerShell wrapper. The actual
// publication implementation is linked directly, never approximated by a fake.
#include "Renderer/Graph/RenderFramePublication.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <string_view>
#include <utility>

namespace
{
	// Only the invoking thread and exact armed operation count allocations.
	thread_local bool armed = false;
	thread_local unsigned attempts = 0;
	thread_local unsigned failures = 0;
	void* Allocate(std::size_t size)
	{
		if(armed) { ++attempts; ++failures; throw std::bad_alloc{}; }
		if(void* memory = std::malloc(size ? size : 1u)) return memory;
		throw std::bad_alloc{};
	}
	void Arm() noexcept { attempts = 0; failures = 0; armed = true; }
	void Disarm() noexcept { armed = false; }
	void Require(bool condition, const char* message)
	{
		if(!condition) { Disarm(); std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
	}
}
void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
	using namespace TerraEngine::Renderer;
	// Empty immutable topology is a legitimate no-draw frame. The optional
	// reference frame isolates the first required duplicate-set allocation.
	std::shared_ptr<RenderFramePublication> Build(std::uint64_t previous, bool scratch = false)
	{
		auto value = std::make_shared<RenderFramePublication>();
		value->frameId = previous + 1; value->generation = previous + 1;
		value->expectedPreviousGeneration = previous; value->rendererSessionGeneration = 1;
		value->persistent = std::make_shared<RenderPersistentTopologyRoot>(RenderPersistentTopologyRoot{1, 1, {}});
		value->dynamic = std::make_shared<RenderDynamicStateRoot>(RenderDynamicStateRoot{1, 1, {}});
		auto backend = std::make_shared<RenderBackendWorkPublication>();
		backend->frameId = value->frameId; backend->valid = true;
		backend->animationWork.frameId = value->frameId; backend->animationWork.valid = true;
		value->backendWork = std::move(backend);
		if(scratch) {
			RenderReferenceFrame frame{};
			frame.frameId = value->frameId; frame.viewId = 1; frame.outputId = 1;
			frame.originGeneration = frame.spatialGeneration = frame.residencyGeneration = 1;
			value->referenceFrames.push_back(frame);
		}
		return value;
	}
	// Support the baseline string receipt and the fixed inline receipt: the red
	// checkpoint must compile and fail for allocation, not an ABI migration.
	template<class Receipt> const char* Text(const Receipt& receipt) noexcept
	{
		if constexpr(requires { receipt.DiagnosticText(); }) return receipt.DiagnosticText();
		else return receipt.diagnostic.c_str();
	}
	template<class Receipt> void SetText(Receipt& receipt, const char* message)
	{
		if constexpr(requires { receipt.SetDiagnostic(message); }) receipt.SetDiagnostic(message);
		else receipt.diagnostic = message;
	}
	void Emit(const char* lane, unsigned iteration, std::uint64_t before,
		std::uint64_t after, RenderPublicationOutcome outcome, bool healthy = true)
	{
		std::printf("{\"lane\":\"%s\",\"iteration\":%u,\"attempts\":%u,\"failedAllocations\":%u,\"rootBefore\":%llu,\"rootAfter\":%llu,\"outcome\":%u,\"healthyFollowup\":%s}\n",
			lane, iteration, attempts, failures, static_cast<unsigned long long>(before),
			static_cast<unsigned long long>(after), static_cast<unsigned>(outcome), healthy ? "true" : "false");
	}
	void NoAllocation(unsigned count)
	{
		Require(count == 0 && failures == 0 && !armed, "A no-allocation checkpoint attempted allocation.");
	}
}
int main(int argc, char** argv)
{
	std::set_terminate([] {
		std::fprintf(stderr, "publication_terminated_during_allocation_failure=1 attempts=%u failedAllocations=%u armed=%u\n", attempts, failures, armed ? 1u : 0u);
		std::_Exit(86);
	});
	unsigned repetitions = 100;
	if(argc > 1) { const int parsed = std::atoi(argv[1]); Require(parsed > 0 && parsed <= 10000, "Invalid repetitions."); repetitions = static_cast<unsigned>(parsed); }
	// Start with a baseline-compatible invalid rejection. Old noexcept string
	// assignment terminates here and gives the parent a meaningful red receipt.
	for(unsigned i = 0; i < repetitions; ++i) {
		auto invalid = Build(0); invalid->rendererSessionGeneration = 2;
		RenderFramePublicationStore rejectedStore;
		Arm(); const auto rejected = ValidateRenderFramePublication(*invalid, 1); Disarm();
		NoAllocation(attempts); Require(rejected.outcome == RenderPublicationOutcome::RejectedSession && rejected.publicationGeneration == 1, "Invalid rejection lost identity.");
		Require(rejectedStore.Load() == nullptr && rejectedStore.TryPublish(
			SealRenderFramePublication(Build(0), 1), 1).IsSuccess(), "Invalid-candidate recovery failed.");
		Emit("rejection", i, 0, 0, rejected.outcome);
		RenderFramePublicationStore store;
		const auto baseline = Build(0); const auto baselineSeal = SealRenderFramePublication(baseline, 1);
		Require(store.TryPublish(baselineSeal, 1).IsSuccess(), "Baseline is invalid.");
		auto scratch = Build(1, true);
		Require(ValidateRenderFramePublication(*scratch, 1).IsSuccess(), "Scratch candidate is not independently valid.");
		Arm(); const auto failed = SealRenderFramePublication(scratch, 1); Disarm();
		const unsigned failedAttempts = attempts, failedCount = failures;
		Require(failed.Receipt().outcome == RenderPublicationOutcome::RejectedAllocation && failedAttempts > 0 && failedCount > 0 && store.Load() == baseline, "Scratch failure did not preserve root.");
		const auto recovered = store.TryPublish(SealRenderFramePublication(scratch, 1), 1);
		Require(recovered.IsSuccess() && store.Load() == scratch, "Healthy recovery failed.");
		attempts = failedAttempts; failures = failedCount; Emit("scratch", i, 1, 1, failed.Receipt().outcome);
		// Both success and rejection receipts own long text before faults are armed.
		for(bool success : {false, true}) {
			auto source = success ? recovered : rejected;
			const auto token = SealRenderFramePublication(success ? scratch : invalid, 1);
			SetText(source, "A deliberately long receipt diagnostic verifies owned copy and move storage under disabled allocation.");
			Arm(); const auto tokenReceipt = token.Receipt();
			auto copy = source; RenderPublicationReceipt assigned; assigned = source;
			auto moved = std::move(copy); assigned = std::move(moved); Disarm();
			NoAllocation(attempts); Require(assigned.outcome == source.outcome && std::string_view(Text(assigned)) == Text(source), "Receipt copy/move changed text or outcome.");
			Require(tokenReceipt.IsSuccess() == success, "Sealed Receipt() changed its outcome.");
			Emit(success ? "receipt-success" : "receipt-rejection", i, 2, 2, assigned.outcome);
		}
		for(unsigned kind = 0; kind < 3; ++kind) {
			std::shared_ptr<const RenderFramePublication> input = kind == 0 ? nullptr : kind == 1 ? invalid : Build(2);
			RenderFramePublicationStore sealRecovery;
			Arm(); const auto sealed = SealRenderFramePublication(input, 1); Disarm(); NoAllocation(attempts);
			Require(sealed.IsValid() == (kind == 2), "Seal outcome mismatch.");
			if(kind != 2) Require(sealRecovery.Load() == nullptr && sealRecovery.TryPublish(
				SealRenderFramePublication(Build(0), 1), 1).IsSuccess(), "Rejected-seal recovery failed.");
			Emit(kind == 0 ? "seal-null" : kind == 1 ? "seal-rejected" : "seal-valid", i, 2, 2, sealed.Receipt().outcome);
		}
		for(unsigned kind = 0; kind < 3; ++kind) {
			const auto candidate = Build(kind == 2 ? 1 : 2);
			const auto sealed = SealRenderFramePublication(candidate, 1);
			Require(sealed.IsValid(), "Exchange candidate was not prevalidated.");
			const auto before = store.Load();
			Arm(); const auto receipt = store.TryPublish(sealed, kind == 1 ? 2 : 1); Disarm(); NoAllocation(attempts);
			Require(receipt.IsSuccess() == (kind == 0), "Exchange result disagrees with lane.");
			Require(kind == 0 ? store.Load() == candidate : store.Load() == before, "Exchange root disagrees with result.");
			const auto generationAfterAttempt = store.Load()->generation;
			if(kind != 0) Require(store.TryPublish(SealRenderFramePublication(
				Build(generationAfterAttempt), 1), 1).IsSuccess(), "Rejected-exchange recovery failed.");
			Emit(kind == 0 ? "publish-success" : kind == 1 ? "publish-session" : "publish-stale", i, before->generation, generationAfterAttempt, receipt.outcome);
			// Restore the same generation for independently repeatable exchange lanes.
			store.ResetForSession(1); Require(store.TryPublish(SealRenderFramePublication(Build(0), 1), 1).IsSuccess(), "Reset baseline failed."); Require(store.TryPublish(SealRenderFramePublication(Build(1), 1), 1).IsSuccess(), "Reset followup failed.");
		}
	}
	std::printf("allocation safety passed: repetitions=%u checkpoints=%u normal_exit=1\n", repetitions, repetitions * 10);
	return 0;
}
