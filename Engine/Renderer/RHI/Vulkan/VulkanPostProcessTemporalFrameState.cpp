#include "VulkanPostProcessTemporalFrameState.h"
#include "VulkanPostProcessMotionBuffers.h"
#include "Renderer/Graph/RenderFramePublication.h"

#include <algorithm>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		bool CameraContinuous(const RenderViewPresentationInput& from, const RenderViewPresentationInput& to)
		{
			// Reference origins follow the eye, so their local translations
			// can both be zero during a teleport. Compare canonical positions.
			constexpr double cutDistance = 25.0;
			const auto oldEye = Scene::ApplyWorldDelta(from.referenceFrame.canonicalOrigin,
				{{}, glm::dvec3(from.cameraLocalPosition)});
			const auto newEye = Scene::ApplyWorldDelta(to.referenceFrame.canonicalOrigin,
				{{}, glm::dvec3(to.cameraLocalPosition)});
			if(!oldEye.Succeeded() || !newEye.Succeeded()) return false;
			const auto movement = Scene::LocalizeWorldPosition(newEye.position, oldEye.position, cutDistance);
			if(!movement.Succeeded() || glm::length(movement.offsetMetres) > cutDistance) return false;
			// Compare basis rows without their translations. A large orientation
			// jump is a cut even if the eye stays at the same world address.
			for(int row = 0; row < 3; ++row)
			{
				const glm::vec3 before(from.viewMatrix[0][row], from.viewMatrix[1][row], from.viewMatrix[2][row]);
				const glm::vec3 after(to.viewMatrix[0][row], to.viewMatrix[1][row], to.viewMatrix[2][row]);
				if(!(glm::dot(before, after) >= 0.70710678f)) return false;
			}
			return true;
		}
	}

	TemporalViewFrameState::TemporalViewFrameState() = default;

	bool TemporalViewFrameState::Writable(std::uint32_t index) const noexcept
	{
		const auto& slot = slots[index];
		for(std::uint32_t frame = 0u; frame < completed.size(); ++frame)
			if(slot.lastEpoch[frame] > completed[frame]) return false;
		return std::none_of(reserved.begin(), reserved.end(), [index](const auto& entry) {
			return entry.recordingId != 0u && (entry.writeSlot == index || entry.readSlot == index);
		});
	}

	std::optional<TemporalViewFramePlan> TemporalViewFrameState::Reserve(
		std::uint64_t passGeneration, std::uint64_t recordingId, std::uint32_t frameSlot,
		const PostProcessHistoryScope& scope, const PostProcessHistorySignature& signature,
		std::shared_ptr<const RenderFramePublication> publication, std::uint32_t presentationIndex,
		std::uint64_t frameId) noexcept
	{
		if(!passGeneration || !recordingId || frameSlot >= completed.size()
			|| (generation && passGeneration != generation)) return std::nullopt;
		const auto room = std::find_if(reserved.begin(), reserved.end(), [](const auto& entry) { return !entry.recordingId; });
		if(room == reserved.end()) return std::nullopt;
		if(!publication || presentationIndex >= publication->presentationInputs.size()) return std::nullopt;
		const auto candidate = history.Previous(scope, signature);
		const auto prior = candidate && CameraContinuous(candidate->publication->presentationInputs[candidate->presentationIndex],
			publication->presentationInputs[presentationIndex]) ? candidate : nullptr;
		TemporalViewFramePlan plan;
		if(prior && accepted && accepted->frameId == prior->frameId && accepted->historyToken == prior->recordingId)
		{
			plan.previous = accepted;
			plan.previousMeshes = acceptedMeshes;
			plan.previousMeshMotion = acceptedMeshMotion;
		}
		// A reset still binds a distinct legal descriptor slot. Reuse the
		// accepted slot as a read-only dummy when its layout is already GENERAL.
		// The shader never samples that slot during a reset.
		for(plan.writeSlot = 0u; plan.writeSlot < kSlotCount; ++plan.writeSlot)
			if(Writable(plan.writeSlot) && (!accepted || plan.writeSlot != accepted->slot)) break;
		if(plan.writeSlot == kSlotCount) return std::nullopt;
		if(plan.previous) plan.readSlot = plan.previous->slot;
		else if(accepted && slots[accepted->slot].layout.depthLayout == VK_IMAGE_LAYOUT_GENERAL
			&& slots[accepted->slot].layout.colorLayout == VK_IMAGE_LAYOUT_GENERAL)
			plan.readSlot = accepted->slot;
		else
		{
			for(plan.readSlot = 0u; plan.readSlot < kSlotCount; ++plan.readSlot)
				if(plan.readSlot != plan.writeSlot && Writable(plan.readSlot)) break;
			if(plan.readSlot == kSlotCount) return std::nullopt;
		}
		// Every new history starts at the same phase. A rejected recording
		// cannot advance the pattern independently from its queue acceptance.
		const auto jitterPhase = prior ? (prior->jitterPhase + 1u) % 8u : 0u;
		auto provenance = history.Record(recordingId, scope, signature, std::move(publication),
			presentationIndex, frameId, jitterPhase, bool(prior));
		if(!provenance) return std::nullopt;
		if(provenance->previous != prior)
		{
			history.Discard(recordingId);
			return std::nullopt;
		}
		plan.provenance = std::move(*provenance);
		for(std::uint32_t slot = 0u; slot < kSlotCount; ++slot) plan.layouts[slot] = slots[slot].layout;
		generation = passGeneration;
		*room = {recordingId, frameId, frameSlot, plan.writeSlot, plan.readSlot};
		return plan;
	}

	bool TemporalViewFrameState::Record(VkCommandBuffer command, const TemporalRecordedUse& recorded,
		std::shared_ptr<const PostProcessMotionBuffers> motionBuffers) noexcept
	{
		if(!command || !motionBuffers || recorded.passGeneration != generation || !recorded.recordingId
			|| recorded.historyToken != recorded.recordingId) return false;
		const auto match = std::find_if(reserved.begin(), reserved.end(), [&](const auto& entry) {
			return entry.recordingId == recorded.recordingId && !entry.command && entry.frameId == recorded.frameId
				&& entry.writeSlot == recorded.writeSlot && entry.readSlot == recorded.readSlot;
		});
		if(match == reserved.end()) return false;
		match->command = command;
		match->recorded = recorded;
		match->motionBuffers = std::move(motionBuffers);
		return true;
	}

	bool TemporalViewFrameState::Accept(VkCommandBuffer command) noexcept
	{
		if(!command) return false;
		const auto match = std::find_if(reserved.begin(), reserved.end(),
			[command](const auto& entry) {
				return entry.command == command && !entry.submittedWithoutEpoch && !entry.submissionEpoch && entry.recorded;
			});
		if(match == reserved.end() || !history.Accept(match->recordingId)) return false;
		const auto& recorded = *match->recorded;
		slots[match->writeSlot].layout = recorded.writeState;
		slots[match->readSlot].layout = recorded.readState;
		accepted = TemporalAcceptedHistory{generation, match->frameId, match->recordingId, match->writeSlot};
		// Keep the exact CPU mesh publications that produced accepted depth.
		// Retain these independently so older descriptor pools can retire.
		acceptedMeshes = match->motionBuffers->prepared.meshes;
		acceptedMeshMotion = match->motionBuffers->meshMotion;
		match->submittedWithoutEpoch = true;
		return true;
	}

	void TemporalViewFrameState::AttachCompletion(std::uint32_t frameSlot, std::uint64_t epoch) noexcept
	{
		if(frameSlot >= completed.size() || epoch <= completed[frameSlot]) return;
		for(auto& entry : reserved)
		{
			if(entry.frameSlot != frameSlot || !entry.submittedWithoutEpoch
				|| !history.AttachCompletion(entry.recordingId, frameSlot, epoch)) continue;
			for(const auto slotIndex : {entry.writeSlot, entry.readSlot})
				slots[slotIndex].lastEpoch[frameSlot] = std::max(slots[slotIndex].lastEpoch[frameSlot], epoch);
			// The VkDescriptorSet and six upload buffers remain owned until
			// the actual fence signals. Attaching an epoch is not completion.
			entry.submittedWithoutEpoch = false;
			entry.submissionEpoch = epoch;
		}
	}

	void TemporalViewFrameState::Complete(std::uint32_t frameSlot, std::uint64_t epoch) noexcept
	{
		if(frameSlot >= completed.size()) return;
		completed[frameSlot] = std::max(completed[frameSlot], epoch);
		history.Complete(frameSlot, epoch);
		for(auto& entry : reserved)
			if(entry.frameSlot == frameSlot && entry.submissionEpoch != 0u && entry.submissionEpoch <= epoch)
				entry = {};
	}

	void TemporalViewFrameState::CompleteDeviceIdle() noexcept
	{
		history.CompleteDeviceIdle();
		for(auto& entry : reserved) if(entry.submittedWithoutEpoch || entry.submissionEpoch) entry = {};
		for(auto& slot : slots) slot.lastEpoch = {};
	}

	void TemporalViewFrameState::Discard(VkCommandBuffer command) noexcept
	{
		if(!command) return;
		for(auto& entry : reserved)
			if(entry.command == command && !entry.submittedWithoutEpoch && !entry.submissionEpoch)
			{
				history.Discard(entry.recordingId);
				entry = {};
			}
	}

	void TemporalViewFrameState::CancelReservation(std::uint64_t recordingId) noexcept
	{
		if(!recordingId) return;
		for(auto& entry : reserved)
			if(entry.recordingId == recordingId && !entry.command)
			{
				history.Discard(entry.recordingId);
				entry = {};
			}
	}
}
