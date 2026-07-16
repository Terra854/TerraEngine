#include "../TestHarness.h"

#include "Components/ScriptComponent.h"
#include "Components/Transform.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Core/Object/Component.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Scripting/ScriptingSystem.h"
#include "ScriptingRuntimeTestFixtures.h"

#include "Renderer/RenderSystem.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptGlue.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests {
	void RegisterScriptingComponentAccessTests(TestRegistry& registry)
	{
		registry.Add("Scripting.ComponentAccess.ManagedCallbacksAreWorkerScheduled", [](TestContext& context) {
			const ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
			context.Expect(scriptingSystem.ManagedCallbacksRunProtected(), "Managed callbacks should remain behind the protected boundary.");
			context.Expect(!scriptingSystem.ManagedCallbacksRequireSchedulerAnnotations(), "C# scripts should not require author-side scheduler annotations.");
		});

		registry.Add("Scripting.ComponentAccess.SchedulerDeclarationReportsProtectedManagedBoundary", [](TestContext& context) {
			const ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
			const TerraEngine::Scheduling::SystemDeclaration declaration = const_cast<ScriptingSystem&>(scriptingSystem).SchedulerDeclaration();
			TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
			workload.workloadName = declaration.name;
			workload.ownerSystem = declaration.name;
			workload.phase = declaration.phase;
			workload.reads = declaration.reads;
			workload.writes = declaration.writes;
			workload.workerEligible = !declaration.mainThread;
			workload.mainThreadRequired = declaration.mainThread;
			workload.protectedReason = scriptingSystem.GetSchedulerBoundarySummary();

			const TerraEngine::Scheduling::FrameExecutionPlan plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ workload }, "scripting-protected-boundary");

			context.Expect(plan.valid, "Scripting protected declaration should validate as a frame workload.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.protectedCount, "Scripting managed boundary should remain protected in migration summaries.");
			context.Expect(plan.formatted.find("C# gameplay callbacks") != std::string::npos, "Scripting protected fallback diagnostics should explain the managed boundary.");
		});

		registry.Add("Scripting.ComponentAccess.ComponentWrapperGenerationChangesOnStructureEdit", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			Object object = ObjectManager::instance().createObject("Cache Target");
			const std::uint64_t beforeGeneration = ObjectManager::instance().getObjectComponentGeneration(object);

			ObjectManager::instance().AddComponent<ScriptComponent>(object);
			ObjectManager::instance().notifyObjectStructureChanged(object);
			const std::uint64_t afterGeneration = ObjectManager::instance().getObjectComponentGeneration(object);

			context.Expect(afterGeneration > beforeGeneration, "Managed component wrapper caches should have a native generation that changes after structural component edits.");
		});

		registry.Add("Scripting.ComponentAccess.ObjectNameCacheGenerationChangesOnRename", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			Object object = ObjectManager::instance().createObject("Cache Target");
			const std::uint64_t beforeGeneration = ObjectManager::instance().getObjectMetadataGeneration(object);

			ObjectManager::instance().setObjectName(object, "Renamed Cache Target");
			const std::uint64_t afterGeneration = ObjectManager::instance().getObjectMetadataGeneration(object);

			context.Expect(afterGeneration > beforeGeneration, "Managed Object.Name caches should invalidate when native object names change.");
		});

		registry.Add("Scripting.ComponentAccess.BindingPresenceGenerationChangesOnScriptBindingEdit", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			Object object = ObjectManager::instance().createObject("Binding Target");
			const std::uint64_t beforeGeneration = ObjectManager::instance().getObjectBindingGeneration(object);

			ScriptComponent& component = *ObjectManager::instance().AddComponent<ScriptComponent>(object);
			component.AddBinding(TerraEngine::EMPTY_UUID, "Game.BindingProbe");
			ObjectManager::instance().notifyObjectStructureChanged(object);
			const std::uint64_t afterGeneration = ObjectManager::instance().getObjectBindingGeneration(object);

			context.Expect(afterGeneration > beforeGeneration, "Managed HasComponent caches for script bindings should invalidate when bindings change.");
		});

		registry.Add("Scripting.ComponentAccess.FrameReadViewCapturesLookupGeneration", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			CreateScriptedRuntimeObject("Reader", "Game.Reader");

			const ScriptingSystem::ScriptUpdateWorkload workload = ScriptingSystem::Instance().BuildScriptUpdateWorkload(0.016f, false);
			const ScriptingSystem::ScriptFrameReadView readView = ScriptingSystem::Instance().BuildScriptFrameReadView(workload);

			context.ExpectEqual(workload.readViewVersion, readView.readViewVersion, "Read views should carry the workload generation used by workers.");
			context.ExpectEqual(ObjectManager::instance().GetSpatialRevision(), readView.lookupIndexGeneration, "Read views should record the lookup cache generation visible to scripts.");
			context.ExpectEqual(static_cast<std::size_t>(1), readView.scriptRecords.size(), "Read views should expose the script record workers are about to execute.");
			context.Expect(readView.scriptRecords.front().componentGeneration != 0, "Read views should carry component generations for managed wrapper caches.");
			context.Expect(readView.scriptRecords.front().bindingGeneration != 0, "Read views should carry binding generations for managed HasComponent caches.");
		});

		registry.Add("Scripting.ObjectMetadata.WorkerWritesPublishOnlyAtProtectedFlush", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Metadata Target");
			const std::uint32_t objectId = static_cast<std::uint32_t>(object.EntityId());
			Native_Scripting_PushDispatchContext(
				objectId, "Game.MetadataWriter", 7U, 1, 0,
				objects.getObjectMetadataGeneration(object),
				objects.getObjectComponentGeneration(object),
				objects.getObjectBindingGeneration(object));
			Native_Object_SetTag(objectId, "Player");
			Native_Object_SetLayer(objectId, "Gameplay");
			Native_Object_SetActive(objectId, false);
			Native_Object_SetStatic(objectId, true);
			Native_Scripting_PopDispatchContext();

			context.ExpectEqual(std::string("Untagged"), objects.getObjectTag(object),
				"Worker metadata must retain the pre-flush tag in live ObjectManager state.");
			context.Expect(objects.isObjectActive(object),
				"Worker metadata must not disable an object before the protected scene boundary.");
			context.ExpectEqual(static_cast<std::size_t>(4U), TerraEngine::Scene::SceneMutationQueue::Instance().Size(),
				"Each distinct metadata field should publish one typed deferred request.");

			const auto result = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "managed-metadata-test");
			context.Expect(result.applied, "The protected scene boundary should accept valid managed metadata writes.");
			context.ExpectEqual(std::string("Player"), objects.getObjectTag(object), "The tag should become visible after the protected flush.");
			context.ExpectEqual(std::string("Gameplay"), objects.getObjectLayer(object), "The layer should become visible after the protected flush.");
			context.Expect(!objects.isObjectActive(object), "The authored active flag should become visible after the protected flush.");
			context.Expect(objects.isObjectStatic(object), "The static flag should become visible after the protected flush.");
		});

		registry.Add("Scripting.PhysicsAuthoring.FixedWidthReflectionRoundTrips", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			RigidBodyComponent::registerType();
			CharacterControllerComponent::registerType();
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Physics Authoring Target");
			objects.AddComponent<RigidBodyComponent>(object);
			objects.AddComponent<CharacterControllerComponent>(object);
			const std::uint32_t objectId = static_cast<std::uint32_t>(object.EntityId());

			std::uint32_t motionType = static_cast<std::uint32_t>(TerraEngine::Physics::PhysicsMotionType::Kinematic);
			Native_SetComponentProperty(objectId, "RigidBodyComponent", "motionType", &motionType, 3);
			std::uint32_t readMotionType = 0U;
			Native_GetComponentProperty(objectId, "RigidBodyComponent", "motionType", &readMotionType, 3);
			context.ExpectEqual(motionType, readMotionType, "Rigid-body enums should round-trip through the fixed-width uint bridge.");

			glm::vec3 initialVelocity(3.0F, 4.0F, 5.0F);
			Native_SetComponentProperty(objectId, "RigidBodyComponent", "initialLinearVelocity", &initialVelocity, 5);
			glm::vec3 readVelocity(0.0F);
			Native_GetComponentProperty(objectId, "RigidBodyComponent", "initialLinearVelocity", &readVelocity, 5);
			context.Expect(readVelocity == initialVelocity, "Engine-owned physics vectors should round-trip as managed Vector3 values.");

			std::uint32_t shape = static_cast<std::uint32_t>(TerraEngine::Physics::PhysicsCharacterShapeKind::Cylinder);
			Native_SetComponentProperty(objectId, "CharacterControllerComponent", "shapeKind", &shape, 3);
			std::uint32_t readShape = 0U;
			Native_GetComponentProperty(objectId, "CharacterControllerComponent", "shapeKind", &readShape, 3);
			context.ExpectEqual(shape, readShape, "Character shape enums should use the same fixed-width managed representation.");
		});

		registry.Add("Scripting.PhysicsAuthoring.CollectionAbiIsFixedAndRejectsPartialReplacement", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			ColliderSetComponent::registerType();
			ConstraintSetComponent::registerType();
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Managed Collection Target");
			ColliderSetComponent* colliderSet = objects.AddComponent<ColliderSetComponent>(object);
			ConstraintSetComponent* constraintSet = objects.AddComponent<ConstraintSetComponent>(object);
			const std::uint32_t objectId = static_cast<std::uint32_t>(object.EntityId());

			context.ExpectEqual(static_cast<std::size_t>(152U), sizeof(NativePhysicsColliderRecord),
				"Collider authoring packets should retain their fixed v1 copied layout.");
			context.ExpectEqual(static_cast<std::size_t>(488U), sizeof(NativePhysicsConstraintRecord),
				"Constraint authoring packets should retain their fixed v1 copied layout.");

			NativePhysicsColliderRecord collider{};
			collider.header = {kNativePhysicsAbiVersion, sizeof(collider)};
			collider.recordId = {1U, 2U};
			collider.recordVersion = TerraEngine::Physics::kColliderRecordSchemaVersion;
			collider.shapeKind = static_cast<std::uint32_t>(TerraEngine::Physics::PhysicsShapeKind::Box);
			collider.localRotation.w = 1.0F;
			collider.halfExtents = {0.5F, 1.0F, 1.5F};
			collider.queryFlags = TerraEngine::Physics::kPhysicsColliderAllQueryFlags;
			collider.eventPolicy = static_cast<std::uint8_t>(TerraEngine::Physics::PhysicsColliderEventPolicy::All);
			collider.useDefaultMaterial = 1U;
			collider.enabled = 1U;
			collider.debugRed = collider.debugGreen = collider.debugBlue = collider.debugAlpha = 1.0F;

			context.Expect(Native_Physics_ReplaceColliderRecords(objectId, &collider, 1U) == NativePhysicsStatus::Success,
				"A valid collider candidate should replace the complete live collection.");
			context.ExpectEqual(static_cast<std::size_t>(1U), colliderSet->colliders.size(),
				"A successful managed replacement should publish exactly one collider.");
			NativePhysicsColliderRecord duplicateColliders[2] = {collider, collider};
			context.Expect(Native_Physics_ReplaceColliderRecords(objectId, duplicateColliders, 2U) == NativePhysicsStatus::InvalidArgument,
				"Duplicate record IDs should reject the complete managed collider candidate.");
			context.ExpectEqual(static_cast<std::size_t>(1U), colliderSet->colliders.size(),
				"A rejected collider replacement must leave the prior live collection unchanged.");

			NativePhysicsConstraintRecord constraint{};
			constraint.header = {kNativePhysicsAbiVersion, sizeof(constraint)};
			constraint.recordId = {3U, 4U};
			constraint.recordVersion = TerraEngine::Physics::kConstraintRecordSchemaVersion;
			constraint.kind = static_cast<std::uint32_t>(TerraEngine::Physics::PhysicsConstraintKind::Fixed);
			constraint.firstEndpointKind = static_cast<std::uint32_t>(TerraEngine::Physics::PhysicsConstraintEndpointKind::ConnectedObject);
			constraint.secondEndpointKind = static_cast<std::uint32_t>(TerraEngine::Physics::PhysicsConstraintEndpointKind::WorldAnchor);
			constraint.firstObjectId = {5U, 6U};
			constraint.firstFrame.rotation.w = 1.0F;
			constraint.secondFrame.rotation.w = 1.0F;
			constraint.firstAxis.y = 1.0F;
			constraint.secondAxis.y = 1.0F;
			constraint.gearRatio = 1.0F;
			constraint.breakForce = std::numeric_limits<float>::infinity();
			constraint.breakTorque = std::numeric_limits<float>::infinity();
			constraint.resetGeneration = 1U;
			constraint.enabled = 1U;

			context.Expect(Native_Physics_ReplaceConstraintRecords(objectId, &constraint, 1U) == NativePhysicsStatus::Success,
				"A valid constraint candidate should replace the complete live collection.");
			context.ExpectEqual(static_cast<std::size_t>(1U), constraintSet->constraints.size(),
				"A successful managed replacement should publish exactly one constraint.");
			std::uint32_t copiedCount = 0U;
			context.Expect(Native_Physics_CopyConstraintRecords(objectId, nullptr, 0U, &copiedCount) == NativePhysicsStatus::LimitExceeded
				&& copiedCount == 1U, "The two-call copy contract should report the required constraint capacity without borrowing storage.");
		});

		registry.Add("Scripting.PhysicsAuthoring.WorkerCollectionReplacementUsesTransactionalVisibility", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			ColliderSetComponent::registerType();
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Deferred Collider Target");
			ColliderSetComponent* colliderSet = objects.AddComponent<ColliderSetComponent>(object);
			const std::uint32_t objectId = static_cast<std::uint32_t>(object.EntityId());
			NativePhysicsColliderRecord collider{};
			collider.header = {kNativePhysicsAbiVersion, sizeof(collider)};
			collider.recordId = {10U, 11U};
			collider.recordVersion = TerraEngine::Physics::kColliderRecordSchemaVersion;
			collider.shapeKind = static_cast<std::uint32_t>(TerraEngine::Physics::PhysicsShapeKind::Sphere);
			collider.localRotation.w = 1.0F;
			collider.radius = 0.75F;
			collider.queryFlags = TerraEngine::Physics::kPhysicsColliderAllQueryFlags;
			collider.eventPolicy = static_cast<std::uint8_t>(TerraEngine::Physics::PhysicsColliderEventPolicy::All);
			collider.useDefaultMaterial = 1U;
			collider.enabled = 1U;

			Native_Scripting_PushDispatchContext(objectId, "Game.ColliderWriter", 42U, 1, 0,
				objects.getObjectMetadataGeneration(object), objects.getObjectComponentGeneration(object), objects.getObjectBindingGeneration(object));
			const NativePhysicsStatus queued = Native_Physics_ReplaceColliderRecords(objectId, &collider, 1U);
			Native_Scripting_PopDispatchContext();

			context.Expect(queued == NativePhysicsStatus::Accepted, "Worker collider authoring should acknowledge a copied deferred transaction.");
			context.Expect(colliderSet->colliders.empty(), "Deferred collider records must remain invisible before the protected flush.");
			context.ExpectEqual(static_cast<std::size_t>(1U), TerraEngine::Scene::SceneMutationQueue::Instance().PendingTransactions().size(),
				"One complete collection replacement should enqueue one typed transaction.");
			const auto accepted = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "managed-collider-visibility-test");
			context.Expect(accepted.applied && colliderSet->colliders.size() == 1U,
				"The protected flush should publish the complete collider collection at once.");

			collider.radius = 1.25F;
			Native_Scripting_PushDispatchContext(objectId, "Game.ColliderWriter", 43U, 1, 0,
				objects.getObjectMetadataGeneration(object), objects.getObjectComponentGeneration(object), objects.getObjectBindingGeneration(object));
			Native_Physics_ReplaceColliderRecords(objectId, &collider, 1U);
			Native_Scripting_PopDispatchContext();
			objects.notifyObjectStructureChanged(object);
			const auto conflicted = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "managed-collider-conflict-test");
			context.Expect(!conflicted.transactionResults.empty()
				&& conflicted.transactionResults.front().status == TerraEngine::Scene::TransactionStatus::Conflicted,
				"A component-generation change should conflict a stale collection replacement.");
			context.ExpectEqual(0.75F, std::get<TerraEngine::Physics::SphereColliderShape>(colliderSet->colliders.front().shape).radius,
				"A conflicted replacement must preserve the complete previously published collider list.");
		});

		registry.Add("Scripting.GraphicsAbi.FixedPacketRejectsLayoutDrift", [](TestContext& context) {
			context.ExpectEqual(1U, Native_Graphics_GetAbiVersion(), "The managed graphics bridge should advertise its pinned ABI version.");
			context.ExpectEqual(static_cast<std::size_t>(120U), sizeof(NativeGraphicsRenderSettings), "The flattened graphics packet should retain its fixed copied size.");
			context.ExpectEqual(static_cast<std::size_t>(96U), offsetof(NativeGraphicsRenderSettings, virtualGeometryBudgetBytes), "The 64-bit virtual-geometry budget should retain its aligned ABI offset.");

			NativeGraphicsRenderSettings settings{};
			settings.header = { kNativeGraphicsAbiVersion, sizeof(settings) };
			context.Expect(Native_Graphics_ReadRenderSettings(&settings) == NativeGraphicsStatus::Success,
				"A correctly sized graphics packet should receive a complete settings snapshot.");
			context.ExpectEqual(kNativeGraphicsAbiVersion, settings.header.abiVersion, "The native read should preserve explicit ABI metadata.");

			settings.header.abiVersion = 999U;
			context.Expect(Native_Graphics_SubmitRuntimeRenderSettings(&settings) == NativeGraphicsStatus::VersionMismatch,
				"An unknown graphics ABI must be rejected before it can enter the runtime settings queue.");
			context.Expect(Native_Graphics_SubmitRuntimeQualityPreset(99U) == NativeGraphicsStatus::InvalidPreset,
				"Unknown quality presets should be rejected without changing renderer state.");
		});

		registry.Add("Scripting.GraphicsApi.ManagedSurfaceMatchesNativeFoundation", [](TestContext& context) {
			const auto readSource = [](const char* relativePath) {
				std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
			};
			const std::string graphics = readSource("Engine/Managed/Source/Graphics.cs");
			const std::string component = readSource("Engine/Managed/Source/Component.cs");
			const std::string light = readSource("Engine/Managed/Source/Light.cs");
			const std::string sky = readSource("Engine/Managed/Source/Sky.cs");
			const std::string mesh = readSource("Engine/Managed/Source/MeshRenderer.cs");
			const std::string rigidbody = readSource("Engine/Managed/Source/Physics/Rigidbody.cs");
			const std::string character = readSource("Engine/Managed/Source/Physics/CharacterController.cs");
			const std::string colliderSet = readSource("Engine/Managed/Source/Physics/ColliderSet.cs");
			const std::string constraintSet = readSource("Engine/Managed/Source/Physics/ConstraintSet.cs");
			const std::string object = readSource("Engine/Managed/Source/Object.cs");

			context.Expect(graphics.find("internal const uint Size = 120") != std::string::npos,
				"The managed graphics mirror should declare the same fixed packet size as native code.");
			context.Expect(graphics.find("Native_Graphics_SetRenderSettings") == std::string::npos,
				"Managed graphics should never call the removed raw native-structure setter.");
			context.Expect(graphics.find("VirtualGeometrySettings") != std::string::npos,
				"Game-wide C# graphics settings should include the native virtual-geometry profile.");
			context.Expect(component.find("public bool Enabled") != std::string::npos,
				"Every managed component should expose the native shared active flag.");
			context.Expect(light.find("public bool CastsShadows") != std::string::npos
				&& light.find("public LightType Type") != std::string::npos,
				"The Light wrapper should expose authored type and shadow settings.");
			context.Expect(sky.find("public class Sky : Component") != std::string::npos
				&& sky.find("public string SourceAsset") != std::string::npos,
				"Sky authoring should have an official managed component wrapper.");
			context.Expect(mesh.find("TaskShaderCullingEnabled") != std::string::npos,
				"MeshRenderer should expose its author-facing culling toggle.");
			context.Expect(rigidbody.find("AllowedDegreesOfFreedom") != std::string::npos
				&& rigidbody.find("CollisionMaskOverride") != std::string::npos,
				"Rigidbody should expose its complete scalar authoring configuration.");
			context.Expect(character.find("MaximumSlopeAngleDegrees") != std::string::npos
				&& character.find("RotationPolicy") != std::string::npos,
				"CharacterController should expose traversal, filtering, and rotation authoring.");
			context.Expect(colliderSet.find("public ColliderDefinition[] Records") != std::string::npos
				&& colliderSet.find("ReplaceRecords(ReadOnlySpan<ColliderDefinition>") != std::string::npos,
				"ColliderSet should expose copied reads and one-call atomic collection replacement.");
			context.Expect(constraintSet.find("public ConstraintDefinition[] Records") != std::string::npos
				&& constraintSet.find("PhysicsConstraintAxisSettings TranslationX") != std::string::npos,
				"ConstraintSet should expose scalar and six-DOF authoring through copied managed records.");
			context.Expect(object.find("public string Tag") != std::string::npos
				&& object.find("public bool ActiveInHierarchy") != std::string::npos,
				"Managed objects should expose authored metadata and effective hierarchy activity.");
		});

		registry.Add("Scripting.GraphicsApi.RenderBoundaryDeclaresRuntimeSettingsQueue", [](TestContext& context) {
			RenderSystem& renderer = RenderSystem::Instance();
			const TerraEngine::Scheduling::SystemDeclaration declaration = renderer.SchedulerDeclaration();
			context.Expect(std::find(declaration.reads.begin(), declaration.reads.end(), TerraEngine::Scheduling::kResourceRuntimeRenderSettingsQueue) != declaration.reads.end(),
				"RenderSystem should declare the managed runtime-settings queue as a read resource.");
			context.Expect(std::find(declaration.writes.begin(), declaration.writes.end(), TerraEngine::Scheduling::kResourceRuntimeRenderSettings) != declaration.writes.end(),
				"RenderSystem should declare protected publication of effective runtime settings.");
			context.Expect(declaration.protectedCommit.find("Runtime render-settings queue application") != std::string::npos,
				"Scheduler diagnostics should identify where C# graphics settings become visible.");
		});

		registry.Add("Scripting.RuntimeApi.FixedPacketsAndProtectedMutationsRemainCoherent", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			const auto runtimeRequest = [](NativeRuntimeOperation operation) {
				NativeRuntimeRequest request{};
				request.header = {kNativeRuntimeAbiVersion, sizeof(request)};
				request.operation = operation;
				return request;
			};
			context.ExpectEqual(static_cast<std::size_t>(80U), sizeof(NativePhysicsCollisionLayer),
				"Each managed collision layer should retain its fixed copied layout.");
			context.ExpectEqual(static_cast<std::size_t>(2872U), sizeof(NativePhysicsSettingsRecord),
				"The complete physics configuration should retain its pinned v1 ABI size.");

			NativePhysicsSettingsRecord physics{};
			physics.header = {kNativePhysicsAbiVersion, sizeof(physics)};
			NativeRuntimeRequest readPhysics = runtimeRequest(NativeRuntimeOperation::PhysicsReadSettings);
			context.Expect(Native_Runtime_Invoke(&readPhysics, &physics) == NativeRuntimeStatus::Success,
				"Managed physics should read a complete copied configuration.");
			const float originalStep = physics.fixedTimeStep;
			physics.fixedTimeStep = 0.0F;
			NativeRuntimeRequest submitPhysics = runtimeRequest(NativeRuntimeOperation::PhysicsSubmitSettings);
			context.Expect(Native_Runtime_Invoke(&submitPhysics, &physics) == NativeRuntimeStatus::InvalidArgument,
				"Invalid world settings should reject before entering the protected physics queue.");
			NativePhysicsSettingsRecord unchanged{};
			unchanged.header = {kNativePhysicsAbiVersion, sizeof(unchanged)};
			Native_Runtime_Invoke(&readPhysics, &unchanged);
			context.ExpectEqual(originalStep, unchanged.fixedTimeStep,
				"Rejected physics settings must leave the complete effective configuration unchanged.");

			ObjectManager& objects = ObjectManager::instance();
			Object parent = objects.createObject("Managed Parent");
			Object child = objects.createObject("Managed Child");
			const std::uint32_t parentId = static_cast<std::uint32_t>(parent.EntityId());
			const std::uint32_t childId = static_cast<std::uint32_t>(child.EntityId());
			NativeRuntimeRequest setParent = runtimeRequest(NativeRuntimeOperation::ObjectSetParent);
			setParent.objectId = childId;
			setParent.secondaryId = parentId;
			context.Expect(Native_Runtime_Invoke(&setParent, nullptr) == NativeRuntimeStatus::Accepted,
				"A typed hierarchy request should enter the transaction queue.");
			NativeRuntimeRequest getParent = runtimeRequest(NativeRuntimeOperation::ObjectGetParent);
			getParent.objectId = childId;
			std::uint32_t observedParent = 0U;
			Native_Runtime_Invoke(&getParent, &observedParent);
			context.Expect(observedParent == std::numeric_limits<std::uint32_t>::max(),
				"Queued hierarchy changes should stay invisible before the protected scene flush.");
			const auto hierarchyResult = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "managed-hierarchy-api-test");
			Native_Runtime_Invoke(&getParent, &observedParent);
			context.Expect(hierarchyResult.applied && observedParent == parentId,
				"The protected scene flush should publish a typed hierarchy change atomically.");
			std::uint32_t requiredChildren = 0U;
			NativeRuntimeRequest copyChildren = runtimeRequest(NativeRuntimeOperation::ObjectCopyChildren);
			copyChildren.objectId = parentId;
			copyChildren.requiredCount = &requiredChildren;
			context.Expect(Native_Runtime_Invoke(&copyChildren, nullptr) == NativeRuntimeStatus::Conflict
				&& requiredChildren == 1U, "Hierarchy enumeration should use a bounded two-call copied contract.");

			NativeSceneRenderSettings renderSettings{};
			renderSettings.header = {kNativeRuntimeAbiVersion, sizeof(renderSettings)};
			NativeRuntimeRequest readRenderSettings = runtimeRequest(NativeRuntimeOperation::SceneReadRenderSettings);
			context.Expect(Native_Runtime_Invoke(&readRenderSettings, &renderSettings) == NativeRuntimeStatus::Success,
				"Scene rendering should expose the current scene-wide configuration.");
			const SceneRenderSettings originalRenderSettings = SceneManager::Instance().GetActiveSceneRenderSettings();
			renderSettings.indirectStrength = originalRenderSettings.indirectStrength + 0.25F;
			NativeRuntimeRequest submitRenderSettings = runtimeRequest(NativeRuntimeOperation::SceneSubmitRenderSettings);
			context.Expect(Native_Runtime_Invoke(&submitRenderSettings, &renderSettings) == NativeRuntimeStatus::Accepted,
				"Valid scene rendering changes should be copied into the protected request queue.");
			context.ExpectEqual(originalRenderSettings.indirectStrength,
				SceneManager::Instance().GetActiveSceneRenderSettings().indirectStrength,
				"Scene rendering changes should stay invisible before the protected runtime flush.");
			FlushManagedRuntimeRequests();
			context.ExpectEqual(renderSettings.indirectStrength,
				SceneManager::Instance().GetActiveSceneRenderSettings().indirectStrength,
				"The protected runtime flush should publish complete scene rendering settings.");
			NativeSceneRenderSettings restoreSettings{};
			restoreSettings.header = {kNativeRuntimeAbiVersion, sizeof(restoreSettings)};
			restoreSettings.indirectStrength = originalRenderSettings.indirectStrength;
			restoreSettings.ambientOcclusionStrength = originalRenderSettings.ambientOcclusionStrength;
			restoreSettings.ambientOcclusionQuality = originalRenderSettings.ambientOcclusionQuality;
			Native_Runtime_Invoke(&submitRenderSettings, &restoreSettings);
			FlushManagedRuntimeRequests();

			NativeInputSnapshot input{};
			input.header = {kNativeRuntimeAbiVersion, sizeof(input)};
			NativeRuntimeRequest readInput = runtimeRequest(NativeRuntimeOperation::InputReadSnapshot);
			context.Expect(Native_Runtime_Invoke(&readInput, &input) == NativeRuntimeStatus::Success,
				"Input should publish a copied immutable frame snapshot for managed workers.");
			context.Expect(input.keyCount <= kNativeInputKeyCapacity
				&& input.mouseButtonCount <= kNativeInputMouseButtonCapacity,
				"Input snapshots must stay inside the ABI's bounded arrays.");

			NativeRuntimeRequest setTimeScale = runtimeRequest(NativeRuntimeOperation::TimeSetScale);
			setTimeScale.scalar = -1.0F;
			context.Expect(Native_Runtime_Invoke(&setTimeScale, nullptr) == NativeRuntimeStatus::InvalidArgument,
				"Invalid time scaling should reject without changing frame timing.");
			setTimeScale.scalar = 0.5F;
			context.Expect(Native_Runtime_Invoke(&setTimeScale, nullptr) == NativeRuntimeStatus::Accepted,
				"Valid time scaling should queue for the next managed frame boundary.");
			ScriptingSystem::Instance().SetRunning(true);
			ScriptingSystem::Instance().BeginFrameTiming(0.02F);
			NativeTimeSnapshot time{};
			time.header = {kNativeRuntimeAbiVersion, sizeof(time)};
			NativeRuntimeRequest readTime = runtimeRequest(NativeRuntimeOperation::TimeReadSnapshot);
			context.Expect(Native_Runtime_Invoke(&readTime, &time) == NativeRuntimeStatus::Success,
				"Time should expose copied scaled and unscaled frame values.");
			context.ExpectEqual(0.01F, time.deltaTime, "The next frame should apply the queued managed time scale.");
			context.ExpectEqual(0.02F, time.unscaledDeltaTime, "Unscaled frame time should remain authoritative.");
			ScriptingSystem::Instance().SetRunning(false);
		});

		registry.Add("Scripting.RuntimeApi.ManagedSurfaceCoversRemainingGameConfiguration", [](TestContext& context) {
			const auto readSource = [](const char* relativePath) {
				std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
			};
			const std::string physics = readSource("Engine/Managed/Source/Physics/Physics.cs");
			const std::string object = readSource("Engine/Managed/Source/Object.cs");
			const std::string scene = readSource("Engine/Managed/Source/Scene.cs");
			const std::string input = readSource("Engine/Managed/Source/Input.cs");
			const std::string time = readSource("Engine/Managed/Source/Time.cs");
			const std::string resources = readSource("Engine/Managed/Source/ResourceStreaming.cs");
			context.Expect(physics.find("public static PhysicsConfiguration Configuration") != std::string::npos
				&& physics.find("ApplyConfiguration") != std::string::npos,
				"Physics should expose readable and writable complete configuration.");
			context.Expect(object.find("public Object? Parent") != std::string::npos
				&& object.find("public Object[] Children") != std::string::npos
				&& object.find("public static Object[] All") != std::string::npos
				&& object.find("public static ObjectCreationRequest Instantiate") != std::string::npos,
				"Objects should expose typed hierarchy and deterministic enumeration.");
			context.Expect(scene.find("InstantiatePrefab") != std::string::npos
				&& scene.find("public static SceneRenderSettings RenderSettings") != std::string::npos
				&& scene.find("public static RuntimeStatus LoadScene") != std::string::npos
				&& scene.find("LoadedScenes") != std::string::npos,
				"Scene should expose lifecycle, prefab, and scene-wide rendering controls.");
			context.Expect(input.find("public static unsafe InputSnapshot Snapshot") != std::string::npos
				&& input.find("WasKeyReleasedThisFrame") != std::string::npos,
				"Input should expose transition-complete immutable snapshots.");
			context.Expect(time.find("public static float TimeScale") != std::string::npos
				&& time.find("UnscaledElapsedTime") != std::string::npos,
				"Time should expose scaling plus scaled and unscaled clocks.");
			context.Expect(resources.find("ResourceStreamingStatus GetStatus") != std::string::npos
				&& resources.find("RuntimeStatus Release") != std::string::npos,
				"Resource streaming should expose readiness, diagnostics, residency bytes, and deferred release.");
		});

		registry.Add("Scripting.RuntimeApi.DeferredObjectReceiptResolvesOnlyAfterPublication", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			NativeObjectCreationReceipt receipt{};
			receipt.header = {kNativeRuntimeAbiVersion, sizeof(receipt)};
			NativeRuntimeRequest create{};
			create.header = {kNativeRuntimeAbiVersion, sizeof(create)};
			create.operation = NativeRuntimeOperation::ObjectRequestCreate;
			create.text = "Deferred Managed Object";
			context.ExpectEqual(static_cast<std::size_t>(56U), sizeof(receipt), "Deferred object receipts should retain their pinned v1 ABI size.");
			context.Expect(Native_Runtime_Invoke(&create, &receipt) == NativeRuntimeStatus::Accepted,
				"Managed object creation should return a stable accepted receipt without touching live ECS storage.");

			NativeRuntimeRequest resolve{};
			resolve.header = {kNativeRuntimeAbiVersion, sizeof(resolve)};
			resolve.operation = NativeRuntimeOperation::ObjectResolveStableId;
			resolve.text = receipt.stableId;
			std::uint32_t objectId = std::numeric_limits<std::uint32_t>::max();
			context.Expect(Native_Runtime_Invoke(&resolve, &objectId) == NativeRuntimeStatus::NotFound,
				"A creation receipt must remain unresolved before the protected mutation flush.");
			context.Expect(ObjectManager::instance().getRootObjects().empty(), "Deferred creation must not allocate a live entity early.");

			const TerraEngine::Scene::SceneMutationBatchResult result = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(ObjectManager::instance(), "managed-create-receipt-test");
			context.Expect(result.applied, "The protected mutation boundary should publish the accepted creation request.");
			context.Expect(Native_Runtime_Invoke(&resolve, &objectId) == NativeRuntimeStatus::Success,
				"The stable creation receipt should resolve after successful publication.");
			const std::optional<Object> object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId));
			context.Expect(object.has_value() && ObjectManager::instance().getObjectName(*object) == "Deferred Managed Object",
				"The resolved receipt should identify the object created by its queued request.");
		});

		registry.Add("Scripting.RuntimeApi.SceneLoadsPublishOnlyAtFrameEnd", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			SceneManager& scenes = SceneManager::Instance();
			scenes.UnloadAll();
			const std::filesystem::path root = std::filesystem::temp_directory_path() / "TerraEngineManagedSceneLifecycleTests";
			std::filesystem::create_directories(root);
			const std::filesystem::path scenePath = root / "Deferred.tescene";
			{
				std::ofstream output(scenePath, std::ios::binary | std::ios::trunc);
				output << R"({"name":"Deferred Managed Scene","sceneSettings":{},"objects":[]})";
			}
			scenes.SetSceneRoot(root);

			NativeRuntimeRequest load{};
			load.header = {kNativeRuntimeAbiVersion, sizeof(load)};
			load.operation = NativeRuntimeOperation::SceneRequestLoad;
			const std::string scenePathText = scenePath.string();
			load.text = scenePathText.c_str();
			context.Expect(Native_Runtime_Invoke(&load, nullptr) == NativeRuntimeStatus::Accepted,
				"Managed scene loading should copy the path into the protected lifecycle queue.");
			context.Expect(scenes.GetLoadedSceneIds().empty(), "Queued scene loading must not publish cache state during a managed callback.");

			FlushManagedRuntimeRequests();
			const std::vector<TerraEngine::UUID> loaded = scenes.GetLoadedSceneIds();
			context.ExpectEqual(static_cast<std::size_t>(1U), loaded.size(), "The frame-end lifecycle boundary should publish the queued scene load.");
			if(!loaded.empty()) {
				const std::string loadedId = TerraEngine::ToHexString(loaded.front());
				NativeRuntimeRequest activate{};
				activate.header = {kNativeRuntimeAbiVersion, sizeof(activate)};
				activate.operation = NativeRuntimeOperation::SceneRequestSetActive;
				activate.text = loadedId.c_str();
				context.Expect(Native_Runtime_Invoke(&activate, nullptr) == NativeRuntimeStatus::Accepted,
					"A loaded scene should enter the protected active-scene queue.");
				context.Expect(scenes.GetActiveSceneId() != loaded.front(), "Active scene replacement must remain invisible before frame end.");
				FlushManagedRuntimeRequests();
				context.Expect(scenes.GetActiveSceneId() == loaded.front(), "Frame end should atomically publish the queued active scene.");
			}

			scenes.UnloadAll();
			ObjectManager::instance().clear();
			std::filesystem::remove_all(root);
		});
	}
}
