#include "TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Editor/TerraEngine-Editor/Core/EditorSchedulerDeclaration.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::Tests
{
	void RegisterAutomationTransactionVisibilityTests(TestRegistry& registry)
	{
		registry.Add("Engine.AutomationTransaction.SchedulerDeclaresProtectedVisibilityResources", [](TestContext& context) {
			const auto resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			const auto hasResource = [&resources](const std::string& name) {
				return std::any_of(resources.begin(), resources.end(), [&name](const auto& resource) { return resource.name == name; });
			};
			context.Expect(hasResource("EditorAutomationQueue"), "The scheduler should declare prepared automation work as a resource.");
			context.Expect(hasResource("SceneMutationQueue"), "The scheduler should declare the typed deferred scene publication resource.");

			// Read the same metadata used at startup, without linking native UI.
			const auto declaration = TerraEngine::Editor::MakeEditorSchedulerDeclaration();
			context.Expect(declaration.protectedCommit.find("post_editor_automation_flush") != std::string::npos, "EditorSystem diagnostics should name the transaction visibility boundary.");
			context.Expect(std::find(declaration.writes.begin(), declaration.writes.end(), "SceneMutationQueue") != declaration.writes.end(), "EditorSystem should declare deferred scene mutation writes.");
		});

		registry.Add("Engine.AutomationTransaction.VisibilityPublishesOnlyAtDeclaredFlush", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue queue;
			queue.Enqueue({"automation-create", TerraEngine::Scene::SceneMutationOperation::CreateObject, {}, {}, "Published At Boundary"});

			// Enqueue represents worker preparation. Live object storage must remain
			// unchanged until the protected editor automation flush owns publication.
			context.Expect(objects.getRootObjects().empty(), "Prepared automation writes must not become visible before the protected boundary.");
			const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(objects, "post_editor_automation_flush");
			context.Expect(result.applied, "The declared protected flush should publish a valid prepared transaction.");
			context.ExpectEqual(static_cast<std::size_t>(1), objects.getRootObjects().size(), "All writes should become visible at the same boundary.");
			context.Expect(result.formattedCommitSummary.find("post_editor_automation_flush") != std::string::npos, "Commit diagnostics should expose the automation visibility boundary.");
			objects.clear();
		});

		registry.Add("Engine.AutomationTransaction.VisibilityDefersRequestsArrivingAfterFlush", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue queue;

			// The first Apply models this frame's already-completed flush. Work that
			// arrives afterward remains queued and can only publish next frame.
			queue.Apply(objects, "post_editor_automation_flush");
			queue.Enqueue({"late-automation-create", TerraEngine::Scene::SceneMutationOperation::CreateObject, {}, {}, "Next Frame"});
			context.Expect(objects.getRootObjects().empty(), "A request arriving after the boundary must not leak into the current frame.");
			context.Expect(!queue.Empty(), "Late work should remain queued for the next declared boundary.");

			const TerraEngine::Scene::SceneMutationBatchResult nextFrame = queue.Apply(objects, "post_editor_automation_flush");
			context.Expect(nextFrame.applied, "The next frame boundary should publish previously late work.");
			context.ExpectEqual(static_cast<std::size_t>(1), objects.getRootObjects().size(), "Late work should become visible exactly once on the next frame.");
			objects.clear();
		});

		registry.Add("Engine.AutomationTransaction.SameFrameAndFrameEndVisibilityUseOnlyTheirDeclaredBoundaries", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue queue;

			// A later same-frame consumer explicitly selects After Physics. The
			// prepared write remains invisible until that named protected boundary.
			queue.Enqueue({"same-frame-create", TerraEngine::Scene::SceneMutationOperation::CreateObject, {}, {}, "Same Frame"});
			context.Expect(objects.getRootObjects().empty(), "A same-frame transaction must remain deferred before After Physics.");
			const auto sameFrame = queue.Apply(objects, "After Physics");
			context.Expect(sameFrame.applied, "After Physics must publish work required by a declared later same-frame consumer.");
			context.ExpectEqual(static_cast<std::size_t>(1u), objects.getRootObjects().size(), "The later same-frame consumer may observe the committed object after its declared flush.");

			objects.clear();
			queue.Enqueue({"frame-end-create", TerraEngine::Scene::SceneMutationOperation::CreateObject, {}, {}, "Frame End"});
			context.Expect(objects.getRootObjects().empty(), "Without a same-frame consumer, work must not become visible in the physics phase.");
			// No After Physics Apply is performed in this branch. This models the
			// scheduler omitting that protected workload rather than issuing a no-op.
			context.Expect(!queue.Empty(), "Frame-end work must remain queued while the optional same-frame boundary is omitted.");
			const auto frameEnd = queue.Apply(objects, "Frame End");
			context.Expect(frameEnd.applied, "Frame End must publish work when no later same-frame consumer was declared.");
			context.ExpectEqual(static_cast<std::size_t>(1u), objects.getRootObjects().size(), "Frame-end publication must expose the transaction exactly once.");
			objects.clear();
		});
	}
}
