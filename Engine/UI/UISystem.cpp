#include "UISystem.h"

#include "Components/UIComponent.h"
#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Input/InputCodes.h"
#include "Input/InputSystem.h"
#include "Resource/ResourceManager.h"
#include "Scripting/ScriptingSystem.h"
#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <unordered_set>
#include <utility>

namespace
{
glm::vec2 GetInteractiveElementMin(const UIElement& element)
{
	// Runtime hit testing must respect the same transform-driven translate
	// offset the preview renderer uses, otherwise hover/click targeting drifts
	// away from the actual drawn element.
	const glm::vec2 translate = element.displayStyle.transform.translate.value_or(glm::vec2(0.0f, 0.0f));
	return element.calculatedPosition + translate;
}

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
	if (!pointerInsideElement)
	{
		return false;
	}

	hoveredPath.push_back(element);
	for (std::size_t reverseIndex = element->children.size(); reverseIndex > 0; --reverseIndex)
	{
		const UIElement::Ptr& child = element->children[reverseIndex - 1];
		if (BuildHoveredElementPath(child, localPointerPosition, hoveredPath, outHitElement))
		{
			return true;
		}
	}

	outHitElement = element;
	return true;
}

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
	for (const UIElement::Ptr& element : hoveredPath)
	{
		if (element != nullptr && !element->id.empty())
		{
			outHoveredIds.insert(element->id);
			outHoveredRefs[element->id] = element;
		}
	}
}

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

UISystem& UISystem::Instance()
{
	static UISystem instance;
	return instance;
}

void UISystem::Initialize()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	documents_.clear();
	pendingImmediateQuads_.clear();
	combinedBatch_ = {};
	hoveredElementRefs_.clear();
	interactionFrame_ = 0;
	initialized_ = true;
}

void UISystem::Update(float deltaTime)
{
	if (!initialized_)
	{
		return;
	}

	IntegrateReadyBuilds();

	const glm::vec2 viewportSize = GetFallbackViewportSize();
	PointerState pointerState;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (hasEditorGameViewPointer_)
		{
			pointerState = editorGameViewPointer_;
			// The editor feeds one viewport-local pointer snapshot per frame from
			// GameViewWindow. Clearing the flag here prevents stale hover/click
			// state from sticking around when the Game View is hidden.
			hasEditorGameViewPointer_ = false;
		}
	}

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
	ObjectManager::instance().forEachObjectWithComponents<UIComponent>([&](Object object, UIComponent& component) {
		if (component.IsVisible())
		{
			liveComponents.emplace_back(object.EntityId(), &component);
		}
	});

	{
		std::lock_guard<std::mutex> lock(stateMutex_);

		std::unordered_set<entt::entity> liveEntities;
		liveEntities.reserve(liveComponents.size());
		std::unordered_set<std::string> nextClickedElementIds;
		std::unordered_set<std::string> nextHoveredElementIds;
		std::unordered_map<std::string, UIElement::Ptr> nextHoveredElementRefs;
		const bool leftButtonClickedThisFrame = pointerState.insideViewport
			&& pointerState.leftButtonDown
			&& !previousLeftButtonDown_;
		bool pointerConsumedByDocument = false;

		for (auto componentIterator = liveComponents.rbegin(); componentIterator != liveComponents.rend(); ++componentIterator)
		{
			const auto& [entity, component] = *componentIterator;
			liveEntities.insert(entity);
			UIDocumentState& state = documents_[entity];

			bool animationChanged = false;
			if (state.root)
			{
				std::vector<UIElement::Ptr> hoveredPath;
				UIElement::Ptr hitElement;
				if (!pointerConsumedByDocument && pointerState.insideViewport)
				{
					pointerConsumedByDocument = BuildHoveredElementPath(state.root, pointerState.position, hoveredPath, hitElement);
				}

				std::unordered_set<const UIElement*> hoveredElements;
				for (const UIElement::Ptr& hoveredElement : hoveredPath)
				{
					if (hoveredElement)
					{
						hoveredElements.insert(hoveredElement.get());
					}
				}
				ApplyHoverStateRecursive(state.root, hoveredElements);
				animationChanged = state.root->AdvanceAnimationRecursive(deltaTime);

				if (hitElement != nullptr)
				{
					CollectHoveredElements(hoveredPath, nextHoveredElementIds, nextHoveredElementRefs);
					if (leftButtonClickedThisFrame && !hitElement->id.empty())
					{
						nextClickedElementIds.insert(hitElement->id);
						DispatchElementEvent(*hitElement, hitElement->OnClick, pointerState.position, "click");
					}
				}
			}

			if (state.root && !state.buildInFlight && (animationChanged || state.root->dirty))
			{
				// Managed runtime UI mutations mark the tree dirty in place, so the
				// system must recompute Yoga layout and rebuild the draw batch even
				// when no authored asset file changed this frame.
				layoutEngine_.ComputeLayout(state.root, viewportSize);
				state.root->ClearDirtyRecursive();
				state.batch = renderer_.BuildBatch(state.root, viewportSize);
			}

			const bool viewportChanged = std::abs(state.viewportSize.x - viewportSize.x) > 0.1f ||
				std::abs(state.viewportSize.y - viewportSize.y) > 0.1f;
			if (!state.buildInFlight && state.root && state.contentVersion == component->GetContentVersion() && viewportChanged)
			{
				// Window and Game View resizes only need a fresh Yoga layout and
				// batch rebuild. Re-reading the authored files on every resize was
				// pure churn and caused avoidable stalls in both the editor and
				// exported runtime.
				layoutEngine_.ComputeLayout(state.root, viewportSize);
				state.root->ClearDirtyRecursive();
				state.batch = renderer_.BuildBatch(state.root, viewportSize);
				state.viewportSize = viewportSize;
				continue;
			}

			if (!state.buildInFlight
				&& ((state.root == nullptr && state.lastError.empty())
					|| state.contentVersion != component->GetContentVersion()
					|| viewportChanged))
			{
				ScheduleDocumentBuild(entity, *component, state, viewportSize);
			}
		}

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

		for (const auto& [hoveredId, hoveredElement] : nextHoveredElementRefs)
		{
			if (!hoveredElementIds_.contains(hoveredId) && hoveredElement)
			{
				DispatchElementEvent(*hoveredElement, hoveredElement->OnMouseEnter, pointerState.position, "mouse-enter");
			}
		}

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

	RefreshCombinedBatch();
}

void UISystem::Shutdown()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	documents_.clear();
	pendingImmediateQuads_.clear();
	combinedBatch_ = {};
	hoveredElementRefs_.clear();
	initialized_ = false;
}

std::string UISystem::SystemName()
{
	return "UISystem";
}

void UISystem::SetGameViewportSize(const glm::vec2& viewportSize)
{
	if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(stateMutex_);
	gameViewportSize_ = viewportSize;
}

void UISystem::SetEditorGameViewPointer(const glm::vec2& pointerPosition, bool pointerInsideViewport, bool leftButtonDown)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	editorGameViewPointer_.position = pointerPosition;
	editorGameViewPointer_.insideViewport = pointerInsideViewport;
	editorGameViewPointer_.leftButtonDown = leftButtonDown;
	hasEditorGameViewPointer_ = true;
}

void UISystem::SubmitImmediateQuad(const UIImmediateQuad& quad)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	pendingImmediateQuads_.push_back(quad);
}

UIRenderBatch UISystem::GetCombinedFrameBatch() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return combinedBatch_;
}

bool UISystem::WasElementClicked(std::string_view elementId) const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return clickedElementIds_.contains(std::string(elementId));
}

bool UISystem::IsElementHovered(std::string_view elementId) const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return hoveredElementIds_.contains(std::string(elementId));
}

std::uint64_t UISystem::GetInteractionFrame() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return interactionFrame_;
}

bool UISystem::EnsureDocumentReady(entt::entity entity, UIComponent& component)
{
	const glm::vec2 viewportSize = GetFallbackViewportSize();

	auto publishBuildResult = [&](UIBuildResult buildResult) {
		std::lock_guard<std::mutex> lock(stateMutex_);
		UIDocumentState& state = documents_[entity];
		state.buildInFlight = false;
		state.contentVersion = component.GetContentVersion();
		state.viewportSize = viewportSize;
		state.lastError = buildResult.error;

		if (buildResult.root)
		{
			state.root = buildResult.root;
			state.batch = buildResult.batch;
			component.SetRoot(buildResult.root);
			return true;
		}

		state.root.reset();
		state.batch = {};
		component.ClearRuntimeTree();
		return false;
	};

	std::shared_future<UIBuildResult> pendingBuild;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		UIDocumentState& state = documents_[entity];
		if (state.root != nullptr && state.contentVersion == component.GetContentVersion())
		{
			component.SetRoot(state.root);
			return true;
		}

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
		{},
		component.GetRoot(),
		viewportSize));
}

UISystem::UIBuildResult UISystem::BuildPreviewDocument(const UIElement::Ptr& root, const glm::vec2& viewportSize) const
{
	UIBuildResult result;
	if (!root)
	{
		return result;
	}

	result.root = root->Clone();
	layoutEngine_.ComputeLayout(result.root, viewportSize);
	result.root->ClearDirtyRecursive();
	result.batch = renderer_.BuildBatch(result.root, viewportSize);
	return result;
}

UIRenderBatch UISystem::BuildPreviewBatch(const UIElement::Ptr& root, const glm::vec2& viewportSize) const
{
	return BuildPreviewDocument(root, viewportSize).batch;
}

std::string UISystem::SerializeMarkup(const UIElement::Ptr& root) const
{
	return parser_.SerializeMarkup(root);
}

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
	UIBuildResult result;

	try
	{
		auto resolvePath = [](const std::string& pathText) -> std::filesystem::path {
			std::filesystem::path path(pathText);
			if (path.is_absolute())
			{
				return path;
			}
			return ResourceManager::Instance().GetAssetRoot() / path;
		};

		if (!markupPath.empty())
		{
			result.root = parser_.ParseMarkupFile(resolvePath(markupPath));
		}
		else if (existingRoot)
		{
			result.root = existingRoot->Clone();
		}
		else
		{
			result.root = UIElement::Create(UIElementType::Root);
			result.root->typeName = "UIRoot";
		}

		std::vector<std::string> resolvedStylePaths = stylePaths;
		if (resolvedStylePaths.empty() && result.root)
		{
			resolvedStylePaths = result.root->styleSheetSources;
		}

		if (result.root && !resolvedStylePaths.empty())
		{
			result.root->styleSheetSources = resolvedStylePaths;

			std::vector<UIStyleRule> combinedRules;
			for (const std::string& resolvedStylePath : resolvedStylePaths)
			{
				std::vector<UIStyleRule> styleRules = parser_.ParseStyleFile(resolvePath(resolvedStylePath));
				const std::size_t orderOffset = combinedRules.size();
				for (UIStyleRule& rule : styleRules)
				{
					rule.order += orderOffset;
					combinedRules.push_back(std::move(rule));
				}
			}

			parser_.ApplyStyles(result.root, combinedRules);
		}
		else if (result.root)
		{
			result.root->styleSheetSources.clear();
			parser_.ApplyStyles(result.root, {});
		}

		layoutEngine_.ComputeLayout(result.root, viewportSize);
		result.root->ClearDirtyRecursive();
		result.batch = renderer_.BuildBatch(result.root, viewportSize);
	}
	catch (const std::exception& exception)
	{
		result.error = exception.what();
		TERRAENGINE_LOG(LogLevel::Error, "UISystem failed to build a document: " << result.error);
	}

	return result;
}

void UISystem::ScheduleDocumentBuild(entt::entity entity, UIComponent& component, UIDocumentState& state, const glm::vec2& viewportSize)
{
	const std::string markupPath = component.GetSourceMarkup();
	// The component no longer owns stylesheet overrides. Runtime styling comes
	// from the .tuiml-authored attachments so the builder stays authoritative.
	const std::vector<std::string> stylePaths;
	const UIElement::Ptr existingRoot = component.GetRoot();

	auto task = ThreadPool::instance().createTask([this, markupPath, stylePaths, existingRoot, viewportSize]() {
		return BuildDocumentFromSources(markupPath, stylePaths, existingRoot, viewportSize);
	});

	state.buildFuture = task->getFuture().share();
	state.buildInFlight = true;
	state.contentVersion = component.GetContentVersion();
	state.viewportSize = viewportSize;
	ThreadPool::instance().enqueue(task);
	(void)entity;
}

void UISystem::IntegrateReadyBuilds()
{
	std::lock_guard<std::mutex> lock(stateMutex_);

	for (auto& [entity, state] : documents_)
	{
		if (!state.buildInFlight || !state.buildFuture.valid())
		{
			continue;
		}

		if (state.buildFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			continue;
		}

		UIBuildResult result = state.buildFuture.get();
		state.buildInFlight = false;
		state.lastError = result.error;

		if (result.root)
		{
			state.root = result.root;
			state.batch = result.batch;

			if (auto object = ObjectManager::instance().getObject(entity))
			{
				if (UIComponent* component = object->getComponent<UIComponent>())
				{
					component->SetRoot(result.root);
				}
			}
		}
		else
		{
			state.root.reset();
			state.batch = {};

			if (auto object = ObjectManager::instance().getObject(entity))
			{
				if (UIComponent* component = object->getComponent<UIComponent>())
				{
					component->ClearRuntimeTree();
				}
			}
		}
	}
}

void UISystem::RefreshCombinedBatch()
{
	std::lock_guard<std::mutex> lock(stateMutex_);

	UIRenderBatch nextBatch;
	for (const auto& [entity, state] : documents_)
	{
		(void)entity;
		nextBatch.Append(state.batch);
	}

	if (nextBatch.viewportSize.x <= 0.0f || nextBatch.viewportSize.y <= 0.0f)
	{
		nextBatch.viewportSize = gameViewportSize_;
	}

	nextBatch.immediateQuads.insert(nextBatch.immediateQuads.end(), pendingImmediateQuads_.begin(), pendingImmediateQuads_.end());
	pendingImmediateQuads_.clear();
	combinedBatch_ = std::move(nextBatch);
}

glm::vec2 UISystem::GetFallbackViewportSize() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return (gameViewportSize_.x > 0.0f && gameViewportSize_.y > 0.0f)
		? gameViewportSize_
		: glm::vec2(1280.0f, 720.0f);
}
