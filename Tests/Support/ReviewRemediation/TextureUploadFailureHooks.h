// One owner-thread fault per real upload; fixed observations never fabricate
// native completion. Caller ambiguity is injected only AFTER a real fence wait.
#pragma once
#include "Core/Export.h"
#include <cstdint>
#include <array>

namespace TerraEngine::Tests::Review
{
	enum class ReviewTextureRole : std::uint8_t { None, White, Normal, Mask, Ordinary };
	enum class ReviewTextureFailure : std::uint8_t { ImageAllocation, CopyRefusal, CompletedCopyCallerAmbiguity, ImageView };
	// Baseline-only observation stop before an unchecked null native use. A
	// checked repaired upload must report its real startup failure before this
	// point; the test explicitly rejects this stop as repair success.
	struct TextureReviewObservationStop {};
	// Proof denotes the existing owner's actually qualified idle/device-loss
	// retirement boundary, not merely an injected successful return value.
	enum class ReviewUploadOwnershipPoint : std::uint8_t { Retained, RetirementProof, DestroyAttempt };
	struct ReviewUploadOwnershipEvent { ReviewUploadOwnershipPoint point{}; std::uint64_t sequence = 0; };
	struct TextureUploadFailureHook
	{
		ReviewTextureRole role = ReviewTextureRole::None;
		ReviewTextureFailure failure = ReviewTextureFailure::ImageAllocation;
		std::uint32_t remaining = 1, reached = 0;
		std::uint32_t stagingCreated = 0, stagingDestroyed = 0, stagingRetained = 0;
		std::uint32_t imageCreated = 0, publicationAttempts = 0, nullImageUseBlocked = 0;
		std::uint32_t ambiguousStagingDestroyAttempts = 0, observerOnlyStagingReclaimed = 0;
		std::array<ReviewUploadOwnershipEvent, 16> ownershipEvents{};
		std::uint32_t ownershipEventCount = 0, stagingDestroyedAfterRetirementProof = 0;
		std::uint64_t ownershipSequence = 0, retainedSequence = 0, retirementProofSequence = 0, lastDestroySequence = 0;
		bool ownershipEventsOverflowed = false;
		bool actualCopySubmitAndFenceSucceeded = false, callerAmbiguityInjected = false;
		std::uint64_t commandHandle = 0, fenceHandle = 0;
		std::uintptr_t imageHandle = 0, stagingHandle = 0;
		void* allocator = nullptr;
		void* imageAllocation = nullptr;
	};
	TERRAENGINE_API bool SetTextureUploadFailureHook(TextureUploadFailureHook* hook) noexcept;
}
