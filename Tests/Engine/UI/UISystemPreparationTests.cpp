#include "TestHarness.h"
#include "Components/UIComponent.h"
#include "Core/Scheduling/FrameWorkload.h"
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

	// Small real documents prepare inline through UISystem and publish cloned
	// roots. Tests attach event observers only after that production publication.
	UIElement::Ptr MakeHitBox(std::string id, float width, float height, UIElementType type = UIElementType::VisualElement)
	{
		auto element = UIElement::Create(type);
		element->id = std::move(id);
		element->inlineStyle.width = UIValue::Points(width);
		element->inlineStyle.height = UIValue::Points(height);
		element->inlineStyle.flexShrink = 0.0f;
		return element;
	}

	UIElement::Ptr MakeOverflowDocument()
	{
		auto root = MakeHitBox("root", 800, 600, UIElementType::Root);
		auto parent = MakeHitBox("parent", 40, 40);
		auto button = MakeHitBox("translated-button", 40, 40, UIElementType::Button);
		button->inlineStyle.transform.translate = glm::vec2(80, 0);
		button->inlineStyle.backgroundColor = glm::vec4(1, 0, 0, 1);
		parent->AddChild(button); root->AddChild(parent);
		return root;
	}

	void CountHitClick(UIElement& element, const glm::vec2&)
	{
		auto& count = element.attributes["observed-clicks"];
		count = std::to_string(count.empty() ? 1 : std::stoi(count) + 1);
	}

	int HitClickCount(const UIElement::Ptr& element)
	{
		const auto found = element->attributes.find("observed-clicks");
		return found == element->attributes.end() ? 0 : std::stoi(found->second);
	}

	class HitTestingFixture
	{
	public:
		HitTestingFixture()
		{
			objects.clear(); ui.Shutdown();
			ResourceManager::Instance().ConfigureExportArchive({});
			ui.Initialize(); ui.SetGameViewportSize({800, 600});
		}
		~HitTestingFixture() { ui.Shutdown(); objects.clear(); }
		Object AddDocument(const UIElement::Ptr& root)
		{
			const auto object = objects.createObject("Hit testing document");
			auto* component = objects.AddComponent<UIComponent>(object);
			component->SetRoot(root); component->MarkDirty();
			return object;
		}
		void Pointer(glm::vec2 position, bool inside, bool down)
		{
			ui.SetEditorGameViewPointer(position, inside, down);
			ui.Update(1.0f / 60.0f);
		}
		void Warm() { Pointer({-1, -1}, false, false); Pointer({-1, -1}, false, false); }
		UIElement::Ptr Published(const Object& object) { return objects.GetComponent<UIComponent>(object)->GetRoot(); }
		ObjectManager& objects = ObjectManager::instance();
		UISystem& ui = UISystem::Instance();
	};

	void ExpectOverflowRectangle(TestContext& context, const UISystem& ui)
	{
		const auto batch = ui.GetCombinedFrameBatch();
		context.Expect(batch.staticQuads.size() == 1U && batch.staticQuads.front().position == glm::vec2(80, 0)
			&& batch.staticQuads.front().size == glm::vec2(40, 40),
			"Production rendering must draw the authored child at (80,0), size (40,40), outside its parent.");
	}

	void ResetRuntimeInputState()
	{
		InputSystem::Instance().Shutdown();
		WindowSystem::Instance().Shutdown();
		ResourceManager::Instance().ConfigureExportArchive({});
	}
}

void RegisterUISystemPreparationTests(TestRegistry& registry)
{
	registry.Add("Engine.UI.HitTesting.TranslatedChildMatchesRenderedRectangle", [](TestContext& context) {
		HitTestingFixture fixture;
		const auto object = fixture.AddDocument(MakeOverflowDocument()); fixture.Warm();
		ExpectOverflowRectangle(context, fixture.ui);
		auto button = fixture.Published(object)->FindById("translated-button"); button->OnClick = CountHitClick;
		fixture.Pointer({90, 10}, true, false);
		context.Expect(fixture.ui.IsElementHovered("translated-button"), "Visible overflow must hover at its drawn position.");
		fixture.Pointer({90, 10}, true, true);
		context.Expect(fixture.ui.WasElementClicked("translated-button") && HitClickCount(button) == 1,
			"The rendered overflow target must receive exactly one accepted click.");
	});

	registry.Add("Engine.UI.HitTesting.NestedOverflowPreservesContainedAncestorHover", [](TestContext& context) {
		HitTestingFixture fixture;
		auto root = MakeOverflowDocument();
		auto containing = MakeHitBox("containing-ancestor", 200, 100);
		auto inner = MakeHitBox("inner-noncontaining", 20, 20);
		auto parent = root->children.front(); auto button = parent->children.front();
		parent->ClearChildren(); inner->AddChild(button); parent->AddChild(inner);
		root->ClearChildren(); containing->AddChild(parent); root->AddChild(containing);
		fixture.AddDocument(root); fixture.Warm(); ExpectOverflowRectangle(context, fixture.ui);
		fixture.Pointer({90, 10}, true, false);
		context.Expect(fixture.ui.IsElementHovered("translated-button") && fixture.ui.IsElementHovered("containing-ancestor")
			&& fixture.ui.IsElementHovered("root"), "Contained ancestors and the descendant must retain hover.");
		context.Expect(!fixture.ui.IsElementHovered("parent") && !fixture.ui.IsElementHovered("inner-noncontaining"),
			"Non-containing ancestors must permit targeting without acquiring their own hover.");
	});

	registry.Add("Engine.UI.HitTesting.HiddenSubtreesAndComponentsStayExcluded", [](TestContext& context) {
		for (int hiddenKind = 0; hiddenKind < 3; ++hiddenKind) {
			HitTestingFixture fixture; auto root = MakeOverflowDocument();
			if (hiddenKind == 0) root->FindById("translated-button")->SetVisible(false);
			if (hiddenKind == 1) root->FindById("parent")->SetVisible(false);
			const auto object = fixture.AddDocument(root);
			if (hiddenKind == 2) fixture.objects.GetComponent<UIComponent>(object)->SetVisible(false);
			fixture.Warm(); fixture.Pointer({90, 10}, true, false); fixture.Pointer({90, 10}, true, true);
			context.Expect(fixture.ui.GetCombinedFrameBatch().staticQuads.empty()
				&& !fixture.ui.IsElementHovered("translated-button") && !fixture.ui.WasElementClicked("translated-button"),
				"Hidden child, ancestor or document must exclude its rendered subtree and all target input.");
		}
	});

	registry.Add("Engine.UI.HitTesting.ReverseSiblingOverflowWinsOverlap", [](TestContext& context) {
		HitTestingFixture fixture; auto root = MakeHitBox("root", 800, 600, UIElementType::Root);
		root->inlineStyle.flexDirection = UIFlexDirection::Row;
		auto back = MakeHitBox("back-button", 40, 40, UIElementType::Button);
		back->inlineStyle.transform.translate = glm::vec2(80, 0); back->inlineStyle.backgroundColor = glm::vec4(0, 0, 1, 1);
		auto frontParent = MakeHitBox("front-parent", 40, 40);
		auto front = MakeHitBox("front-button", 40, 40, UIElementType::Button);
		front->inlineStyle.transform.translate = glm::vec2(40, 0); front->inlineStyle.backgroundColor = glm::vec4(1, 0, 0, 1);
		frontParent->AddChild(front); root->AddChild(back); root->AddChild(frontParent);
		const auto object = fixture.AddDocument(root); fixture.Warm();
		const auto batch = fixture.ui.GetCombinedFrameBatch();
		context.Expect(batch.staticQuads.size() == 2U && batch.staticQuads[0].position == glm::vec2(80, 0)
			&& batch.staticQuads[1].position == glm::vec2(80, 0), "Both siblings must really draw the independently expected overlapping rectangles.");
		auto published = fixture.Published(object); auto frontTarget = published->FindById("front-button");
		auto backTarget = published->FindById("back-button"); frontTarget->OnClick = CountHitClick; backTarget->OnClick = CountHitClick;
		fixture.Pointer({90, 10}, true, false); fixture.Pointer({90, 10}, true, true);
		context.Expect(fixture.ui.IsElementHovered("front-button") && !fixture.ui.IsElementHovered("back-button")
			&& HitClickCount(frontTarget) == 1 && HitClickCount(backTarget) == 0,
			"Reverse sibling targeting must select the front overflow descendant exactly once.");
	});

	registry.Add("Engine.UI.HitTesting.DocumentConsumptionAndViewportStayAuthoritative", [](TestContext& context) {
		HitTestingFixture fixture;
		auto firstRoot = MakeOverflowDocument(); firstRoot->FindById("translated-button")->id = "first-document-button";
		auto secondRoot = MakeOverflowDocument(); secondRoot->FindById("translated-button")->id = "second-document-button";
		const auto first = fixture.AddDocument(firstRoot); const auto second = fixture.AddDocument(secondRoot); fixture.Warm();
		// Existing input precedence is reverse UUID membership, independently of
		// render-batch entity order. Preserve that contract without a stacking change.
		const bool firstWins = fixture.objects.getUUID(first) > fixture.objects.getUUID(second);
		const std::string winner = firstWins ? "first-document-button" : "second-document-button";
		const std::string loser = firstWins ? "second-document-button" : "first-document-button";
		auto firstButton = fixture.Published(first)->FindById("first-document-button");
		auto secondButton = fixture.Published(second)->FindById("second-document-button");
		firstButton->OnClick = CountHitClick; secondButton->OnClick = CountHitClick;
		fixture.Pointer({90, 10}, true, false); fixture.Pointer({90, 10}, true, true);
		context.Expect(fixture.ui.IsElementHovered(winner) && fixture.ui.WasElementClicked(winner)
			&& !fixture.ui.IsElementHovered(loser) && !fixture.ui.WasElementClicked(loser)
			&& HitClickCount(firstButton) + HitClickCount(secondButton) == 1,
			"The established front input document must consume one click and exclude lower documents.");
		fixture.Pointer({90, 10}, false, false); fixture.Pointer({90, 10}, false, true);
		context.Expect(!fixture.ui.IsElementHovered(winner) && !fixture.ui.WasElementClicked(winner)
			&& HitClickCount(firstButton) + HitClickCount(secondButton) == 1,
			"Outside the active viewport, even a point inside a drawn rectangle must produce no interaction.");
	});

	registry.Add("Engine.UI.HitTesting.PressHoldReleaseCountsAcceptedPresses", [](TestContext& context) {
		HitTestingFixture fixture; const auto object = fixture.AddDocument(MakeOverflowDocument()); fixture.Warm();
		auto button = fixture.Published(object)->FindById("translated-button"); button->OnClick = CountHitClick;
		fixture.Pointer({90, 10}, true, false); fixture.Pointer({90, 10}, true, true);
		context.Expect(fixture.ui.WasElementClicked("translated-button") && HitClickCount(button) == 1, "First press must click once.");
		for (int heldFrame = 0; heldFrame < 3; ++heldFrame) {
			fixture.Pointer({90, 10}, true, true);
			context.Expect(!fixture.ui.WasElementClicked("translated-button") && HitClickCount(button) == 1, "Held input must not repeat a click.");
		}
		fixture.Pointer({90, 10}, true, false);
		context.Expect(!fixture.ui.WasElementClicked("translated-button") && HitClickCount(button) == 1, "Release must not click.");
		fixture.Pointer({90, 10}, true, true);
		context.Expect(fixture.ui.WasElementClicked("translated-button") && HitClickCount(button) == 2, "Second accepted press must produce exactly the second callback.");
	});

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

	registry.Add("Engine.UISystemPreparation.SchedulerDeclarationIsWorkerSafeForSnapshotPreparation", [](TestContext& context) {
		UISystem& uiSystem = UISystem::Instance();
		const TerraEngine::Scheduling::SystemDeclaration declaration = uiSystem.SchedulerDeclaration();
		TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
		workload.workloadName = declaration.name;
		workload.ownerSystem = declaration.name;
		workload.phase = declaration.phase;
		workload.reads = declaration.reads;
		workload.writes = declaration.writes;
		workload.workerEligible = !declaration.mainThread;
		workload.mainThreadRequired = declaration.mainThread;
		workload.protectedReason = "UI preparation uses cloned request snapshots.";

		const TerraEngine::Scheduling::FrameExecutionPlan plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ workload }, "ui-worker-compat");

		context.Expect(plan.valid, "UI scheduler declaration should validate as a frame workload.");
		context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.workerSafeCount, "UI snapshot preparation should be worker-safe in the migration summary.");
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
		Object objectHandle = objects.makeObjectHandle(object.EntityId());
		context.Expect(TerraEngine::IsResolvedObjectIdentity(objects.validateObjectHandle(objectHandle)), "UI preparation objects should have a valid compact identity before runtime collection.");
		UIComponent& component = *objects.AddComponent<UIComponent>(object);
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
		Object objectHandle = objects.makeObjectHandle(object.EntityId());
		context.Expect(TerraEngine::IsResolvedObjectIdentity(objects.validateObjectHandle(objectHandle)), "Export UI preparation should start from a valid compact identity.");
		UIComponent& component = *objects.AddComponent<UIComponent>(object);
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
