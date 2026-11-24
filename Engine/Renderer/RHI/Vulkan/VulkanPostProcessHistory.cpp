// VulkanPostProcessHistory.cpp - protected, allocation-safe submission ownership.
#include "VulkanPostProcessHistory.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include <algorithm>

namespace TerraEngine::Renderer::Vulkan
{
	struct PostProcessHistory::View
	{
		PostProcessHistoryScope scope;
		std::shared_ptr<const PostProcessHistoryState> accepted;
		bool retired = false;
	};
	struct PostProcessHistory::Use
	{
		std::shared_ptr<View> view;
		PostProcessHistoryRecording recording;
		bool submitted = false, fenced = false;
		std::uint32_t slot = 0;
		std::uint64_t epoch = 0;
	};

	namespace
	{
		bool ValidScope(const PostProcessHistoryScope& scope) noexcept
		{
			if(!scope.target.view || !scope.target.view->lifetimeGeneration
				|| !scope.lifecycle.rendererSessionGeneration
				|| !scope.lifecycle.backendResourceGeneration) return false;
			if(scope.target.scope == GraphicsSettings::GraphicsScope::Camera && !scope.target.camera) return false;
			if(scope.target.camera)
			{
				const auto& camera = *scope.target.camera;
				if(camera.cameraId == TerraEngine::EMPTY_UUID || !camera.sceneGeneration
					|| !camera.objectLifetimeGeneration || !camera.componentGeneration
					|| !camera.bindingGeneration) return false;
			}
			return true;
		}
		bool ValidSignature(const PostProcessHistorySignature& s) noexcept
		{
			return s.internalWidth && s.internalHeight && s.outputWidth && s.outputHeight
				&& s.colorFormat && s.depthFormat && s.motionFormat
				&& s.scaleNumerator && s.scaleDenominator && s.reconstructionGeneration
				&& s.projectionGeneration && s.cutGeneration && s.worldOriginGeneration
				&& s.surfaceGeneration;
		}
	}

	PostProcessHistory::PostProcessHistory(std::uint32_t frameSlotCount)
		: completedEpochs(frameSlotCount, 0) {}
	PostProcessHistory::~PostProcessHistory() = default;

	std::optional<PostProcessHistoryRecording> PostProcessHistory::Record(
		std::uint64_t recordingId, const PostProcessHistoryScope& scope,
		const PostProcessHistorySignature& signature,
		std::shared_ptr<const RenderFramePublication> publication,
		std::uint32_t presentationIndex, std::uint64_t frameId, std::uint32_t jitterPhase,
		bool allowPrevious) noexcept
	{
		if(!recordingId || recordingId <= lastRecordingId || !ValidScope(scope)
			|| !ValidSignature(signature) || !publication || !frameId
			|| publication->frameId != frameId
			|| publication->rendererSessionGeneration != scope.lifecycle.rendererSessionGeneration
			|| publication->backendResourceGeneration != scope.lifecycle.backendResourceGeneration) return std::nullopt;
		if(presentationIndex >= publication->presentationInputs.size()) return std::nullopt;
		const auto& input = publication->presentationInputs[presentationIndex];
		const bool matchesNativeRuntimeExtent = input.output.kind == RenderOutputKind::MainRuntimeView
			&& input.output.targetSize.x == 0u && input.output.targetSize.y == 0u && input.graphicsSettings
			&& input.graphicsSettings->outputWidth == signature.outputWidth
			&& input.graphicsSettings->outputHeight == signature.outputHeight;
		if(input.graphicsView != scope.target.view || input.graphicsCamera != scope.target.camera
			|| !input.graphicsSettings || input.graphicsSettings->lifecycle != scope.lifecycle
			|| input.output.kind != scope.target.view->outputKind
			|| input.output.diagnosticsView != scope.target.view->viewType
			|| input.output.outputId != scope.target.view->outputId
			|| (!matchesNativeRuntimeExtent && (input.output.targetSize.x != signature.outputWidth
				|| input.output.targetSize.y != signature.outputHeight))) return std::nullopt;
		// One unresolved recording per exact live view prevents two candidates
		// from both selecting the same predecessor before the first submits.
		for(const auto& use : uses)
			if(!use.submitted && !use.view->retired && use.view->scope == scope) return std::nullopt;
		try
		{
			auto found = std::find_if(views.begin(), views.end(), [&](const auto& view) { return view->scope == scope; });
			const bool newView = found == views.end();
			auto view = newView ? std::make_shared<View>() : *found;
			if(newView) view->scope = scope;
			auto current = std::make_shared<const PostProcessHistoryState>(PostProcessHistoryState{
				scope, signature, std::move(publication), presentationIndex, frameId, recordingId, jitterPhase});
			PostProcessHistoryRecording result{std::move(current), {}};
			// A cut belongs only to this recording until the actual submit
			// succeeds. Leaving the old receipt intact allows a failed cut to
			// be discarded without wiping the previous accepted image.
			if(allowPrevious && view->accepted && view->accepted->signature == signature) result.previous = view->accepted;
			// Stage the use first. If publishing a new lookup owner allocates and
			// fails, roll back the staged use; accepted history remains unchanged.
			uses.push_back({view, result});
			if(newView)
			{
				try { views.push_back(view); }
				catch(...) { uses.pop_back(); throw; }
			}
			lastRecordingId = recordingId;
			return result;
		}
		catch(...) { return std::nullopt; }
	}

	bool PostProcessHistory::Accept(std::uint64_t recordingId) noexcept
	{
		for(auto& use : uses)
			if(use.recording.current->recordingId == recordingId)
			{
				if(use.submitted) return false;
				use.submitted = true;
				if(!use.view->retired) use.view->accepted = use.recording.current;
				return true;
			}
		return false;
	}

	bool PostProcessHistory::AttachCompletion(std::uint64_t recordingId,
		std::uint32_t frameSlot, std::uint64_t submissionEpoch) noexcept
	{
		if(frameSlot >= completedEpochs.size() || submissionEpoch <= completedEpochs[frameSlot]) return false;
		for(auto& use : uses)
			if(use.recording.current->recordingId == recordingId)
			{
				if(!use.submitted || use.fenced) return false;
				use.fenced = true; use.slot = frameSlot; use.epoch = submissionEpoch;
				return true;
			}
		return false;
	}

	void PostProcessHistory::Complete(std::uint32_t frameSlot, std::uint64_t submissionEpoch) noexcept
	{
		if(frameSlot >= completedEpochs.size()) return;
		completedEpochs[frameSlot] = std::max(completedEpochs[frameSlot], submissionEpoch);
		uses.remove_if([&](const Use& use) {
			return use.submitted && use.fenced && use.slot == frameSlot
				&& use.epoch <= completedEpochs[frameSlot];
		});
	}

	void PostProcessHistory::CompleteDeviceIdle() noexcept
	{
		// Idle proves submitted work only; unsubmitted command recordings may
		// still borrow their exact predecessor and must be explicitly discarded.
		uses.remove_if([](const Use& use) { return use.submitted; });
	}
	void PostProcessHistory::Discard(std::uint64_t recordingId) noexcept
	{
		uses.remove_if([&](const Use& use) {
			return !use.submitted && use.recording.current->recordingId == recordingId;
		});
	}
	void PostProcessHistory::Retire(const PostProcessHistoryScope& scope) noexcept
	{
		views.remove_if([&](const auto& view) {
			if(view->scope != scope) return false;
			view->retired = true;
			return true;
		});
	}
	std::shared_ptr<const PostProcessHistoryState> PostProcessHistory::Previous(
		const PostProcessHistoryScope& scope, const PostProcessHistorySignature& signature) const noexcept
	{
		for(const auto& view : views)
			if(view->scope == scope && view->accepted && view->accepted->signature == signature)
				return view->accepted;
		return {};
	}
	std::size_t PostProcessHistory::RetainedUseCount() const noexcept { return uses.size(); }
}
