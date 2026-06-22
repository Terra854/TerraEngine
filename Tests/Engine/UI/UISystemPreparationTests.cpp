#include "TestHarness.h"
#include "Components/UIComponent.h"
#include "Input/InputBackend.h"
#include "Input/InputSystem.h"
#include "Core/Object/ObjectManager.h"
#include "Resource/ResourceManager.h"
#include "Scripting/ScriptingSystem.h"
#include "UI/UISystem.h"
#include "Window/WindowBackend.h"
#include "Window/WindowSystem.h"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
namespace {
	UIElement::Ptr MakeRootWithChildren(std::size_t childCount)
	{
		UIElement::Ptr root = UIElement::Create(UIElementType::Root);
		root->typeName = "UIRoot";
		root->inlineStyle.width = UIValue::Points(800.0f);
		root->inlineStyle.height = UIValue::Points(600.0f);

		for (std::size_t index = 0; index < childCount; ++index)
		{
			UIElement::Ptr child = UIElement::Create(UIElementType::VisualElement);
			child->id = "node-" + std::to_string(index);
			child->inlineStyle.width = UIValue::Points(12.0f);
			child->inlineStyle.height = UIValue::Points(6.0f);
			child->inlineStyle.backgroundColor = glm::vec4(0.2f, 0.4f, 0.8f, 1.0f);
			root->AddChild(child);
		}

		return root;
	}

	UISystem::UIPreparationRequest MakeSnapshotRequest(const UIElement::Ptr& root)
	{
		UISystem::UIPreparationRequest request;
		request.entity = entt::entity{ 7 };
		request.requestVersion = 1;
		request.contentVersion = 1;
		request.viewportSize = glm::vec2(800.0f, 600.0f);
		request.reason = UISystem::UIPreparationReason::LiveTreeDirty;
		request.existingRootSnapshot = root ? root->Clone() : nullptr;
		request.estimatedElementCount = UISystem::CountElementTree(request.existingRootSnapshot);
		return request;
	}

	class TestWindowBackend final : public IWindowBackend
	{
	public:
		std::string BackendName() const override { return "UIRuntimeTestWindow"; }

		void Initialize(const WindowConfig&) override
		{
			// InputSystem only needs a stable non-null native handle in this
			// headless test; no real OS window is created.
			nativeHandle_ = this;
		}

		void PollEvents() override {}
		void SwapBuffers() override {}
		bool ShouldClose() const override { return false; }
		void* GetNativeHandle() const override { return nativeHandle_; }
		void SetCloseCallback(std::function<void()> onClose) override { closeCallback_ = std::move(onClose); }
		void SetResizeCallback(std::function<void(int, int)> onResize) override { resizeCallback_ = std::move(onResize); }

		void Shutdown() override
		{
			nativeHandle_ = nullptr;
			closeCallback_ = nullptr;
			resizeCallback_ = nullptr;
		}

	private:
		void* nativeHandle_ = nullptr;
		std::function<void()> closeCallback_;
		std::function<void(int, int)> resizeCallback_;
	};

	class QueuedInputBackend final : public IInputBackend
	{
	public:
		std::string BackendName() const override { return "UIRuntimeQueuedInput"; }
		void SetWindowHandle(void* windowHandle) override { windowHandle_ = windowHandle; }
		void Initialize() override {}

		void Update() override
		{
			for (const auto& [button, state] : queuedMouseEvents_)
			{
				mouseState_[static_cast<std::size_t>(button)] = state;
				if (mouseButtonCallback_)
				{
					mouseButtonCallback_(button, state);
				}
			}
			queuedMouseEvents_.clear();
		}

		void Shutdown() override
		{
			windowHandle_ = nullptr;
			queuedMouseEvents_.clear();
		}

		void SetKeyCallback(std::function<void(Key, KeyState)> onKeyEvent) override { keyCallback_ = std::move(onKeyEvent); }
		void SetMouseButtonCallback(std::function<void(MouseButton, KeyState)> onMouseButton) override { mouseButtonCallback_ = std::move(onMouseButton); }
		void SetCursorPositionCallback(std::function<void(double, double)> onCursorMove) override { cursorCallback_ = std::move(onCursorMove); }
		void SetScrollCallback(std::function<void(double, double)> onScroll) override { scrollCallback_ = std::move(onScroll); }
		bool IsKeyPressed(Key) const override { return false; }

		bool IsMouseButtonPressed(MouseButton button) const override
		{
			const std::size_t index = static_cast<std::size_t>(button);
			return index < mouseState_.size()
				&& (mouseState_[index] == KeyState::Pressed || mouseState_[index] == KeyState::Held);
		}

		std::pair<double, double> GetCursorPosition() const override { return cursorPosition_; }
		void SetCursorPosition(double x, double y) override { cursorPosition_ = { x, y }; }

		void SetHardwareCursorPosition(double x, double y)
		{
			// Export UI should use polled backend coordinates even if the cursor
			// has not generated a movement callback since startup/focus.
			cursorPosition_ = { x, y };
		}

		void QueueMouseEvent(MouseButton button, KeyState state)
		{
			queuedMouseEvents_.push_back({ button, state });
		}

	private:
		void* windowHandle_ = nullptr;
		std::function<void(Key, KeyState)> keyCallback_;
		std::function<void(MouseButton, KeyState)> mouseButtonCallback_;
		std::function<void(double, double)> cursorCallback_;
		std::function<void(double, double)> scrollCallback_;
		std::array<KeyState, MouseButtonCount()> mouseState_{};
		std::pair<double, double> cursorPosition_{ 0.0, 0.0 };
		std::vector<std::pair<MouseButton, KeyState>> queuedMouseEvents_;
	};

	void ResetRuntimeInputState()
	{
		InputSystem::Instance().Shutdown();
		WindowSystem::Instance().Shutdown();
		ResourceManager::Instance().ConfigureExportArchive({});
	}
}

void RegisterUISystemPreparationTests(TestRegistry& registry)
{
	registry.Add("Engine.UISystemPreparation.CountsClonedTreeDeterministically", [](TestContext& context) {
		const UIElement::Ptr root = MakeRootWithChildren(4);
		const UIElement::Ptr clone = root->Clone();

		context.Expect(UISystem::CountElementTree(root) == 5, "Root plus four children should be counted.");
		context.Expect(UISystem::CountElementTree(clone) == UISystem::CountElementTree(root), "Cloned trees should preserve deterministic element counts.");
	});

	registry.Add("Engine.UISystemPreparation.WorkerThresholdSeparatesSmallAndLargeDocuments", [](TestContext& context) {
		UISystem& uiSystem = UISystem::Instance();

		UISystem::UIPreparationRequest small = MakeSnapshotRequest(MakeRootWithChildren(8));
		UISystem::UIPreparationRequest large = MakeSnapshotRequest(MakeRootWithChildren(256));

		context.Expect(!uiSystem.ShouldPrepareOnWorker(small), "Small UI snapshots should stay inline.");
		context.Expect(uiSystem.ShouldPrepareOnWorker(large), "Large UI snapshots should be worker eligible.");
	});

	registry.Add("Engine.UISystemPreparation.PreparedOutputMatchesPreviewPath", [](TestContext& context) {
		UISystem& uiSystem = UISystem::Instance();
		const UIElement::Ptr root = MakeRootWithChildren(12);
		UISystem::UIPreparationRequest request = MakeSnapshotRequest(root);

		const UISystem::UIBuildResult prepared = uiSystem.BuildPreparedDocument(request);
		const UISystem::UIBuildResult preview = uiSystem.BuildPreviewDocument(root, request.viewportSize);

		context.Expect(prepared.root != nullptr, "Prepared result should produce a root.");
		context.Expect(prepared.error.empty(), "Prepared result should not report an error.");
		context.Expect(prepared.batch.staticQuads.size() == preview.batch.staticQuads.size(), "Prepared and preview batches should contain the same static quad count.");
	});

	registry.Add("Engine.UISystemPreparation.RepeatedPreparationIsDeterministic", [](TestContext& context) {
		UISystem& uiSystem = UISystem::Instance();
		const UIElement::Ptr root = MakeRootWithChildren(1000);
		const UISystem::UIPreparationRequest request = MakeSnapshotRequest(root);

		const UISystem::UIBuildResult first = uiSystem.BuildPreparedDocument(request);
		for (int iteration = 0; iteration < 16; ++iteration)
		{
			const UISystem::UIBuildResult next = uiSystem.BuildPreparedDocument(request);
			context.Expect(next.batch.staticQuads.size() == first.batch.staticQuads.size(), "Repeated preparation should keep static draw count stable.");
			context.Expect(next.batch.viewportSize == first.batch.viewportSize, "Repeated preparation should keep viewport stable.");
		}
	});

	registry.Add("Engine.UISystemPreparation.DiagnosticsCanStayClosed", [](TestContext& context) {
		UISystem& uiSystem = UISystem::Instance();
		uiSystem.SetPreparationDiagnosticsRequested(false);
		const UISystem::UIPreparationDiagnostics diagnostics = uiSystem.GetLastPreparationDiagnostics();

		context.Expect(!diagnostics.enabled, "Closed diagnostics should report disabled state.");
		context.Expect(diagnostics.documentsVisited == 0, "Closed diagnostics should not carry per-frame document details.");
	});

	registry.Add("Engine.UISystemPreparation.DiagnosticsCanBeArmedWithoutPreparing", [](TestContext& context) {
		UISystem& uiSystem = UISystem::Instance();
		uiSystem.SetPreparationDiagnosticsRequested(true);
		const UISystem::UIPreparationDiagnostics diagnostics = uiSystem.GetLastPreparationDiagnostics();
		uiSystem.SetPreparationDiagnosticsRequested(false);

		context.Expect(diagnostics.enabled, "Open diagnostics should report that UI preparation collection is armed.");
		context.Expect(diagnostics.requestsScheduled == 0, "Arming diagnostics should not fabricate preparation work.");
	});

	registry.Add("Engine.UISystemPreparation.ClassifiesPublishOutcomes", [](TestContext& context) {
		UISystem::UIBuildResult result;
		result.root = MakeRootWithChildren(1);
		result.requestVersion = 4;
		result.contentVersion = 9;
		result.shutdownGeneration = 2;

		context.Expect(
			UISystem::ClassifyPreparedResult(result, 4, 9, 2) == UISystem::UIPublishStatus::Published,
			"Matching versions and a valid root should publish.");

		context.Expect(
			UISystem::ClassifyPreparedResult(result, 5, 9, 2) == UISystem::UIPublishStatus::DiscardedStale,
			"Older worker request versions should be discarded.");

		context.Expect(
			UISystem::ClassifyPreparedResult(result, 4, 10, 2) == UISystem::UIPublishStatus::DiscardedStale,
			"Older content versions should be discarded.");

		context.Expect(
			UISystem::ClassifyPreparedResult(result, 4, 9, 3) == UISystem::UIPublishStatus::ShutdownSkipped,
			"Results from an older shutdown generation should not publish.");

		result.error = "parse failed";
		context.Expect(
			UISystem::ClassifyPreparedResult(result, 4, 9, 2) == UISystem::UIPublishStatus::FailedPreservedPrevious,
			"Failed preparations should preserve the previous live document.");
	});

	registry.Add("Engine.UISystemPreparation.InvalidInputPreservesPublishedRuntimeTree", [](TestContext& context) {
		ObjectManager& objects = ObjectManager::instance();
		UISystem& uiSystem = UISystem::Instance();
		objects.clear();
		uiSystem.Initialize();
		uiSystem.SetGameViewportSize(glm::vec2(800.0f, 600.0f));

		Object object = objects.createObject("UI Root");
		UIComponent& component = object.addComponent<UIComponent>();
		component.SetRoot(MakeRootWithChildren(4));
		component.MarkDirty();
		uiSystem.Update(1.0f / 60.0f);

		const UIElement::Ptr publishedRoot = component.GetRoot();
		const UIRenderBatch publishedBatch = uiSystem.GetCombinedFrameBatch();
		context.Expect(publishedRoot != nullptr, "A valid inline document should publish a runtime root.");
		context.Expect(!publishedBatch.staticQuads.empty(), "A valid inline document should publish draw data.");

		// SetSourceMarkup intentionally clears UIComponent's transient root. The
		// failed synchronous preparation must restore the previous root instead
		// of leaving scripts with a null document.
		component.SetSourceMarkup("missing-invalid-input-preserve-test.tuiml");
		const bool ready = uiSystem.EnsureDocumentReady(object.EntityId(), component);
		const UIRenderBatch preservedBatch = uiSystem.GetCombinedFrameBatch();

		context.Expect(!ready, "Missing authored markup should fail readiness.");
		context.Expect(component.GetRoot() == publishedRoot, "Failed preparation should restore the last valid component root.");
		context.Expect(preservedBatch.staticQuads.size() == publishedBatch.staticQuads.size(), "Failed preparation should preserve the last valid frame batch.");

		uiSystem.Shutdown();
		objects.clear();
	});

	registry.Add("Engine.UISystemPreparation.ExportRuntimeUsesWindowPointerWithoutScriptExportMode", [](TestContext& context) {
		ResetRuntimeInputState();

		WindowSystem& windowSystem = WindowSystem::Instance();
		windowSystem.Configure(std::make_unique<TestWindowBackend>(), WindowConfig{}, []() {});
		windowSystem.Initialize();

		auto inputBackend = std::make_unique<QueuedInputBackend>();
		QueuedInputBackend* inputBackendPointer = inputBackend.get();
		InputSystem& inputSystem = InputSystem::Instance();
		inputSystem.Configure(std::move(inputBackend));
		inputSystem.Initialize();
		inputBackendPointer->SetHardwareCursorPosition(24.0, 24.0);

		ResourceManager::Instance().ConfigureExportArchive("HeadlessExportRuntime/game.tepak");
		context.Expect(ResourceManager::Instance().IsExportRuntime(), "A configured TEPAK archive should mark the session as a packaged runtime.");
		context.Expect(!ScriptingSystem::Instance().IsExportMode(), "Packaged UI pointer input should not depend on NativeAOT scripting mode.");

		ObjectManager& objects = ObjectManager::instance();
		UISystem& uiSystem = UISystem::Instance();
		objects.clear();
		uiSystem.Initialize();
		uiSystem.SetGameViewportSize(glm::vec2(800.0f, 600.0f));

		UIElement::Ptr root = UIElement::Create(UIElementType::Root);
		root->typeName = "UIRoot";
		root->inlineStyle.width = UIValue::Points(800.0f);
		root->inlineStyle.height = UIValue::Points(600.0f);

		UIElement::Ptr button = UIElement::Create(UIElementType::Button);
		button->id = "export-button";
		button->inlineStyle.width = UIValue::Points(120.0f);
		button->inlineStyle.height = UIValue::Points(48.0f);
		button->inlineStyle.backgroundColor = glm::vec4(0.1f, 0.7f, 0.3f, 1.0f);
		root->AddChild(button);

		Object object = objects.createObject("Export UI Root");
		UIComponent& component = object.addComponent<UIComponent>();
		component.SetRoot(root);
		component.MarkDirty();

		uiSystem.Update(1.0f / 60.0f);

		inputBackendPointer->QueueMouseEvent(MouseButton::Left, KeyState::Pressed);
		inputBackendPointer->QueueMouseEvent(MouseButton::Left, KeyState::Released);
		inputSystem.Update(1.0f / 60.0f);
		uiSystem.Update(1.0f / 60.0f);

		context.Expect(uiSystem.WasElementClicked("export-button"), "Packaged runtime UI should publish clicks from window-space input even without script export mode.");

		uiSystem.Shutdown();
		objects.clear();
		ResetRuntimeInputState();
	});
}
}
