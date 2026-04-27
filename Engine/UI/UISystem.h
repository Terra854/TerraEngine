// UISystem.h — Main orchestrator for the engine's UI subsystem.
//
// This file defines the UISystem class, which manages the lifecycle of UI
// documents, handles user input (mouse/keyboard) for UI interaction, and
// coordinates between the parser, layout engine, and renderer. It supports
// asynchronous document builds and maintains the global UI render state.

#pragma once

#include "Core/SystemBase.h"
#include "UIParser.h"
#include "UILayoutEngine.h"
#include "UIRenderer.h"

#include <entt/entity/entity.hpp>
#include <glm/vec2.hpp>

#include <future>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class UIComponent;

// UISystem keeps authored UI documents off the hot path by parsing and laying
// them out on worker tasks, then publishing a lightweight CPU-side batch that
// the editor can preview immediately.
class UISystem : public SystemsBase
{
public:
	// struct UIBuildResult groups related state and operations so callers have one documented contract.
	struct UIBuildResult
	{
		UIElement::Ptr root;
		UIRenderBatch batch;
		std::string error;
	};

	static UISystem& Instance();

	UISystem(const UISystem&) = delete;
	UISystem& operator=(const UISystem&) = delete;
	UISystem(UISystem&&) = delete;
	UISystem& operator=(UISystem&&) = delete;
	~UISystem() override = default;

	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;

	void SetGameViewportSize(const glm::vec2& viewportSize);
	void SetEditorGameViewPointer(const glm::vec2& pointerPosition, bool pointerInsideViewport, bool leftButtonDown);
	void SubmitImmediateQuad(const UIImmediateQuad& quad);

	[[nodiscard]] UIRenderBatch GetCombinedFrameBatch() const;
	[[nodiscard]] UIBuildResult BuildPreviewDocument(const UIElement::Ptr& root, const glm::vec2& viewportSize) const;
	[[nodiscard]] UIRenderBatch BuildPreviewBatch(const UIElement::Ptr& root, const glm::vec2& viewportSize) const;
	[[nodiscard]] std::string SerializeMarkup(const UIElement::Ptr& root) const;
	[[nodiscard]] std::string SerializeStyles(const UIElement::Ptr& root) const;
	[[nodiscard]] bool WasElementClicked(std::string_view elementId) const;
	[[nodiscard]] bool IsElementHovered(std::string_view elementId) const;
	[[nodiscard]] std::uint64_t GetInteractionFrame() const;
	bool EnsureDocumentReady(entt::entity entity, UIComponent& component);

private:
	UISystem() = default;

	// struct UIDocumentState groups related state and operations so callers have one documented contract.
	struct UIDocumentState
	{
		std::uint64_t contentVersion = 0;
		glm::vec2 viewportSize = glm::vec2(0.0f, 0.0f);
		std::shared_future<UIBuildResult> buildFuture;
		bool buildInFlight = false;
		UIElement::Ptr root;
		UIRenderBatch batch;
		std::string lastError;
	};

	// struct PointerState groups related state and operations so callers have one documented contract.
	struct PointerState
	{
		glm::vec2 position = glm::vec2(0.0f);
		bool insideViewport = false;
		bool leftButtonDown = false;
	};

	UIBuildResult BuildDocumentFromSources(
		const std::string& markupPath,
		const std::vector<std::string>& stylePaths,
		const UIElement::Ptr& existingRoot,
		const glm::vec2& viewportSize) const;

	void ScheduleDocumentBuild(entt::entity entity, UIComponent& component, UIDocumentState& state, const glm::vec2& viewportSize);
	void IntegrateReadyBuilds();
	void RefreshCombinedBatch();
	[[nodiscard]] glm::vec2 GetFallbackViewportSize() const;

	UIParser parser_;
	UILayoutEngine layoutEngine_;
	UIRenderer renderer_;
	mutable std::mutex stateMutex_;
	std::unordered_map<entt::entity, UIDocumentState> documents_;
	std::vector<UIImmediateQuad> pendingImmediateQuads_;
	UIRenderBatch combinedBatch_;
	glm::vec2 gameViewportSize_ = glm::vec2(0.0f, 0.0f);
	PointerState editorGameViewPointer_;
	bool hasEditorGameViewPointer_ = false;
	bool previousLeftButtonDown_ = false;
	std::unordered_set<std::string> clickedElementIds_;
	std::unordered_set<std::string> hoveredElementIds_;
	std::unordered_map<std::string, UIElement::Ptr> hoveredElementRefs_;
	std::uint64_t interactionFrame_ = 0;
	bool initialized_ = false;
};
