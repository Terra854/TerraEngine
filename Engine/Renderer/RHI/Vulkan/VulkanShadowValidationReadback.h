// Validation-only copies share the caller's primary and existing fence receipts.
#pragma once
#include "VulkanCommonIncludes.h"
#include "Renderer/RenderTypes.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include <memory>

namespace TerraEngine::Renderer
{
	// Receipt transitions are allocation-free after the real queue has accepted
	// work. Wrong commands or older completions never advance an action's proof.
	inline bool MarkShadowValidationFixtureActionSubmitted(RendererEfficiencyFixtureActionReceipt& action, std::uint64_t command, std::uint64_t submit) noexcept
	{
		if (!command || !submit || !action.recorded || action.commandSerial != command || (action.submitSerial && action.submitSerial != submit)) return false;
		action.submitSerial = submit; action.submitted = true; return true;
	}
	inline bool MarkShadowValidationFixtureActionCompleted(RendererEfficiencyFixtureActionReceipt& action, std::uint64_t submit, std::uint64_t completion) noexcept
	{
		if (!action.submitted || !submit || action.submitSerial != submit || completion < submit) return false;
		action.completionSerial = completion; action.completed = true; return true;
	}
	struct ShadowValidationFrameIdentity
	{
		std::uint64_t rendererSessionGeneration = 0, frameSerial = 0, commandSerial = 0;
		RenderViewType viewType = RenderViewType::Game;
		std::uint32_t viewId = 0;
		bool operator==(const ShadowValidationFrameIdentity&) const = default;
	};
	// One native frame is sampled for each view and exact fixture control token.
	// Remaining faces in that frame share the sample; later frames stay frozen
	// until a new control publication or fixture correlation is accepted.
	struct ShadowValidationSampleIntent
	{
		RenderViewType viewType = RenderViewType::Game;
		std::uint32_t viewId = 0;
		RendererEfficiencyFixtureCorrelation correlation{};
		std::uint64_t controlGeneration = 0, frameSerial = 0;
	};
	inline bool AdmitShadowValidationSample(ShadowValidationSampleIntent& retained,
		const RendererEfficiencyEvidence& requested)
	{
		if(retained.correlation == requested.fixtureCorrelation
			&& retained.controlGeneration == requested.fixtureControlGeneration)
			return retained.frameSerial == requested.frameSerial;
		retained.correlation = requested.fixtureCorrelation;
		retained.controlGeneration = requested.fixtureControlGeneration;
		retained.frameSerial = requested.frameSerial;
		return true;
	}
	struct ShadowValidationObservation
	{
		RendererEfficiencyEvidence evidence;
		ShadowViewKey key;
		ShadowCasterMembership membership;
		std::vector<ShadowCasterStamp> submittedSegments;
		std::uint64_t contentToken = 0;
		bool preparationComplete = true; // False means eligible draw coverage was lost.
	};
	class TERRAENGINE_API VulkanShadowValidationReadback
	{
	public:
		VulkanShadowValidationReadback();
		~VulkanShadowValidationReadback();
		VulkanShadowValidationReadback(const VulkanShadowValidationReadback&) = delete;
		VulkanShadowValidationReadback& operator=(const VulkanShadowValidationReadback&) = delete;
		void Init(VmaAllocator allocator, VkPhysicalDevice physicalDevice);
		bool RecordFace(VkCommandBuffer command, VkImage image, VkFormat format, std::uint32_t layer, const ShadowValidationObservation& observation);
		// The caller supplies the exact uploaded physical snapshot slice. Copies
		// share the existing primary, submission and fence; this never submits.
		bool RecordLighting(VkCommandBuffer command, VkBuffer source, std::uint64_t offset, std::uint64_t size, const RendererEfficiencyEvidence& evidence, std::uint32_t sourceCandidateCount);
		bool RecordFrameColor(VkCommandBuffer command, VkImage image, VkFormat format, std::uint32_t width, std::uint32_t height, VkImageLayout layout, const ShadowValidationFrameIdentity& identity);
		bool ConfirmSubmitted(std::span<const std::uint64_t> orderedCommands, std::uint64_t actualSubmitSerial);
		void PreserveSubmittedCommand(std::uint64_t commandSerial, std::uint64_t actualSubmitSerial) noexcept;
		void CompleteSubmission(std::uint64_t actualSubmitSerial, std::uint64_t completionSerial);
		void DiscardCommand(std::uint64_t commandSerial);
		// Attach only actions that actually happened to this recorded primary.
		// Submission/completion fields are filled solely by real receipt hooks.
		bool AttachFixtureAction(std::uint64_t commandSerial, const RendererEfficiencyFixtureActionReceipt& action);
		// Reset cancels observations but retains recorded and submitted staging:
		// descriptor synchronization may still submit an older recorded primary.
		// Explicit discard or completion proves release; device loss is terminal.
		void Reset(std::uint64_t newSession, bool deviceLost = false);
		bool Shutdown(bool gpuIdleOrDeviceLost);
		std::vector<RendererEfficiencyEvidence> DrainCompleted();
	private:
		struct State;
		std::unique_ptr<State> state;
	};
}
