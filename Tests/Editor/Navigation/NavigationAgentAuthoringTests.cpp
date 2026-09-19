// NavigationAgentAuthoringTests.cpp - inspector-facing authored agent regressions.

#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Navigation/NavigationInspectorSupport.h"
#include "Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.h"
#include "Engine/Components/NavigationAgentComponent.h"
#include "Engine/Core/Object/ComponentRegistry.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Scene/TransactionalMutation.h"

#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		constexpr std::string_view kAgentComponent = NavigationAgentComponent::kComponentName;

		NavigationAgentComponent* AgentFor(Object object)
		{
			return ObjectManager::instance().GetComponent<NavigationAgentComponent>(object);
		}

		void CompleteReplay(TestContext& context, const char* commitName)
		{
			auto& queue = Scene::SceneMutationQueue::Instance();
			queue.Apply(ObjectManager::instance(), commitName);
			const std::vector<Scene::TransactionResult> receipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(receipts.size() == 1U
				&& UndoRedoSystem::Instance().CompleteDeferredReplay(receipts.front()),
				"A deferred authoring history replay must complete only after its protected transaction receipt arrives.");
		}
	}

	void RegisterNavigationAgentAuthoringTests(TestRegistry& registry)
	{
		registry.Add("Editor.Navigation.AgentAuthoring.PresetCopyDuplicateAndOrdinarySaveLoading", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();

			Object source = objects.createObject("Navigation Agent Source");
			NavigationAgentComponent* agent = objects.AddComponent<NavigationAgentComponent>(source);
			Navigation::NavigationProfile projectProfile = Navigation::NavigationProfile::Defaults();
			projectProfile.radiusMetres = 0.65;
			projectProfile.heightMetres = 2.10;
			projectProfile.areaPenalties[3U] = 2.5;
			const double preservedArrivalTolerance = agent != nullptr ? agent->arrivalToleranceMetres : 0.0;
			const TerraEngine::Editor::Navigation::NavigationAuthoringValidation copied = agent != nullptr
				? TerraEngine::Editor::Navigation::ApplyNavigationAgentProjectProfile(*agent, projectProfile)
				: TerraEngine::Editor::Navigation::NavigationAuthoringValidation{};
			context.Expect(agent != nullptr && copied.accepted
				&& agent->radiusMetres == projectProfile.radiusMetres
				&& agent->heightMetres == projectProfile.heightMetres
				&& agent->areaPenalties[3U] == projectProfile.areaPenalties[3U]
				&& agent->arrivalToleranceMetres == preservedArrivalTolerance,
				"Copying an existing project navigation profile must replace only traversal values and preserve agent-specific arrival settings.");

			const nlohmann::json saved = Serializer::SerializeObject(source);
			nlohmann::json duplicateDocument = saved;
			duplicateDocument["uuid"] = ToHexString(GenerateUUID());
			Object duplicate = Serializer::DeserializeObject(duplicateDocument);
			NavigationAgentComponent* duplicateAgent = AgentFor(duplicate);
			context.Expect(duplicateAgent != nullptr && objects.getUUID(duplicate) != objects.getUUID(source)
				&& duplicateAgent->ValidateAuthoredValues()
				&& duplicateAgent->radiusMetres == projectProfile.radiusMetres
				&& duplicateAgent->areaPenalties[3U] == projectProfile.areaPenalties[3U],
				"Ordinary save/load duplication must retain authored agent values while creating a fresh object identity.");
			context.Expect(saved.contains("components")
				&& ComponentAccessRegistry().contains(std::string(kAgentComponent))
				&& !ComponentAccessRegistry().contains("NavigationSourceComponent")
				&& !ComponentAccessRegistry().contains("NavigationConnectionComponent"),
				"Agent defaults must load through normal component persistence while removed source and connection types remain unavailable.");

			objects.clear();
		});

		registry.Add("Editor.Navigation.AgentAuthoring.MultiEditRollsBackAndRestoresCompleteValues", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = Scene::SceneMutationQueue::Instance();
			objects.clear();
			queue.Clear();
			UndoRedoSystem::Instance().Clear();

			Object first = objects.createObject("First Agent");
			Object second = objects.createObject("Second Agent");
			NavigationAgentComponent* firstAgent = objects.AddComponent<NavigationAgentComponent>(first);
			NavigationAgentComponent* secondAgent = objects.AddComponent<NavigationAgentComponent>(second);
			if(firstAgent == nullptr || secondAgent == nullptr)
			{
				context.Expect(false, "The authoring fixture must attach NavigationAgentComponent to both selected objects.");
				return;
			}
			firstAgent->radiusMetres = 0.40;
			secondAgent->radiusMetres = 0.55;
			const auto originalFirstPenalties = firstAgent->areaPenalties;
			const auto originalSecondPenalties = secondAgent->areaPenalties;
			auto editedPenalties = originalFirstPenalties;
			editedPenalties[2U] = 3.0;

			Scene::TransactionalMutationBuilder rejectedBuilder(
				"navigation-agent-invalid-multi-edit", Scene::TransactionOrigin::Editor, "InspectorNavigationAgentMultiEdit");
			rejectedBuilder.ObserveScene(objects)
				.ObserveObject(objects, objects.getUUID(first))
				.ObserveObject(objects, objects.getUUID(second))
				.SetUndoGroup("Edit Navigation Agent Settings")
				.UpdateComponentProperty(objects.getUUID(first), std::string(kAgentComponent), "radiusMetres", Reflection::Variant{0.70})
				.UpdateComponentProperty(objects.getUUID(first), std::string(kAgentComponent), "areaPenalties", Reflection::Variant{editedPenalties})
				.UpdateComponentProperty(objects.getUUID(second), std::string(kAgentComponent), "minimumReplanIntervalSeconds", Reflection::Variant{0.01});
			const Scene::TransactionResult rejected = Scene::CommitTransaction(objects, rejectedBuilder.Build());
			context.Expect(rejected.status != Scene::TransactionStatus::Committed
				&& firstAgent->radiusMetres == 0.40 && secondAgent->radiusMetres == 0.55
				&& firstAgent->areaPenalties == originalFirstPenalties && secondAgent->areaPenalties == originalSecondPenalties,
				"One invalid complete agent candidate must reject the whole multi-selection edit without a partial write.");

			Scene::TransactionalMutationBuilder committedBuilder(
				"navigation-agent-valid-multi-edit", Scene::TransactionOrigin::Editor, "InspectorNavigationAgentMultiEdit");
			committedBuilder.ObserveScene(objects)
				.ObserveObject(objects, objects.getUUID(first))
				.ObserveObject(objects, objects.getUUID(second))
				.SetUndoGroup("Edit Navigation Agent Settings")
				.UpdateComponentProperty(objects.getUUID(first), std::string(kAgentComponent), "radiusMetres", Reflection::Variant{0.70})
				.UpdateComponentProperty(objects.getUUID(first), std::string(kAgentComponent), "areaPenalties", Reflection::Variant{editedPenalties})
				.UpdateComponentProperty(objects.getUUID(second), std::string(kAgentComponent), "radiusMetres", Reflection::Variant{0.70})
				.UpdateComponentProperty(objects.getUUID(second), std::string(kAgentComponent), "areaPenalties", Reflection::Variant{editedPenalties});
			const Scene::Transaction transaction = committedBuilder.Build();
			const Scene::TransactionResult committed = Scene::CommitTransaction(objects, transaction);
			context.Expect(committed.status == Scene::TransactionStatus::Committed
				&& UndoRedoSystem::Instance().RecordCommittedTransaction(transaction, committed)
				&& UndoRedoSystem::Instance().GetUndoDepth() == 1U,
				"One valid agent multi-edit must commit atomically and create one complete undo group.");
			context.Expect(firstAgent->radiusMetres == 0.70 && secondAgent->radiusMetres == 0.70
				&& firstAgent->areaPenalties == editedPenalties && secondAgent->areaPenalties == editedPenalties,
				"A committed multi-edit must publish the complete copied values to each selected agent.");

			UndoRedoSystem::Instance().Undo();
			CompleteReplay(context, "navigation-agent-authoring-undo");
			context.Expect(firstAgent->radiusMetres == 0.40 && secondAgent->radiusMetres == 0.55
				&& firstAgent->areaPenalties == originalFirstPenalties && secondAgent->areaPenalties == originalSecondPenalties,
				"Undo must restore every selected agent's complete pre-edit configuration.");
			UndoRedoSystem::Instance().Redo();
			CompleteReplay(context, "navigation-agent-authoring-redo");
			context.Expect(firstAgent->radiusMetres == 0.70 && secondAgent->radiusMetres == 0.70
				&& firstAgent->areaPenalties == editedPenalties && secondAgent->areaPenalties == editedPenalties,
				"Redo must replay the same complete multi-agent edit through the protected transaction queue.");

			UndoRedoSystem::Instance().Clear();
			queue.Clear();
			objects.clear();
		});
	}
}
