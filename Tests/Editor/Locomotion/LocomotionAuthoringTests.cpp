// LocomotionAuthoringTests.cpp - durable authoring, transactional edits, and prefab identity.

#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Locomotion/LocomotionInspectorSupport.h"
#include "Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.h"
#include "Engine/Components/CharacterLocomotionComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ComponentRegistry.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Locomotion/LocomotionTypes.h"
#include "Engine/Locomotion/LocomotionCommands.h"
#include "Engine/Locomotion/LocomotionSystem.h"
#include "Engine/Components/Physics/CharacterControllerComponent.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scene/BulkScenePublication.h"
#include "Engine/Scene/PrefabComposition.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Scene/TransactionalMutation.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include "Tests/Engine/Scene/SceneGraphTestFixtures.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <vector>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		using TerraEngine::Editor::Locomotion::DescribeLocomotionRuntime;
		using TerraEngine::Editor::Locomotion::ValidateCharacterLocomotionDraft;

		void SetRepresentativeValues(CharacterLocomotionComponent& value)
		{
			value.active = true;
			value.controllerBinding = CharacterLocomotionComponent::kSelfBinding;
			value.navigationBinding = CharacterLocomotionComponent::kSelfBinding;
			value.animatorBinding = CharacterLocomotionComponent::kDescendantBinding;
			value.animatorObjectId = TerraEngine::Tests::SceneFixtures::MakeStableUuid(3U);
			value.maximumSpeed = 7.25;
			value.acceleration = 13.5;
			value.braking = 18.75;
			value.turnRate = 4.5;
			value.facing = CharacterLocomotionComponent::kFacingMovement;
			value.initialMotionMode = CharacterLocomotionComponent::kRootMotionMode;
			value.speedParameter = "Speed";
			value.localDirectionXParameter = "DirectionX";
			value.localDirectionZParameter = "DirectionZ";
			value.groundedParameter = "Grounded";
			value.movingParameter = "Moving";
		}

		void CompleteReplay(TestContext& context, const char* commitName)
		{
			auto& queue = Scene::SceneMutationQueue::Instance();
			(void)queue.Apply(ObjectManager::instance(), commitName);
			const auto receipts = queue.DrainCompletedEditorTransactionResults();
			context.Expect(receipts.size() == 1U
				&& UndoRedoSystem::Instance().CompleteDeferredReplay(receipts.front()),
				"The undo/redo cursor advances only after its protected transaction receipt arrives.");
		}

		struct LocomotionPlayLifecycleCleanup final {
			Locomotion::LocomotionSystem& locomotion;
			Physics::PhysicsSystem& physics;
			ScriptingSystem& scripting;
			Scene::SceneMutationQueue& queue;
			ObjectManager& objects;
			~LocomotionPlayLifecycleCleanup()
			{
				scripting.SetRunning(false);
				locomotion.Shutdown();
				(void)physics.StopWorld();
				physics.Shutdown();
				queue.Clear();
				objects.clear();
				ThreadPool::instance().shutdown();
			}
		};
	}

	void RegisterLocomotionAuthoringTests(TestRegistry& registry)
	{
		registry.Add("Editor.Locomotion.Authoring.InspectorProgressiveDisclosure", [](TestContext& context) {
			// Source wiring complements transactional behavior tests below. It catches
			// accidentally exposing advanced/runtime fields by default, not pixel layout.
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp");
			const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
			context.Expect(!source.empty(), "Inspector source must be available for the UI wiring regression.");
			const auto rowStart = source.find("void DrawMovementLabel(");
			const auto rowEnd = source.find("void DrawAnimatorPropertyGuidance(", rowStart);
			context.Expect(rowStart != std::string::npos && rowEnd != std::string::npos,
				"Movement inspectors must share the single-line label/value row helper.");
			if (rowStart != std::string::npos && rowEnd != std::string::npos) {
				const auto row = source.substr(rowStart, rowEnd - rowStart);
				context.Expect(row.find("ImGui::SameLine()") != std::string::npos
					&& row.find("SetCursorPosX(localStart.x + labelWidth)") != std::string::npos
					&& row.find("TextWrapped") == std::string::npos,
					"Labels must stay left of their values on one row, including long labels.");
			}
			const auto start = source.find("void InspectorWindow::DrawCharacterLocomotionInspector(");
			const auto end = source.find("void InspectorWindow::DrawAnimatorInspector(", start);
			if (start == std::string::npos || end == std::string::npos) {
				context.Expect(false, "Locomotion inspector boundaries must exist.");
				return;
			}
			const auto body = source.substr(start, end - start);
			context.Expect(body.find("Maximum Speed (m/s)") < body.find("CollapsingHeader(\"Advanced##Locomotion\")"),
				"Common speed tuning must appear before the default-collapsed advanced section.");
			context.Expect(body.find("CollapsingHeader(\"Runtime Diagnostics (Read-Only)##Locomotion\")") < body.find("CopyObservations()"),
				"Read-only runtime snapshots must be behind their own default-collapsed section.");
			context.Expect(body.find("BeginDisabled(!canTurn)") != std::string::npos
				&& body.find("BeginDisabled(!hasAnimator)") != std::string::npos
				&& body.find("if (showDescendant)") != std::string::npos,
				"Unused turning/animation settings must be disabled and descendant UUID entry conditional.");
			context.Expect(body.find("SubmitLocomotionBatch") != std::string::npos
				&& source.find("CollapsingHeader(\"Advanced##Navigation\")") != std::string::npos
				&& source.find("CollapsingHeader(\"Runtime Diagnostics (Read-Only)##Navigation\")") != std::string::npos,
				"Both inspectors must retain progressive disclosure and the transactional authoring path.");
		});

		registry.Add("Editor.Locomotion.Authoring.AuthoredRoundTripRejectsInvalidDetachedCandidate", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();

			CharacterLocomotionComponent authored;
			SetRepresentativeValues(authored);
			const nlohmann::json encoded = CharacterLocomotionComponent::SerializeAuthored(authored);
			CharacterLocomotionComponent decoded;
			context.Expect(CharacterLocomotionComponent::DeserializeAuthored(decoded, encoded)
				&& decoded.ValidateAuthoredValues()
				&& CharacterLocomotionComponent::SerializeAuthored(decoded) == encoded,
				"Every durable locomotion setting and descendant UUID must survive detached authored serialization.");

			const Object liveOwner = objects.createObject("Live locomotion authoring");
			auto* live = objects.AddComponent<CharacterLocomotionComponent>(liveOwner);
			if(live == nullptr) {
				context.Expect(false, "The invalid-detached-load fixture must own a live component.");
				return;
			}
			SetRepresentativeValues(*live);
			live->maximumSpeed = 6.0;
			const nlohmann::json liveBefore = CharacterLocomotionComponent::SerializeAuthored(*live);
			nlohmann::json invalid = encoded;
			invalid["maximumSpeed"] = 1000.0;
			CharacterLocomotionComponent candidate;
			context.Expect(!CharacterLocomotionComponent::DeserializeAuthored(candidate, invalid)
				&& CharacterLocomotionComponent::SerializeAuthored(*live) == liveBefore,
				"An invalid detached candidate must be rejected without changing an existing live authoring value.");

			// A scene loader must validate component payloads before it replaces the live
			// scene graph. The detached decode should discard its valid prefix on failure.
			const nlohmann::json invalidScene = {
				{"format", Scene::kTerraSceneUnboundedFormat}, {"name", "Invalid locomotion scene"},
				{"objects", nlohmann::json::array({{
					{"name", "bad candidate"}, {"uuid", ToHexString(GenerateUUID())},
					{"components", {{std::string(CharacterLocomotionComponent::kComponentName), invalid}}}
				}})}
			};
			const Scene::PreparedSceneDecode rejected = Serializer::PrepareSceneDecode(invalidScene);
			context.Expect(!rejected.accepted && rejected.objects.empty()
				&& objects.GetComponent<CharacterLocomotionComponent>(liveOwner) == live
				&& CharacterLocomotionComponent::SerializeAuthored(*live) == liveBefore,
				"Detached scene preparation must reject malformed locomotion data and leave the current live scene untouched.");
			objects.clear();
		});

		registry.Add("Editor.Locomotion.Authoring.SceneDuplicatePersistsOnlyAuthoredValues", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object source = objects.createObject("Locomotion source");
			if(!objects.AddComponent<Transform>(source)) {
				context.Expect(false, "The scene duplicate fixture requires a Transform.");
				return;
			}
			auto* authored = objects.AddComponent<CharacterLocomotionComponent>(source);
			if(authored == nullptr) {
				context.Expect(false, "The scene duplicate fixture requires CharacterLocomotionComponent.");
				return;
			}
			SetRepresentativeValues(*authored);
			const double sourceMaximumSpeed = authored->maximumSpeed;
			const UUID sourceAnimatorObjectId = authored->animatorObjectId;
			const nlohmann::json saved = Serializer::SerializeObject(source);
			const nlohmann::json payload = saved["components"][std::string(CharacterLocomotionComponent::kComponentName)];
			context.Expect(payload.value("maximumSpeed", 0.0) == sourceMaximumSpeed
				&& payload.value("animatorObjectId", std::string{}) == ToHexString(sourceAnimatorObjectId)
				&& !payload.contains("ownerToken") && !payload.contains("authorityEpoch")
				&& !payload.contains("runtimeHandle") && !payload.contains("receipt"),
				"Ordinary scene serialization must retain authored settings while excluding runtime authority and receipt state.");

			nlohmann::json duplicateDocument = saved;
			duplicateDocument["uuid"] = ToHexString(GenerateUUID());
			const Object duplicate = Serializer::DeserializeObject(duplicateDocument);
			const auto* cloned = objects.GetComponent<CharacterLocomotionComponent>(duplicate);
			context.Expect(cloned != nullptr && objects.getUUID(duplicate) != objects.getUUID(source)
				&& cloned->ValidateAuthoredValues() && cloned->maximumSpeed == sourceMaximumSpeed
				&& cloned->animatorBinding == CharacterLocomotionComponent::kDescendantBinding
				&& cloned->animatorObjectId == sourceAnimatorObjectId,
				"Ordinary duplication must copy authored configuration while preserving the reference as an explicit unresolved source UUID.");
			objects.clear();
		});

		registry.Add("Editor.Locomotion.Authoring.MultiEditRollsBackAndUndoRedoRestoresFullValues", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = Scene::SceneMutationQueue::Instance();
			objects.clear();
			queue.Clear();
			UndoRedoSystem::Instance().Clear();
			const Object first = objects.createObject("First locomotion actor");
			const Object second = objects.createObject("Second locomotion actor");
			auto* firstValue = objects.AddComponent<CharacterLocomotionComponent>(first);
			auto* secondValue = objects.AddComponent<CharacterLocomotionComponent>(second);
			if(firstValue == nullptr || secondValue == nullptr) {
				context.Expect(false, "Multi-edit needs locomotion components on both selected actors.");
				return;
			}
			firstValue->maximumSpeed = 4.0;
			secondValue->maximumSpeed = 5.0;
			const nlohmann::json firstBefore = CharacterLocomotionComponent::SerializeAuthored(*firstValue);
			const nlohmann::json secondBefore = CharacterLocomotionComponent::SerializeAuthored(*secondValue);

			Scene::TransactionalMutationBuilder invalidBuilder(
				"locomotion-invalid-multi-edit", Scene::TransactionOrigin::Editor, "InspectorCharacterLocomotionMultiEdit");
			invalidBuilder.ObserveScene(objects).ObserveObject(objects, objects.getUUID(first))
				.ObserveObject(objects, objects.getUUID(second)).SetUndoGroup("Edit Character Locomotion")
				.UpdateComponentProperty(objects.getUUID(first), std::string(CharacterLocomotionComponent::kComponentName),
					"maximumSpeed", Reflection::Variant{8.0})
				.UpdateComponentProperty(objects.getUUID(second), std::string(CharacterLocomotionComponent::kComponentName),
					"braking", Reflection::Variant{0.0});
			const Scene::TransactionResult rejected = Scene::CommitTransaction(objects, invalidBuilder.Build());
			context.Expect(rejected.status != Scene::TransactionStatus::Committed
				&& CharacterLocomotionComponent::SerializeAuthored(*firstValue) == firstBefore
				&& CharacterLocomotionComponent::SerializeAuthored(*secondValue) == secondBefore
				&& UndoRedoSystem::Instance().GetUndoDepth() == 0U,
				"One invalid selected-object candidate must roll back the complete edit and create no undo entry.");

			Scene::TransactionalMutationBuilder validBuilder(
				"locomotion-valid-multi-edit", Scene::TransactionOrigin::Editor, "InspectorCharacterLocomotionMultiEdit");
			validBuilder.ObserveScene(objects).ObserveObject(objects, objects.getUUID(first))
				.ObserveObject(objects, objects.getUUID(second)).SetUndoGroup("Edit Character Locomotion")
				.UpdateComponentProperty(objects.getUUID(first), std::string(CharacterLocomotionComponent::kComponentName),
					"maximumSpeed", Reflection::Variant{8.0})
				.UpdateComponentProperty(objects.getUUID(first), std::string(CharacterLocomotionComponent::kComponentName),
					"movingParameter", Reflection::Variant{std::string("Moving")})
				.UpdateComponentProperty(objects.getUUID(second), std::string(CharacterLocomotionComponent::kComponentName),
					"maximumSpeed", Reflection::Variant{8.0})
				.UpdateComponentProperty(objects.getUUID(second), std::string(CharacterLocomotionComponent::kComponentName),
					"movingParameter", Reflection::Variant{std::string("Moving")});
			const Scene::Transaction validTransaction = validBuilder.Build();
			const Scene::TransactionResult committed = Scene::CommitTransaction(objects, validTransaction);
			context.Expect(committed.status == Scene::TransactionStatus::Committed
				&& UndoRedoSystem::Instance().RecordCommittedTransaction(validTransaction, committed)
				&& UndoRedoSystem::Instance().GetUndoDepth() == 1U,
				"A valid multi-selection edit must commit the full candidate and create one undo group.");
			context.Expect(firstValue->maximumSpeed == 8.0 && secondValue->maximumSpeed == 8.0
				&& firstValue->movingParameter == "Moving" && secondValue->movingParameter == "Moving",
				"The protected multi-edit must publish every selected authored value together.");
			UndoRedoSystem::Instance().Undo();
			CompleteReplay(context, "locomotion-authoring-undo");
			context.Expect(CharacterLocomotionComponent::SerializeAuthored(*firstValue) == firstBefore
				&& CharacterLocomotionComponent::SerializeAuthored(*secondValue) == secondBefore,
				"Undo must restore each selected object's complete pre-edit authored payload.");
			UndoRedoSystem::Instance().Redo();
			CompleteReplay(context, "locomotion-authoring-redo");
			context.Expect(firstValue->maximumSpeed == 8.0 && secondValue->maximumSpeed == 8.0
				&& firstValue->movingParameter == "Moving" && secondValue->movingParameter == "Moving",
				"Redo must replay the same complete multi-object authored edit through the protected queue.");
			UndoRedoSystem::Instance().Clear();
			queue.Clear();
			objects.clear();
		});

		registry.Add("Editor.Locomotion.Authoring.PrefabRemapsDescendantAndPreservesSelfOrExternalReferences", [](TestContext& context) {
			auto source = Scene::PrefabSource{};
			source.sourceId = TerraEngine::Tests::SceneFixtures::MakeStableUuid(40U);
			source.sourcePath = "Fixtures/Locomotion.teprefab";
			source.sourceGraph = TerraEngine::Tests::SceneFixtures::MakeThreeLevelGraph();
			const UUID rootId = TerraEngine::Tests::SceneFixtures::MakeStableUuid(1U);
			const UUID childId = TerraEngine::Tests::SceneFixtures::MakeStableUuid(2U);
			const UUID instanceRoot = TerraEngine::Tests::SceneFixtures::MakeStableUuid(51U);
			const UUID instanceChild = TerraEngine::Tests::SceneFixtures::MakeStableUuid(52U);
			const UUID externalActor = TerraEngine::Tests::SceneFixtures::MakeStableUuid(99U);

			CharacterLocomotionComponent descendant;
			SetRepresentativeValues(descendant);
			descendant.animatorObjectId = childId;
			const std::string componentName(CharacterLocomotionComponent::kComponentName);
			source.componentDefaults[rootId][componentName] = CharacterLocomotionComponent::SerializeAuthored(descendant);
			Scene::PrefabInstance instance;
			instance.instanceId = TerraEngine::Tests::SceneFixtures::MakeStableUuid(60U);
			instance.sourceId = source.sourceId;
			instance.rootObjectId = instanceRoot;
			instance.sourceToInstanceObjectIds[rootId] = instanceRoot;
			instance.sourceToInstanceObjectIds[childId] = instanceChild;
			Scene::PrefabCompositionResult resolved = Scene::ResolvePrefabInstance(source, instance);
			const auto rootDefaults = resolved.resolvedComponentDefaults.find(instanceRoot);
			bool descendantRemapped = false;
			if(rootDefaults != resolved.resolvedComponentDefaults.end()) {
				const auto component = rootDefaults->second.find(componentName);
				if(component != rootDefaults->second.end()) {
					CharacterLocomotionComponent value;
					descendantRemapped = CharacterLocomotionComponent::DeserializeAuthored(value, component->second)
						&& value.animatorBinding == CharacterLocomotionComponent::kDescendantBinding
						&& value.animatorObjectId == instanceChild;
				}
			}
			context.Expect(resolved.success && descendantRemapped,
				"A descendant animator reference inside the cloned prefab set must resolve to that clone's UUID.");

			CharacterLocomotionComponent self;
			self.animatorBinding = CharacterLocomotionComponent::kSelfBinding;
			self.animatorObjectId = EMPTY_UUID;
			source.componentDefaults[rootId][componentName] = CharacterLocomotionComponent::SerializeAuthored(self);
			const Scene::PrefabCompositionResult selfResolved = Scene::ResolvePrefabInstance(source, instance);
			bool selfPreserved = false;
			if(const auto row = selfResolved.resolvedComponentDefaults.find(instanceRoot);
				row != selfResolved.resolvedComponentDefaults.end()) {
				if(const auto component = row->second.find(componentName); component != row->second.end()) {
					CharacterLocomotionComponent value;
					selfPreserved = CharacterLocomotionComponent::DeserializeAuthored(value, component->second)
						&& value.animatorBinding == CharacterLocomotionComponent::kSelfBinding
						&& value.animatorObjectId == EMPTY_UUID;
				}
			}
			context.Expect(selfResolved.success && selfPreserved,
				"A Self animator binding must stay Self instead of becoming an explicit old-object UUID.");

			CharacterLocomotionComponent outside;
			outside.animatorBinding = CharacterLocomotionComponent::kDescendantBinding;
			outside.animatorObjectId = externalActor;
			source.componentDefaults[rootId][componentName] = CharacterLocomotionComponent::SerializeAuthored(outside);
			const Scene::PrefabCompositionResult outsideResolved = Scene::ResolvePrefabInstance(source, instance);
			bool unresolvedRetained = false;
			if(const auto row = outsideResolved.resolvedComponentDefaults.find(instanceRoot);
				row != outsideResolved.resolvedComponentDefaults.end()) {
				if(const auto component = row->second.find(componentName); component != row->second.end()) {
					CharacterLocomotionComponent value;
					unresolvedRetained = CharacterLocomotionComponent::DeserializeAuthored(value, component->second)
						&& value.animatorBinding == CharacterLocomotionComponent::kDescendantBinding
						&& value.animatorObjectId == externalActor;
				}
			}
			context.Expect(outsideResolved.success && unresolvedRetained,
				"A descendant reference outside the cloned set must remain authored and explicitly unresolved, not retarget the source actor.");
		});

		registry.Add("Editor.Locomotion.Authoring.InspectorValidatesDetachedDraftAndDisplaysCopiedObservation", [](TestContext& context) {
			CharacterLocomotionComponent valid;
			SetRepresentativeValues(valid);
			const auto accepted = ValidateCharacterLocomotionDraft(valid);
			context.Expect(accepted.accepted,
				"The inspector must accept a complete valid detached candidate before submitting a transaction.");
			CharacterLocomotionComponent invalid = valid;
			invalid.maximumSpeed = std::numeric_limits<double>::quiet_NaN();
			const auto rejected = ValidateCharacterLocomotionDraft(invalid);
			context.Expect(!rejected.accepted && !rejected.field.empty() && !rejected.explanation.empty(),
				"Invalid inspector candidates must identify a field and explain why the draft cannot be committed.");

			Locomotion::LocomotionObservation observation;
			observation.authority.state = Locomotion::AuthorityState::Controlled;
			observation.authority.mode = Locomotion::MotionMode::RootMotion;
			observation.authority.ownerToken = 42U;
			observation.bindingReason = Locomotion::LocomotionReason::None;
			observation.navigationReason = Locomotion::LocomotionReason::None;
			observation.animationReason = Locomotion::LocomotionReason::None;
			observation.desiredVelocity = {3.0, 0.0, 4.0};
			observation.observedPlanarSpeed = 4.0;
			observation.physicalTick = 77U;
			const auto display = DescribeLocomotionRuntime(observation);
			context.Expect(!display.authority.empty() && !display.mode.empty() && !display.binding.empty()
				&& !display.navigation.empty() && !display.animation.empty()
				&& std::abs(display.desiredSpeed - 5.0) < 0.001
				&& std::abs(display.realizedSpeed - 4.0) < 0.001 && display.physicalTick == 77U,
				"Inspector runtime details must come from the copied observation and omit the owner token itself.");
		});

		registry.Add("Editor.Locomotion.Authoring.PlayStopRevokesBeforeAuthoredRestoreAndFreshPlayCannotReuseToken", [](TestContext& context) {
			// Use the actual scripting Play/Stop boundary plus Physics and scene bulk
			// publication APIs; the test never reaches into the locomotion store.
			ThreadPool::instance().shutdown();
			ThreadPool::instance().initialize();
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = Scene::SceneMutationQueue::Instance();
			auto& locomotion = Locomotion::LocomotionSystem::Instance();
			auto& physics = Physics::PhysicsSystem::Instance();
			auto& scripting = ScriptingSystem::Instance();
			LocomotionPlayLifecycleCleanup cleanup{locomotion, physics, scripting, queue, objects};
			locomotion.Shutdown();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			objects.clear();
			queue.Clear();
			scripting.SetRunning(false);
			Object owner = objects.createObject("Play lifecycle locomotion owner");
			if(!objects.AddComponent<Transform>(owner)) {
				context.Expect(false, "The Play lifecycle fixture requires a Transform.");
				return;
			}
			auto* controller = objects.AddComponent<CharacterControllerComponent>(owner);
			auto* authored = objects.AddComponent<CharacterLocomotionComponent>(owner);
			if(controller == nullptr || authored == nullptr) {
				context.Expect(false, "The Play lifecycle fixture needs both a controller and locomotion authoring.");
				return;
			}
			controller->radius = 0.35F;
			controller->height = 1.8F;
			controller->localCenter = {0.0F, 0.9F, 0.0F};
			controller->useGravity = false;
			authored->maximumSpeed = 6.5;
			const double authoredSpeed = authored->maximumSpeed;
			const nlohmann::json snapshotDocument = {
				{"format", Scene::kTerraSceneUnboundedFormat}, {"name", "Locomotion Play Restore"},
				{"objects", nlohmann::json::array({Serializer::SerializeObject(owner)})}
			};
			const Scene::PreparedSceneDecode prepared = Serializer::PrepareSceneDecode(snapshotDocument);
			Scene::BulkScenePublicationPacket restorePacket = Scene::BuildBulkScenePublicationPacket(
				prepared, objects.GetSceneGeneration(), objects.GetSceneGeneration());
			context.Expect(prepared.accepted && !restorePacket.cancelled && physics.StartWorld(),
				"The actual runtime fixture must capture an authored scene and start a fresh Physics Play world.");
			if(!prepared.accepted || restorePacket.cancelled || !physics.WorldHandle().IsValid()) return;
			scripting.SetRunning(true);
			locomotion.Initialize();
			auto advance = [&]() {
				(void)objects.PublishSceneChangePacket();
				physics.Update(1.0F / 60.0F);
				const auto flushed = queue.Apply(objects, "locomotion-play-lifecycle-physics");
				physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
				locomotion.ObserveProtectedBoundary();
				locomotion.Update(1.0F / 60.0F);
			};
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) advance();
			const UUID ownerId = objects.getUUID(owner);
			const auto initialResolution = locomotion.ResolvePublishedSnapshot(ownerId);
			const auto initialObservation = locomotion.CopyObservations();
			if(!initialResolution || !initialObservation || initialObservation->entries.empty()) {
				context.Expect(false, "Play must publish a current locomotion identity before any command is admitted.");
				return;
			}
			const auto row = std::find_if(initialObservation->entries.begin(), initialObservation->entries.end(),
				[&initialResolution](const Locomotion::LocomotionObservation& value) {
					return value.handle == initialResolution->handle;
				});
			if(row == initialObservation->entries.end()) {
				context.Expect(false, "The current handle must have a matching copied authority observation.");
				return;
			}
			const Locomotion::LocomotionHandle oldHandle = initialResolution->handle;
			const Physics::PhysicsCharacterCommandTarget oldController = row->authority.controller;
			Locomotion::LocomotionCommand acquire;
			acquire.kind = Locomotion::CommandKind::Acquire;
			acquire.handle = oldHandle;
			acquire.expectedConfigurationRevision = initialResolution->configurationRevision;
			acquire.expectedAuthorityEpoch = initialResolution->authorityEpoch;
			acquire.producerId = 4401U;
			acquire.sequence = 1U;
			acquire.correlationId = 44001U;
			const auto acquireAdmission = locomotion.SubmitBatch(std::span(&acquire, 1U));
			locomotion.Update(1.0F / 60.0F);
			const auto acquired = locomotion.ReadReceipt(oldHandle, acquire.producerId, acquire.sequence, acquire.correlationId);
			const auto initialLease = physics.ReadCharacterMotionLease(oldController);
			context.Expect(acquireAdmission.queued && acquired.state == Locomotion::ReceiptState::Applied
				&& acquired.ownerToken != 0U && initialLease
				&& initialLease->state == Physics::CharacterMotionLeaseState::LocomotionOwned,
				"Play must grant one real Physics-backed owner token before the stop boundary.");
			if(acquired.state != Locomotion::ReceiptState::Applied) return;

			Locomotion::LocomotionCommand queuedIntent;
			queuedIntent.kind = Locomotion::CommandKind::SetIntent;
			queuedIntent.intentKind = Locomotion::IntentKind::Script;
			queuedIntent.handle = oldHandle;
			queuedIntent.expectedConfigurationRevision = initialResolution->configurationRevision;
			queuedIntent.expectedAuthorityEpoch = acquired.authorityEpoch;
			queuedIntent.ownerToken = acquired.ownerToken;
			queuedIntent.producerId = acquire.producerId;
			queuedIntent.sequence = 2U;
			queuedIntent.correlationId = 44002U;
			queuedIntent.direction = {1.0, 0.0, 0.0};
			queuedIntent.requestedSpeed = 5.0;
			queuedIntent.lifetime = Locomotion::IntentLifetime::UntilStopped;
			const auto intentAdmission = locomotion.SubmitBatch(std::span(&queuedIntent, 1U));
			const auto queuedReceipt = locomotion.ReadReceipt(oldHandle, queuedIntent.producerId,
				queuedIntent.sequence, queuedIntent.correlationId);
			const auto leaseWithQueuedIntent = physics.ReadCharacterMotionLease(oldController);
			const auto lifecycleDiagnostics = locomotion.ReadDiagnostics();
			const bool intentStayedQueued = intentAdmission.queued
				&& queuedReceipt.state == Locomotion::ReceiptState::Queued;
			const bool authorityStayedActive = leaseWithQueuedIntent
				&& leaseWithQueuedIntent->state == Physics::CharacterMotionLeaseState::LocomotionOwned;
			const std::string queuedIntentDiagnostic = "queued=" + std::to_string(intentAdmission.queued)
				+ ", admissionReason=" + std::to_string(static_cast<unsigned>(intentAdmission.reason))
				+ ", receiptState=" + std::to_string(static_cast<unsigned>(queuedReceipt.state))
				+ ", cachedPendingCommands=" + std::to_string(lifecycleDiagnostics.pendingCommands)
				+ ", physicalLease=" + std::to_string(leaseWithQueuedIntent
					? static_cast<unsigned>(leaseWithQueuedIntent->state) : 255U);
			context.Expect(intentStayedQueued && authorityStayedActive,
				"The Stop boundary fixture must contain active Physics authority and an unapplied queued receipt ("
				+ queuedIntentDiagnostic + ").");

			// The scene snapshot is complete before this runtime-only mutation. Stop
			// must revoke command identity before the bulk packet restores authored data.
			objects.GetComponent<CharacterLocomotionComponent>(owner)->maximumSpeed = 15.0;
			scripting.SetRunning(false);
			const auto leaseBeforeRestore = physics.ReadCharacterMotionLease(oldController);
			context.Expect(!locomotion.ResolvePublished(ownerId).has_value()
				&& locomotion.ReadDiagnostics().pendingCommands == 0U
				&& leaseBeforeRestore && leaseBeforeRestore->state == Physics::CharacterMotionLeaseState::Unleased,
				"Play Stop must revoke bindings, queued commands, and the Physics locomotion lease before authored restore begins.");
			context.Expect(physics.StopWorld(), "The Play world must drain before its captured authored graph is republished.");
			restorePacket.expectedSceneGeneration = objects.GetSceneGeneration();
			const auto restored = Scene::PublishBulkScenePacket(restorePacket, objects);
			const auto restoredOwner = objects.getObject(ownerId);
			const auto* restoredAuthoring = restoredOwner
				? objects.GetComponent<CharacterLocomotionComponent>(*restoredOwner) : nullptr;
			context.Expect(restored.published && restoredAuthoring != nullptr
				&& restoredAuthoring->maximumSpeed == authoredSpeed,
				"The actual bulk restore must recover saved locomotion authoring only after stale runtime authority has been retired.");

			context.Expect(physics.StartWorld(), "A fresh Play re-entry must create a new provider world after restoration.");
			if(!physics.WorldHandle().IsValid()) return;
			scripting.SetRunning(true);
			locomotion.Initialize();
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) advance();
			const auto freshResolution = locomotion.ResolvePublishedSnapshot(ownerId);
			context.Expect(freshResolution.has_value() && freshResolution->handle != oldHandle,
				"Re-entry must mint a new scene-qualified binding rather than resurrect the pre-Stop handle.");
			const auto staleIntentAdmission = locomotion.SubmitBatch(std::span(&queuedIntent, 1U));
			context.Expect(!staleIntentAdmission.queued,
				"The queued old-token intent must remain stale after a new Play session republishes the same object UUID.");
			if(freshResolution) {
				const auto freshObservation = locomotion.CopyObservations();
				bool foundFreshRow = false;
				if(freshObservation) {
					const auto freshRow = std::find_if(freshObservation->entries.begin(), freshObservation->entries.end(),
						[&freshResolution](const Locomotion::LocomotionObservation& value) {
							return value.handle == freshResolution->handle;
						});
					foundFreshRow = freshRow != freshObservation->entries.end();
				}
				if(foundFreshRow) {
					Locomotion::LocomotionCommand freshAcquire;
					freshAcquire.kind = Locomotion::CommandKind::Acquire;
					freshAcquire.handle = freshResolution->handle;
					freshAcquire.expectedConfigurationRevision = freshResolution->configurationRevision;
					freshAcquire.expectedAuthorityEpoch = freshResolution->authorityEpoch;
					freshAcquire.producerId = acquire.producerId;
					freshAcquire.sequence = 3U;
					freshAcquire.correlationId = 44003U;
					(void)locomotion.SubmitBatch(std::span(&freshAcquire, 1U));
					locomotion.Update(1.0F / 60.0F);
					const auto freshReceipt = locomotion.ReadReceipt(freshAcquire.handle,
						freshAcquire.producerId, freshAcquire.sequence, freshAcquire.correlationId);
					context.Expect(freshReceipt.state == Locomotion::ReceiptState::Applied
						&& freshReceipt.ownerToken != 0U && freshReceipt.ownerToken != acquired.ownerToken,
						"Only an explicit fresh-session Acquire may obtain a different token after restoration.");
				} else context.Expect(false, "Fresh Play must expose a copied binding row for explicit reacquisition.");
			}
		});
	}
}
