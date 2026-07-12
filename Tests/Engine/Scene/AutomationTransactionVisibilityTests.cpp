#include "TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"

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

			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Core/EditorSystem.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream source;
			source << input.rdbuf();
			context.Expect(source.str().find("post_editor_automation_flush") != std::string::npos, "EditorSystem diagnostics should name the transaction visibility boundary.");
			context.Expect(source.str().find("SceneMutationQueue") != std::string::npos, "EditorSystem should declare scene mutation writes instead of direct object-storage writes.");
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
	}
}
