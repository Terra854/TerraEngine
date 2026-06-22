// UISystem.cpp — Implementation of the UI system core logic.
//
// This file implements the main UISystem class, managing the update loop for
// all active UI documents. It handles asynchronous rebuilding of UI visual
// trees, processing of input events like clicks and hovers, and the
// aggregation of render batches from multiple UI components.

#include "UISystem.h"

#include "Components/UIComponent.h"
#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Input/InputCodes.h"
#include "Input/InputSystem.h"
#include "Profiling/ProfilerScope.h"
#include "Resource/ResourceManager.h"
#include "Scripting/ScriptingSystem.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace
{
// GetInteractiveElementMin is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
glm::vec2 GetInteractiveElementMin(const UIElement& element)
{
	// Runtime hit testing must respect the same transform-driven translate
	// offset the preview renderer uses, otherwise hover/click targeting drifts
	// away from the actual drawn element.
	const glm::vec2 translate = element.displayStyle.transform.translate.value_or(glm::vec2(0.0f, 0.0f));
	return element.calculatedPosition + translate;
}

// GetInteractiveElementSize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
glm::vec2 GetInteractiveElementSize(const UIElement& element)
{
	// Scale is authored on the visual style, so hit testing must expand or
	// shrink the logical layout size to match the final rendered rectangle.
	const float scale = element.displayStyle.transform.scale.value_or(1.0f);
	return element.calculatedSize * scale;
}

bool BuildHoveredElementPath(
	const UIElement::Ptr& element,
	const glm::vec2& localPointerPosition,
	std::vector<UIElement::Ptr>& hoveredPath,
	UIElement::Ptr& outHitElement)
{
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!element || !element->IsVisible())
	{
		return false;
	}

	const glm::vec2 elementMin = GetInteractiveElementMin(*element);
	const glm::vec2 elementMax = elementMin + GetInteractiveElementSize(*element);
	const bool pointerInsideElement = localPointerPosition.x >= elementMin.x
		&& localPointerPosition.y >= elementMin.y
		&& localPointerPosition.x <= elementMax.x
		&& localPointerPosition.y <= elementMax.y;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!pointerInsideElement)
	{
		return false;
	}

	hoveredPath.push_back(element);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t reverseIndex = element->children.size(); reverseIndex > 0; --reverseIndex)
	{
		const UIElement::Ptr& child = element->children[reverseIndex - 1];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (BuildHoveredElementPath(child, localPointerPosition, hoveredPath, outHitElement))
		{
			return true;
		}
	}

	outHitElement = element;
	return true;
}

TerraEngine::Scheduling::EcsComponentQueryDeclaration MakeProtectedUiQuery(
	bool includeDiagnosticsMetadata,
	TerraEngine::Scheduling::EcsQueryChunkWork work)
{
	TerraEngine::Scheduling::EcsComponentQueryDeclaration query;
	query.queryName = "UI.CollectVisibleComponents";
	if (includeDiagnosticsMetadata)
	{
		query.access.ownerSystem = "UISystem";
		query.access.queryName = query.queryName;
		query.access.readComponents = { "UIComponent" };
		query.access.writeResources = { "UI.LiveComponents" };
		query.access.workerEligible = false;
		query.access.mainThreadReason = "UI collection stores live component pointers that are consumed by protected document/input state.";
	}
	query.preserveMatchingObjectOrder = true;
	query.copyObjectIdsIntoChunks = false;
	query.runOnceWithoutObjectSnapshot = true;
	query.work = std::move(work);
	return query;
}

// ApplyHoverStateRecursive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ApplyHoverStateRecursive(const UIElement::Ptr& element, const std::unordered_set<const UIElement*>& hoveredElements)
{
	if (!element)
	{
		return;
	}

	const bool nextHovered = hoveredElements.contains(element.get());
	// UIElement internally tracks hover animation state, so the system only
	// needs to publish whether each node is considered hovered this frame.
	element->SetHoverState(nextHovered);

	for (const UIElement::Ptr& child : element->children)
	{
		ApplyHoverStateRecursive(child, hoveredElements);
	}
}

void CollectHoveredElements(
	const std::vector<UIElement::Ptr>& hoveredPath,
	std::unordered_set<std::string>& outHoveredIds,
	std::unordered_map<std::string, UIElement::Ptr>& outHoveredRefs)
{
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const UIElement::Ptr& element : hoveredPath)
	{
		if (element != nullptr && !element->id.empty())
		{
			outHoveredIds.insert(element->id);
			outHoveredRefs[element->id] = element;
		}
	}
}

// DispatchElementEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void DispatchElementEvent(UIElement& element, UIElement::UIEventCallback callback, const glm::vec2& pointerPosition, std::string_view eventName)
{
	if (callback == nullptr)
	{
		return;
	}

	TERRAENGINE_LOG(LogLevel::Debug, "[UI] Dispatching " << eventName << " for element '" << element.id << "'.");
	callback(element, pointerPosition);
}
}

// UISystem::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UISystem& UISystem::Instance()
{
	static UISystem instance;
	return instance;
}

// UISystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UISystem::Initialize()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	documents_.clear();
	pendingImmediateQuads_.clear();
	combinedBatch_ = {};
	hoveredElementRefs_.clear();
	interactionFrame_ = 0;
	framePreparationDiagnostics_ = {};
	lastPreparationDiagnostics_ = {};
	initialized_ = true;
}

// UISystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UISystem::Update(float deltaTime)
{
	TERRAENGINE_PROFILE_SCOPE("UISystem::Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!initialized_)
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		framePreparationDiagnostics_ = {};
		framePreparationDiagnostics_.enabled = preparationDiagnosticsRequested_
			|| TerraEngine::Scheduling::EcsGraphFrameDiagnosticsEnabled();
	}

	{
		TERRAENGINE_PROFILE_SCOPE("UISystem: Integrate Ready Builds", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		IntegrateReadyBuilds();
	}

	const glm::vec2 viewportSize = GetFallbackViewportSize();
	PointerState pointerState;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hasEditorGameViewPointer_)
		{
			pointerState = editorGameViewPointer_;
			// The editor feeds one viewport-local pointer snapshot per frame from
			// GameViewWindow. Clearing the flag here prevents stale hover/click
			// state from sticking around when the Game View is hidden.
			hasEditorGameViewPointer_ = false;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!pointerState.insideViewport && ScriptingSystem::Instance().IsExportMode())
	{
		// Standalone exports use the window-space cursor directly because there
		// is no editor viewport layer to translate the mouse into local space.
		const auto [cursorX, cursorY] = InputSystem::Instance().GetCursorPosition();
		pointerState.position = glm::vec2(static_cast<float>(cursorX), static_cast<float>(cursorY));
		pointerState.insideViewport = pointerState.position.x >= 0.0f
			&& pointerState.position.y >= 0.0f
			&& pointerState.position.x <= viewportSize.x
			&& pointerState.position.y <= viewportSize.y;
		pointerState.leftButtonDown = InputSystem::Instance().IsMouseButtonPressed(MouseButton::Left);
	}

	std::vector<std::pair<entt::entity, UIComponent*>> liveComponents;
	{
		TERRAENGINE_PROFILE_SCOPE("UISystem: ECS UI Collection Graph", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const bool diagnosticsEnabled = TerraEngine::Scheduling::EcsGraphFrameDiagnosticsEnabled();
		TerraEngine::Scheduling::EcsQueryTaskGraph uiCollectionGraph({ "ui-system-collection", "UI System Collection", false, diagnosticsEnabled });
		uiCollectionGraph.AddQuery(MakeProtectedUiQuery(
			diagnosticsEnabled,
			[&liveComponents](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				TERRAENGINE_PROFILE_SCOPE("UISystem: Collect UI Components", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				ObjectManager::instance().forEachObjectWithComponents<UIComponent>([&](Object object, UIComponent& component) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (component.IsVisible())
					{
						liveComponents.emplace_back(object.EntityId(), &component);
					}
				});
			}));
		const TerraEngine::Scheduling::EcsSystemGraphFrameSummary& graphSummary = uiCollectionGraph.Execute();
		if (diagnosticsEnabled)
		{
			TerraEngine::Scheduling::PublishEcsGraphFrameSummary(graphSummary);
		}
		if (!graphSummary.valid)
		{
			TERRAENGINE_LOG(LogLevel::Error, TerraEngine::Scheduling::FormatEcsSystemGraphFrameSummary(graphSummary));
		}
	}

	{
		TERRAENGINE_PROFILE_SCOPE("UISystem: Update Documents and Input", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::lock_guard<std::mutex> lock(stateMutex_);

		std::unordered_set<entt::entity> liveEntities;
		liveEntities.reserve(liveComponents.size());
		if (framePreparationDiagnostics_.enabled)
		{
			framePreparationDiagnostics_.documentsVisited = liveComponents.size();
		}
		std::unordered_set<std::string> nextClickedElementIds;
		std::unordered_set<std::string> nextHoveredElementIds;
		std::unordered_map<std::string, UIElement::Ptr> nextHoveredElementRefs;
		const bool leftButtonClickedThisFrame = pointerState.insideViewport
			&& pointerState.leftButtonDown
			&& !previousLeftButtonDown_;
		bool pointerConsumedByDocument = false;

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto componentIterator = liveComponents.rbegin(); componentIterator != liveComponents.rend(); ++componentIterator)
		{
			const auto& [entity, component] = *componentIterator;
			liveEntities.insert(entity);
			UIDocumentState& state = documents_[entity];
			const std::uint64_t componentVersion = component->GetContentVersion();

			if (state.buildInFlight && state.contentVersion != componentVersion)
			{
				// Newer authored content supersedes the currently running worker
				// result. The old task may still finish, but its request version
				// can no longer match this document state at publish time.
				ScheduleDocumentBuild(entity, *component, state, viewportSize);
				continue;
			}

			bool animationChanged = false;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (state.root)
			{
				std::vector<UIElement::Ptr> hoveredPath;
				UIElement::Ptr hitElement;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!pointerConsumedByDocument && pointerState.insideViewport)
				{
					pointerConsumedByDocument = BuildHoveredElementPath(state.root, pointerState.position, hoveredPath, hitElement);
				}

				std::unordered_set<const UIElement*> hoveredElements;
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (const UIElement::Ptr& hoveredElement : hoveredPath)
				{
					if (hoveredElement)
					{
						hoveredElements.insert(hoveredElement.get());
					}
				}
				ApplyHoverStateRecursive(state.root, hoveredElements);
				animationChanged = state.root->AdvanceAnimationRecursive(deltaTime);

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (hitElement != nullptr)
				{
					CollectHoveredElements(hoveredPath, nextHoveredElementIds, nextHoveredElementRefs);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (leftButtonClickedThisFrame && !hitElement->id.empty())
					{
						nextClickedElementIds.insert(hitElement->id);
						DispatchElementEvent(*hitElement, hitElement->OnClick, pointerState.position, "click");
					}
				}
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (state.root && !state.buildInFlight && (animationChanged || state.root->dirty))
			{
				// Managed runtime UI mutations mark the tree dirty in place, so the
				// system must recompute Yoga layout and rebuild the draw batch even
				// when no authored asset file changed this frame.
				{
					TERRAENGINE_PROFILE_SCOPE("UISystem: Recompute Dirty Layout", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					layoutEngine_.ComputeLayout(state.root, viewportSize);
				}
				state.root->ClearDirtyRecursive();
				{
					TERRAENGINE_PROFILE_SCOPE("UISystem: Rebuild Dirty Batch", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					state.batch = renderer_.BuildBatch(state.root, viewportSize);
				}
				if (framePreparationDiagnostics_.enabled)
				{
					++framePreparationDiagnostics_.inlinePreparations;
					framePreparationDiagnostics_.estimatedElementsProcessed += CountElementTree(state.root);
				}
			}

			const bool viewportChanged = std::abs(state.viewportSize.x - viewportSize.x) > 0.1f ||
				std::abs(state.viewportSize.y - viewportSize.y) > 0.1f;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!state.buildInFlight && state.root && state.contentVersion == component->GetContentVersion() && viewportChanged)
			{
				// Window and Game View resizes only need a fresh Yoga layout and
				// batch rebuild. Re-reading the authored files on every resize was
				// pure churn and caused avoidable stalls in both the editor and
				// exported runtime.
				{
					TERRAENGINE_PROFILE_SCOPE("UISystem: Recompute Resized Layout", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					layoutEngine_.ComputeLayout(state.root, viewportSize);
				}
				state.root->ClearDirtyRecursive();
				{
					TERRAENGINE_PROFILE_SCOPE("UISystem: Rebuild Resized Batch", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					state.batch = renderer_.BuildBatch(state.root, viewportSize);
				}
				state.viewportSize = viewportSize;
				if (framePreparationDiagnostics_.enabled)
				{
					++framePreparationDiagnostics_.inlinePreparations;
					framePreparationDiagnostics_.estimatedElementsProcessed += CountElementTree(state.root);
				}
				continue;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!state.buildInFlight
				&& ((state.root == nullptr && state.lastError.empty())
					|| state.contentVersion != component->GetContentVersion()
					|| viewportChanged))
			{
				ScheduleDocumentBuild(entity, *component, state, viewportSize);
			}
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto iterator = documents_.begin(); iterator != documents_.end();)
		{
			if (!liveEntities.contains(iterator->first))
			{
				iterator = documents_.erase(iterator);
			}
			else
			{
				++iterator;
			}
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& [hoveredId, hoveredElement] : nextHoveredElementRefs)
		{
			if (!hoveredElementIds_.contains(hoveredId) && hoveredElement)
			{
				DispatchElementEvent(*hoveredElement, hoveredElement->OnMouseEnter, pointerState.position, "mouse-enter");
			}
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& [hoveredId, hoveredElement] : hoveredElementRefs_)
		{
			if (!nextHoveredElementIds.contains(hoveredId) && hoveredElement)
			{
				DispatchElementEvent(*hoveredElement, hoveredElement->OnMouseLeave, pointerState.position, "mouse-leave");
			}
		}

		clickedElementIds_ = std::move(nextClickedElementIds);
		hoveredElementIds_ = std::move(nextHoveredElementIds);
		hoveredElementRefs_ = std::move(nextHoveredElementRefs);
		++interactionFrame_;
		previousLeftButtonDown_ = pointerState.insideViewport && pointerState.leftButtonDown;
	}

	{
		TERRAENGINE_PROFILE_SCOPE("UISystem: Refresh Combined Batch", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RefreshCombinedBatch();
	}

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		lastPreparationDiagnostics_ = framePreparationDiagnostics_;
	}
}

// UISystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UISystem::Shutdown()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	++shutdownGeneration_;
	documents_.clear();
	pendingImmediateQuads_.clear();
	combinedBatch_ = {};
	hoveredElementRefs_.clear();
	framePreparationDiagnostics_ = {};
	initialized_ = false;
}

// UISystem::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string UISystem::SystemName()
{
	return "UISystem";
}

// UISystem::SetGameViewportSize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UISystem::SetGameViewportSize(const glm::vec2& viewportSize)
{
	if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(stateMutex_);
	gameViewportSize_ = viewportSize;
}

// UISystem::SetEditorGameViewPointer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UISystem::SetEditorGameViewPointer(const glm::vec2& pointerPosition, bool pointerInsideViewport, bool leftButtonDown)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	editorGameViewPointer_.position = pointerPosition;
	editorGameViewPointer_.insideViewport = pointerInsideViewport;
	editorGameViewPointer_.leftButtonDown = leftButtonDown;
	hasEditorGameViewPointer_ = true;
}

// UISystem::SubmitImmediateQuad is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UISystem::SubmitImmediateQuad(const UIImmediateQuad& quad)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	pendingImmediateQuads_.push_back(quad);
}

// UISystem::GetCombinedFrameBatch is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIRenderBatch UISystem::GetCombinedFrameBatch() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return combinedBatch_;
}

// UISystem::WasElementClicked is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UISystem::WasElementClicked(std::string_view elementId) const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return clickedElementIds_.contains(std::string(elementId));
}

// UISystem::IsElementHovered is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UISystem::IsElementHovered(std::string_view elementId) const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return hoveredElementIds_.contains(std::string(elementId));
}

// UISystem::GetInteractionFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::uint64_t UISystem::GetInteractionFrame() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return interactionFrame_;
}

UISystem::UIPreparationDiagnostics UISystem::GetLastPreparationDiagnostics() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return lastPreparationDiagnostics_;
}

void UISystem::SetPreparationDiagnosticsRequested(bool requested)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	preparationDiagnosticsRequested_ = requested;
	if (!requested)
	{
		framePreparationDiagnostics_ = {};
		lastPreparationDiagnostics_ = {};
	}
	else
	{
		// Let diagnostics windows show that collection is armed even before the
		// next frame publishes a fresh per-frame summary.
		lastPreparationDiagnostics_.enabled = true;
	}
}

std::size_t UISystem::CountElementTree(const UIElement::Ptr& root)
{
	if (!root)
	{
		return 0;
	}

	std::size_t count = 1;
	for (const UIElement::Ptr& child : root->children)
	{
		count += CountElementTree(child);
	}
	return count;
}

bool UISystem::ShouldPrepareOnWorker(const UIPreparationRequest& request) const
{
	constexpr std::size_t kWorkerElementThreshold = 128;
	constexpr std::size_t kWorkerStyleRuleThreshold = 64;

	// Authored source builds do not know their final tree size until parsing
	// completes. Treat non-empty source paths as worker candidates unless a
	// caller has already provided a small cloned tree estimate.
	if (!request.sourceMarkup.empty() && request.estimatedElementCount == 0)
	{
		return true;
	}

	return request.estimatedElementCount >= kWorkerElementThreshold
		|| request.estimatedStyleRuleCount >= kWorkerStyleRuleThreshold;
}

UISystem::UIPublishStatus UISystem::ClassifyPreparedResult(
	const UIBuildResult& result,
	std::uint64_t currentRequestVersion,
	std::uint64_t currentContentVersion,
	std::uint64_t currentShutdownGeneration)
{
	// Worker results are versioned because they can finish after newer UI data
	// has been queued. Classification is kept pure so tests and publish code use
	// the same deterministic stale/failure/shutdown rules.
	if (result.shutdownGeneration != currentShutdownGeneration)
	{
		return UIPublishStatus::ShutdownSkipped;
	}

	if (result.requestVersion != currentRequestVersion || result.contentVersion != currentContentVersion)
	{
		return UIPublishStatus::DiscardedStale;
	}

	if (!result.error.empty() || !result.root)
	{
		return UIPublishStatus::FailedPreservedPrevious;
	}

	return UIPublishStatus::Published;
}

// UISystem::EnsureDocumentReady is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UISystem::EnsureDocumentReady(entt::entity entity, UIComponent& component)
{
	const glm::vec2 viewportSize = GetFallbackViewportSize();

	auto publishBuildResult = [&](UIBuildResult buildResult) {
		std::lock_guard<std::mutex> lock(stateMutex_);
		UIDocumentState& state = documents_[entity];
		state.buildInFlight = false;
		if (state.requestVersion < buildResult.requestVersion)
		{
			state.requestVersion = buildResult.requestVersion;
		}
		state.contentVersion = component.GetContentVersion();
		if (buildResult.requestVersion == 0)
		{
			buildResult.requestVersion = state.requestVersion;
		}
		if (buildResult.contentVersion == 0)
		{
			buildResult.contentVersion = state.contentVersion;
		}
		buildResult.entity = entity;
		buildResult.shutdownGeneration = state.shutdownGeneration;
		return PublishPreparedResult(entity, state, buildResult);
	};

	std::shared_future<UIBuildResult> pendingBuild;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		UIDocumentState& state = documents_[entity];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (state.root != nullptr && state.contentVersion == component.GetContentVersion())
		{
			component.SetRoot(state.root);
			return true;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (state.buildInFlight && state.buildFuture.valid())
		{
			pendingBuild = state.buildFuture;
		}
	}

	// Exported gameplay scripts can ask for authored controls in OnStart before
	// UISystem::Update has integrated the asynchronous build. Wait for the
	// in-flight document or build it synchronously so root.Q("...") resolves
	// against the authored tree instead of an empty placeholder element.
	if (pendingBuild.valid())
	{
		return publishBuildResult(pendingBuild.get());
	}

	return publishBuildResult(BuildDocumentFromSources(
		component.GetSourceMarkup(),
		component.GetSourceStylePaths(),
		component.GetRoot(),
		viewportSize));
}

// UISystem::BuildPreviewDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UISystem::UIBuildResult UISystem::BuildPreviewDocument(const UIElement::Ptr& root, const glm::vec2& viewportSize) const
{
	UIPreparationRequest request;
	request.viewportSize = viewportSize;
	request.existingRootSnapshot = root ? root->Clone() : nullptr;
	request.reason = UIPreparationReason::PreviewBuild;
	request.estimatedElementCount = CountElementTree(request.existingRootSnapshot);
	return BuildPreparedDocument(request);
}

// UISystem::BuildPreviewBatch is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIRenderBatch UISystem::BuildPreviewBatch(const UIElement::Ptr& root, const glm::vec2& viewportSize) const
{
	return BuildPreviewDocument(root, viewportSize).batch;
}

// UISystem::SerializeMarkup is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string UISystem::SerializeMarkup(const UIElement::Ptr& root) const
{
	return parser_.SerializeMarkup(root);
}

// UISystem::SerializeStyles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string UISystem::SerializeStyles(const UIElement::Ptr& root) const
{
	return parser_.SerializeStyles(root);
}

UISystem::UIBuildResult UISystem::BuildDocumentFromSources(
	const std::string& markupPath,
	const std::vector<std::string>& stylePaths,
	const UIElement::Ptr& existingRoot,
	const glm::vec2& viewportSize) const
{
	UIPreparationRequest request;
	request.viewportSize = viewportSize;
	request.sourceMarkup = markupPath;
	request.styleSources = stylePaths;
	request.existingRootSnapshot = existingRoot ? existingRoot->Clone() : nullptr;
	request.estimatedElementCount = CountElementTree(request.existingRootSnapshot);
	return BuildPreparedDocument(request);
}

UISystem::UIBuildResult UISystem::BuildPreparedDocument(const UIPreparationRequest& request) const
{
	TERRAENGINE_PROFILE_SCOPE("UISystem: Build Document From Sources", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	UIBuildResult result;
	result.entity = request.entity;
	result.requestVersion = request.requestVersion;
	result.contentVersion = request.contentVersion;
	result.shutdownGeneration = request.shutdownGeneration;
	result.viewportSize = request.viewportSize;
	result.reason = request.reason;
	result.estimatedElementCount = request.estimatedElementCount;

	try
	{
		auto resolvePath = [](const std::string& pathText) -> std::filesystem::path {
			std::filesystem::path path(pathText);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (path.is_absolute())
			{
				return path;
			}
			return ResourceManager::Instance().GetAssetRoot() / path;
		};

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!request.sourceMarkup.empty())
		{
			TERRAENGINE_PROFILE_SCOPE("UISystem: Parse Markup", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			result.root = parser_.ParseMarkupFile(resolvePath(request.sourceMarkup));
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (request.existingRootSnapshot)
		{
			result.root = request.existingRootSnapshot->Clone();
		}
		else
		{
			result.root = UIElement::Create(UIElementType::Root);
			result.root->typeName = "UIRoot";
		}

		std::vector<std::string> resolvedStylePaths = request.styleSources;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (resolvedStylePaths.empty() && result.root)
		{
			resolvedStylePaths = result.root->styleSheetSources;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (result.root && !resolvedStylePaths.empty())
		{
			result.root->styleSheetSources = resolvedStylePaths;

			std::vector<UIStyleRule> combinedRules;
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const std::string& resolvedStylePath : resolvedStylePaths)
			{
				TERRAENGINE_PROFILE_SCOPE("UISystem: Parse Style", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				std::vector<UIStyleRule> styleRules = parser_.ParseStyleFile(resolvePath(resolvedStylePath));
				const std::size_t orderOffset = combinedRules.size();
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (UIStyleRule& rule : styleRules)
				{
					rule.order += orderOffset;
					combinedRules.push_back(std::move(rule));
				}
			}

			parser_.ApplyStyles(result.root, combinedRules);
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (result.root)
		{
			result.root->styleSheetSources.clear();
			parser_.ApplyStyles(result.root, {});
		}

		{
			TERRAENGINE_PROFILE_SCOPE("UISystem: Compute Document Layout", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			layoutEngine_.ComputeLayout(result.root, request.viewportSize);
		}
		result.root->ClearDirtyRecursive();
		{
			TERRAENGINE_PROFILE_SCOPE("UISystem: Build Document Batch", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			result.batch = renderer_.BuildBatch(result.root, request.viewportSize);
		}
		result.estimatedElementCount = CountElementTree(result.root);
	}
	catch (const std::exception& exception)
	{
		result.error = exception.what();
		TERRAENGINE_LOG(LogLevel::Error, "UISystem failed to build a document: " << result.error);
	}

	return result;
}

// UISystem::ScheduleDocumentBuild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UISystem::ScheduleDocumentBuild(entt::entity entity, UIComponent& component, UIDocumentState& state, const glm::vec2& viewportSize)
{
	TERRAENGINE_PROFILE_SCOPE("UISystem: Create Preparation Request", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	UIPreparationReason reason = state.root == nullptr ? UIPreparationReason::InitialLoad : UIPreparationReason::AuthoredReload;
	const bool viewportChanged = std::abs(state.viewportSize.x - viewportSize.x) > 0.1f ||
		std::abs(state.viewportSize.y - viewportSize.y) > 0.1f;
	if (viewportChanged && state.contentVersion == component.GetContentVersion())
	{
		reason = UIPreparationReason::ViewportResize;
	}

	UIPreparationRequest request = CreatePreparationRequest(entity, component, state, viewportSize, reason);
	const bool useWorker = ShouldPrepareOnWorker(request);
	RecordPreparationRequest(request, useWorker ? UIPreparationMode::Worker : UIPreparationMode::Inline);

	if (!useWorker)
	{
		TERRAENGINE_PROFILE_SCOPE("UISystem: Inline Preparation", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		UIBuildResult result = BuildPreparedDocument(request);
		result.mode = UIPreparationMode::Inline;
		state.buildInFlight = false;
		PublishPreparedResult(entity, state, result);
		return;
	}

	auto task = ThreadPool::instance().createTask([this, request]() {
		TERRAENGINE_PROFILE_SCOPE("UISystem: Worker Preparation", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		UIBuildResult result = BuildPreparedDocument(request);
		result.mode = UIPreparationMode::Worker;
		return result;
	});

	state.buildFuture = task->getFuture().share();
	state.buildInFlight = true;
	state.contentVersion = request.contentVersion;
	state.viewportSize = viewportSize;
	state.lastPreparationMode = UIPreparationMode::Worker;
	ThreadPool::instance().enqueue(task);
}

UISystem::UIPreparationRequest UISystem::CreatePreparationRequest(entt::entity entity, const UIComponent& component, UIDocumentState& state, const glm::vec2& viewportSize, UIPreparationReason reason) const
{
	UIPreparationRequest request;
	request.entity = entity;
	request.requestVersion = ++state.requestVersion;
	request.contentVersion = component.GetContentVersion();
	request.shutdownGeneration = shutdownGeneration_;
	request.viewportSize = viewportSize;
	request.sourceMarkup = component.GetSourceMarkup();
	request.styleSources = component.GetSourceStylePaths();
	request.existingRootSnapshot = component.GetRoot() ? component.GetRoot()->Clone() : nullptr;
	request.reason = reason;
	request.estimatedElementCount = CountElementTree(request.existingRootSnapshot);
	request.estimatedStyleRuleCount = request.styleSources.size();
	state.contentVersion = request.contentVersion;
	state.shutdownGeneration = request.shutdownGeneration;
	return request;
}

bool UISystem::PublishPreparedResult(entt::entity entity, UIDocumentState& state, const UIBuildResult& result)
{
	TERRAENGINE_PROFILE_SCOPE("UISystem: Publish Prepared Result", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	const UIPublishStatus status = ClassifyPreparedResult(result, state.requestVersion, state.contentVersion, state.shutdownGeneration);

	if (status == UIPublishStatus::FailedPreservedPrevious)
	{
		state.lastError = result.error;
		// UIComponent clears its runtime tree when a new authored source path is
		// assigned. If that source fails to parse, restore the last valid tree so
		// scripts and visible UI keep seeing the same document as the batch.
		if (auto object = ObjectManager::instance().getObject(entity))
		{
			if (UIComponent* component = object->getComponent<UIComponent>())
			{
				component->SetRoot(state.root);
			}
		}
	}
	else if (status == UIPublishStatus::Published)
	{
		state.root = result.root;
		state.batch = result.batch;
		state.viewportSize = result.viewportSize;
		state.publishedVersion = result.requestVersion;
		state.lastError.clear();

		if (auto object = ObjectManager::instance().getObject(entity))
		{
			if (UIComponent* component = object->getComponent<UIComponent>())
			{
				component->SetRoot(result.root);
			}
		}
	}

	state.buildInFlight = false;
	state.lastPreparationMode = result.mode;
	state.lastPublishStatus = status;
	RecordPublishStatus(status);
	return status == UIPublishStatus::Published;
}

void UISystem::RecordPreparationRequest(const UIPreparationRequest& request, UIPreparationMode mode)
{
	if (!framePreparationDiagnostics_.enabled)
	{
		return;
	}

	++framePreparationDiagnostics_.requestsScheduled;
	framePreparationDiagnostics_.estimatedElementsProcessed += request.estimatedElementCount;
	if (mode == UIPreparationMode::Worker)
	{
		++framePreparationDiagnostics_.workerPreparations;
	}
	else
	{
		++framePreparationDiagnostics_.inlinePreparations;
	}
}

void UISystem::RecordPublishStatus(UIPublishStatus status)
{
	if (!framePreparationDiagnostics_.enabled)
	{
		return;
	}

	framePreparationDiagnostics_.lastPublishStatus = status;
	switch (status)
	{
	case UIPublishStatus::Published:
		++framePreparationDiagnostics_.publishedResults;
		break;
	case UIPublishStatus::DiscardedStale:
		++framePreparationDiagnostics_.staleResultsDiscarded;
		break;
	case UIPublishStatus::FailedPreservedPrevious:
		++framePreparationDiagnostics_.failedResultsPreserved;
		break;
	default:
		break;
	}
}

// UISystem::IntegrateReadyBuilds is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UISystem::IntegrateReadyBuilds()
{
	std::lock_guard<std::mutex> lock(stateMutex_);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& [entity, state] : documents_)
	{
		if (!state.buildInFlight || !state.buildFuture.valid())
		{
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (state.buildFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			continue;
		}

		UIBuildResult result = state.buildFuture.get();
		if (framePreparationDiagnostics_.enabled)
		{
			++framePreparationDiagnostics_.readyResultsIntegrated;
		}
		PublishPreparedResult(entity, state, result);
	}
}

// UISystem::RefreshCombinedBatch is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UISystem::RefreshCombinedBatch()
{
	std::lock_guard<std::mutex> lock(stateMutex_);

	UIRenderBatch nextBatch;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	std::vector<entt::entity> sortedEntities;
	sortedEntities.reserve(documents_.size());
	for (const auto& [entity, state] : documents_)
	{
		(void)entity;
		(void)state;
		sortedEntities.push_back(entity);
	}
	std::sort(sortedEntities.begin(), sortedEntities.end(), [](entt::entity left, entt::entity right) {
		return static_cast<std::uint32_t>(left) < static_cast<std::uint32_t>(right);
	});
	for (entt::entity entity : sortedEntities)
	{
		const UIDocumentState& state = documents_.at(entity);
		nextBatch.Append(state.batch);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (nextBatch.viewportSize.x <= 0.0f || nextBatch.viewportSize.y <= 0.0f)
	{
		nextBatch.viewportSize = gameViewportSize_;
	}

	nextBatch.immediateQuads.insert(nextBatch.immediateQuads.end(), pendingImmediateQuads_.begin(), pendingImmediateQuads_.end());
	pendingImmediateQuads_.clear();
	combinedBatch_ = std::move(nextBatch);
}

// UISystem::GetFallbackViewportSize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
glm::vec2 UISystem::GetFallbackViewportSize() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return (gameViewportSize_.x > 0.0f && gameViewportSize_.y > 0.0f)
		? gameViewportSize_
		: glm::vec2(1280.0f, 720.0f);
}
