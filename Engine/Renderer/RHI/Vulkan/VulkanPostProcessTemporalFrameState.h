// Submission-owned history ring for one prepared camera output.
#pragma once

#include "VulkanPostProcessTemporal.h"
#include "VulkanPostProcessHistory.h"
#include "VulkanPostProcessMotionPreparation.h"

namespace TerraEngine::Renderer::Vulkan
{
	struct PostProcessMotionBuffers;
	struct TemporalViewFramePlan
	{
		PostProcessHistoryRecording provenance;
		std::array<TemporalHistorySlotState, 4> layouts{};
		std::optional<TemporalAcceptedHistory> previous;
		std::shared_ptr<const PostProcessMotionMeshOwners> previousMeshes;
		std::uint32_t writeSlot = 0u;
		std::uint32_t readSlot = 0u;
	};

	// Only the protected queue owner may mutate this state. Recording can
	// reserve a slot. Successful submission publishes its contents, and a
	// separate completion epoch authorizes rebinding that allocation.
	class TemporalViewFrameState
	{
	public:
		static constexpr std::uint32_t kSlotCount = 4u;
		TemporalViewFrameState();

		std::optional<TemporalViewFramePlan> Reserve(
			std::uint64_t passGeneration, std::uint64_t recordingId, std::uint32_t frameSlot,
			const PostProcessHistoryScope& scope, const PostProcessHistorySignature& signature,
			std::shared_ptr<const RenderFramePublication> publication, std::uint32_t presentationIndex,
			std::uint64_t frameId) noexcept;
		bool Record(VkCommandBuffer command, const TemporalRecordedUse& recorded,
			std::shared_ptr<const PostProcessMotionBuffers> motionBuffers) noexcept;
		bool Accept(VkCommandBuffer command) noexcept;
		void AttachCompletion(std::uint32_t frameSlot, std::uint64_t epoch) noexcept;
		void Complete(std::uint32_t frameSlot, std::uint64_t epoch) noexcept;
		void CompleteDeviceIdle() noexcept;
		void Discard(VkCommandBuffer command) noexcept;
		void CancelReservation(std::uint64_t recordingId) noexcept;

	private:
		struct Slot
		{
			TemporalHistorySlotState layout;
			std::array<std::uint64_t, 3> lastEpoch{};
		};
		struct Reservation
		{
			std::uint64_t recordingId = 0u;
			std::uint64_t frameId = 0u;
			std::uint32_t frameSlot = 0u, writeSlot = 0u, readSlot = 0u;
			bool submittedWithoutEpoch = false;
			std::uint64_t submissionEpoch = 0u;
			VkCommandBuffer command = VK_NULL_HANDLE;
			std::optional<TemporalRecordedUse> recorded;
			std::shared_ptr<const PostProcessMotionBuffers> motionBuffers;
		};
		bool Writable(std::uint32_t slot) const noexcept;

		PostProcessHistory history{3u};
		std::array<Slot, kSlotCount> slots{};
		// There can be at most three frame-slot submissions and one open
		// recording borrowing this four-slot ring at a protected boundary.
		std::array<Reservation, kSlotCount> reserved{};
		std::array<std::uint64_t, 3> completed{};
		std::optional<TemporalAcceptedHistory> accepted;
		std::shared_ptr<const PostProcessMotionMeshOwners> acceptedMeshes;
		std::uint64_t generation = 0u;
	};
}
