#include "TestHarness.h"
#include "Components/UIComponent.h"
#include "Core/Object/ObjectManager.h"
#include "UI/UISystem.h"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <string>

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
}
}
