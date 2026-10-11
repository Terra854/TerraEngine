// Completion-qualified depth evidence owns its queries and validation-only GPU resources.
#pragma once
#include "VulkanCommonIncludes.h"
#include "Renderer/RenderTypes.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include <memory>
#include <span>

namespace TerraEngine::Renderer
{
	struct LightingDepthSampleIntent
	{
		RenderViewType view = RenderViewType::Game;
		std::uint32_t output = 0;
		RendererEfficiencyFixtureCorrelation correlation;
		std::uint64_t control = 0, frame = 0;
	};
	inline bool LightingDepthSampleCanRecord(const LightingDepthSampleIntent& retained, const RendererEfficiencyEvidence& row)
	{
		return retained.view != row.viewType || retained.output != row.viewId || retained.correlation != row.fixtureCorrelation
			|| retained.control != row.fixtureControlGeneration || retained.frame == row.frameSerial;
	}
	// Only an actual unsubmitted discard releases this first-frame reservation.
	// A stale discard cannot revoke a later control token or submitted owner.
	inline bool ReleaseLightingDepthSampleOnDiscard(LightingDepthSampleIntent& retained, const RendererEfficiencyEvidence& row, bool submitted)
	{
		if (submitted || retained.view != row.viewType || retained.output != row.viewId || retained.frame != row.frameSerial
			|| retained.control != row.fixtureControlGeneration || retained.correlation != row.fixtureCorrelation) return false;
		retained = {}; return true;
	}
	// Color attachment is an exact native owner join, never a nearest-frame lookup.
	inline bool LightingDepthColorOwnerMatches(const RendererEfficiencyEvidence& captured, const RendererEfficiencyEvidence& color)
	{
		return color.status == RendererEfficiencyEvidenceStatus::Valid && color.artifacts && color.artifacts->frameColor
			&& captured.frameSerial && captured.frameSerial == color.frameSerial && captured.commandSerial == color.commandSerial
			&& captured.submitSerial && captured.submitSerial == color.submitSerial && captured.completionSerial >= captured.submitSerial
			&& captured.completionSerial == color.completionSerial
			&& captured.viewType == color.viewType && captured.viewId == color.viewId
			&& captured.effectiveOptions.rendererSessionGeneration == color.effectiveOptions.rendererSessionGeneration
			&& captured.effectiveOptions.settingsGeneration == color.effectiveOptions.settingsGeneration
			&& captured.effectiveOptions.optionsSessionGeneration == color.effectiveOptions.optionsSessionGeneration
			&& captured.effectiveOptions.options == color.effectiveOptions.options
			&& captured.fixtureCorrelation == color.fixtureCorrelation && captured.fixtureControlGeneration == color.fixtureControlGeneration;
	}
	class TERRAENGINE_API VulkanLightingDepthEvidence
	{
	public:
		struct Config
		{
			VkDevice device = VK_NULL_HANDLE;
			VkPhysicalDevice physical = VK_NULL_HANDLE;
			VmaAllocator allocator = VK_NULL_HANDLE;
			std::uint64_t rendererSessionGeneration = 0, backendResourceGeneration = 0;
			std::uint32_t timestampValidBits = 0;
			double periodNanoseconds = 0;
			// Initialize consumes these words synchronously; no caller span is retained.
			std::span<const std::uint32_t> cooperativeOracleSpirv, referenceOracleSpirv;
		};
		struct Request
		{
			RendererEfficiencyEvidence evidence;
			std::uint32_t frameSlot = 0, snapshotSlot = 0, width = 0, height = 0;
			bool cooperative = false, validation = false;
			std::optional<LightingDepthReduction::DepthRecipe> oracleRecipe;
		};
		VulkanLightingDepthEvidence();
		~VulkanLightingDepthEvidence();
		VulkanLightingDepthEvidence(const VulkanLightingDepthEvidence&) = delete;
		VulkanLightingDepthEvidence& operator=(const VulkanLightingDepthEvidence&) = delete;
		bool Initialize(const Config& config);
		std::uint64_t QueryPoolGeneration() const;
		bool BeginProduction(VkCommandBuffer command, const Request& request);
		bool EndProduction(VkCommandBuffer command, const Request& request);
		bool RecordProductionBounds(VkCommandBuffer command, const Request& request, VkBuffer source, std::uint64_t offset, std::uint64_t allocationBytes);
		bool RecordOracle(VkCommandBuffer command, const Request& request, const LightingDepthReduction::DepthRecipe& recipe,
			std::span<const std::uint32_t> input, const FrameCullingDataGPU& culling);
		bool ConfirmSubmitted(std::span<const std::uint64_t> orderedCommands, std::uint64_t submit);
		void PreserveSubmittedCommand(std::uint64_t command, std::uint64_t submit) noexcept;
		void CompleteSubmission(std::uint64_t submit, std::uint64_t completion);
		bool AttachCompletedFrameColor(const RendererEfficiencyEvidence& color);
		void DiscardCommand(std::uint64_t command);
		void Reset(std::uint64_t newSession, bool deviceLost = false);
		bool Shutdown(bool gpuIdleOrDeviceLost);
		std::vector<RendererEfficiencyEvidence> DrainCompleted();
	private:
		struct State;
		std::unique_ptr<State> state;
	};
}
