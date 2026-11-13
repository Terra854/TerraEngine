// Validation-only, fixed-capacity observations of actual primary command owners.
#pragma once
#include "Core/Export.h"
#include <array>
#include <cstdint>
// The renderer owns these enums; forward declarations keep this test seam light.
enum class RenderOutputKind : std::uint8_t;
enum class RenderViewType : std::uint8_t;
namespace TerraEngine::Tests::Review
{
	enum class PrimaryReviewPoint : std::uint8_t { Reset, AnimationRecorded, DeformationRecorded, Prefix, LightingRefusal, Admitted, Submit, Begin };
	// Numeric output IDs are local to a typed output namespace. Scene0 and
	// Game0 must remain distinct, especially at the pre-submit safety guard.
	struct PrimaryReviewViewIdentity
	{
		RenderOutputKind outputKind{};
		RenderViewType viewType{};
		std::uint32_t outputId = 0;
		bool operator==(const PrimaryReviewViewIdentity&) const noexcept = default;
	};
	struct PrimaryReviewEvent
	{
		PrimaryReviewPoint point{};
		std::uint64_t frame = 0, command = 0, fence = 0;
		PrimaryReviewViewIdentity view{};
		std::uint32_t animationDispatches = 0, deformationDispatches = 0;
		std::int32_t result = 0;
	};
	struct DiscardedPrimaryReviewHook
	{
		std::array<PrimaryReviewEvent, 128> events{};
		std::uint32_t count = 0, refusals = 0, unsafeDependentViewBlocked = 0, refusedEventIndex = 0;
		std::uint64_t refusedFrame = 0, refusedCommand = 0;
		PrimaryReviewViewIdentity refusedView{};
		bool overflow = false, armed = true;
	};
	// Install before the protected frame starts; disarm before owner teardown.
	TERRAENGINE_API bool SetDiscardedPrimaryReviewHook(DiscardedPrimaryReviewHook*) noexcept;
	TERRAENGINE_API bool ObserveDiscardedPrimaryReview(PrimaryReviewEvent) noexcept;
}
