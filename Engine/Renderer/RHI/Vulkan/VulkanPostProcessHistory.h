// VulkanPostProcessHistory.h - CPU provenance for future per-view reconstruction.
#pragma once

#include "Renderer/GraphicsSettings/GraphicsSettingsTypes.h"
#include "Scene/WorldCoordinate.h"
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <vector>

namespace TerraEngine::Renderer { struct RenderFramePublication; }

namespace TerraEngine::Renderer::Vulkan
{
	// All access belongs to the protected renderer boundary. Worker preparation
	// may read copied returned immutable states; it must not mutate this owner.
	struct PostProcessHistoryScope
	{
		GraphicsSettings::GraphicsTarget target;
		GraphicsSettings::GraphicsLifecycleStamp lifecycle;
		bool operator==(const PostProcessHistoryScope&) const = default;
	};

	// Tokens describe actual consumed inputs, never the latest editor settings.
	// Ordinary camera movement does not change projection/cut tokens. Origin
	// changes deliberately reset this first scaffold rather than guessing rebases.
	struct PostProcessHistorySignature
	{
		std::uint32_t internalWidth = 0, internalHeight = 0;
		std::uint32_t outputWidth = 0, outputHeight = 0;
		std::uint32_t colorFormat = 0, depthFormat = 0, motionFormat = 0;
		std::uint32_t scaleNumerator = 0, scaleDenominator = 0;
		std::uint64_t reconstructionGeneration = 0;
		std::uint64_t projectionGeneration = 0, cutGeneration = 0;
		std::uint64_t worldOriginGeneration = 0, surfaceGeneration = 0;
		TerraEngine::Scene::WorldCell worldOriginCell{};
		bool operator==(const PostProcessHistorySignature&) const = default;
	};

	struct PostProcessHistoryState
	{
		PostProcessHistoryScope scope;
		PostProcessHistorySignature signature;
		std::shared_ptr<const RenderFramePublication> publication;
		std::uint32_t presentationIndex = 0;
		std::uint64_t frameId = 0;
		// A recording ID proves identity only. It does not imply native submission.
		std::uint64_t recordingId = 0;
		std::uint32_t jitterPhase = 0;
	};

	struct PostProcessHistoryRecording
	{
		std::shared_ptr<const PostProcessHistoryState> current;
		// Empty on first use or any incompatible scope/signature. The root retains
		// exact canonical poses and CPU animation owners of the accepted view.
		std::shared_ptr<const PostProcessHistoryState> previous;
	};

	// No images, motion production, or temporal capability are supplied here.
	// Native integration must additionally retain its exact mesh/deformed-buffer
	// owners; publication identity alone does not freeze mutable GPU buffer bytes.
	class PostProcessHistory
	{
	public:
		explicit PostProcessHistory(std::uint32_t frameSlotCount = 3);
		~PostProcessHistory();
		PostProcessHistory(const PostProcessHistory&) = delete;
		PostProcessHistory& operator=(const PostProcessHistory&) = delete;

		// recordingId is a globally increasing owner-local command recording token,
		// not a recycled VkCommandBuffer address. Allocate all storage before submit.
		[[nodiscard]] std::optional<PostProcessHistoryRecording> Record(
			std::uint64_t recordingId, const PostProcessHistoryScope& scope,
			const PostProcessHistorySignature& signature,
			std::shared_ptr<const RenderFramePublication> publication,
			std::uint32_t presentationIndex, std::uint64_t frameId, std::uint32_t jitterPhase,
			bool allowPrevious = true) noexcept;
		// Call only after the corresponding real queue submit succeeds, in submit
		// order. An unfenced editor Scene submit remains retained until attached.
		[[nodiscard]] bool Accept(std::uint64_t recordingId) noexcept;
		[[nodiscard]] bool AttachCompletion(std::uint64_t recordingId,
			std::uint32_t frameSlot, std::uint64_t submissionEpoch) noexcept;
		// Observe the exact submitted epoch before resetting/reusing its fence.
		void Complete(std::uint32_t frameSlot, std::uint64_t submissionEpoch) noexcept;
		void CompleteDeviceIdle() noexcept;
		void Discard(std::uint64_t recordingId) noexcept;
		void Retire(const PostProcessHistoryScope& scope) noexcept;
		[[nodiscard]] std::shared_ptr<const PostProcessHistoryState> Previous(
			const PostProcessHistoryScope& scope,
			const PostProcessHistorySignature& signature) const noexcept;
		[[nodiscard]] std::size_t RetainedUseCount() const noexcept;

	private:
		struct View;
		struct Use;
		std::list<std::shared_ptr<View>> views;
		std::list<Use> uses;
		std::vector<std::uint64_t> completedEpochs;
		std::uint64_t lastRecordingId = 0;
	};
}
