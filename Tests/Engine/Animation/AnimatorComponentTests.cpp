#include "TestHarness.h"

#include "Components/AnimatorComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/Serializer.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>

namespace TerraEngine::Tests {
	namespace {
		// Runtime animation ownership must stay outside authored components. These
		// concepts make accidental handle, pose, or effective-mode members a
		// compile-time contract failure instead of a scene-format regression.
		template<typename ComponentType>
		concept HasRuntimeInstanceHandle = requires(ComponentType component) {
			component.runtimeInstanceHandle;
		};

		template<typename ComponentType>
		concept HasRuntimePoseState = requires(ComponentType component) {
			component.runtimePose;
		};

		template<typename ComponentType>
		concept HasEffectiveMode = requires(ComponentType component) {
			component.effectiveMode;
		};

		static_assert(std::is_base_of_v<Component<AnimatorComponent>, AnimatorComponent>,
			"AnimatorComponent must use the engine's CRTP component lifecycle.");
		static_assert(AnimatorComponent::kComponentName == std::string_view("AnimatorComponent"),
			"AnimatorComponent needs a stable reflected and serialized name.");
		static_assert(static_cast<std::uint8_t>(Animation::AnimationExecutionPolicy::Auto) == 0U
			&& static_cast<std::uint8_t>(Animation::AnimationExecutionPolicy::ForceGpuVisual) == 1U
			&& static_cast<std::uint8_t>(Animation::AnimationExecutionPolicy::ForceHybridAuthoritative) == 2U
			&& static_cast<std::uint8_t>(Animation::AnimationExecutionPolicy::ForceCpuDeterministicFallback) == 3U,
			"Authored execution-policy values are durable and append-only.");
		static_assert(!HasRuntimeInstanceHandle<AnimatorComponent>,
			"AnimationSystem, not authored scene data, owns runtime instance handles.");
		static_assert(!HasRuntimePoseState<AnimatorComponent>,
			"Transient evaluated poses must never be stored on AnimatorComponent.");
		static_assert(!HasEffectiveMode<AnimatorComponent>,
			"Effective execution mode is runtime output and must not be authored.");

		TerraEngine::UUID MakeUuid(std::uint8_t fillValue)
		{
			TerraEngine::UUID uuid{};
			uuid.fill(fillValue);
			return uuid;
		}

		bool HasReflectedProperty(std::string_view propertyName)
		{
			const TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType<AnimatorComponent>();
			return TerraEngine::Reflection::IsValid(
				TerraEngine::Reflection::FindProperty(type, propertyName));
		}

		void ResetObjectManager()
		{
			// Tests share the singleton registry, so clear live objects before and
			// after each case while leaving process-wide reflection metadata intact.
			ObjectManager::instance().clear();
		}

		nlohmann::json MakeAuthoredAnimatorObjectJson()
		{
			return {
				{"uuid", TerraEngine::ToHexString(MakeUuid(31U))},
				{"name", "Serialized Animator"},
				{"tag", "Untagged"},
				{"layer", "Default"},
				{"active", true},
				{"isStatic", false},
				{"components", {
					{"AnimatorComponent", {
						{"active", true},
						{"schemaVersion", Animation::kAnimatorComponentSchemaVersion},
						{"controllerAssetId", TerraEngine::ToHexString(MakeUuid(1U))},
						{"defaultClipAssetId", TerraEngine::ToHexString(MakeUuid(7U))},
						{"loopDefaultClip", false},
						{"playAutomatically", false},
						{"defaultClipStartFrame", 17U},
						{"skeletonOverrideAssetId", TerraEngine::ToHexString(MakeUuid(2U))},
						{"avatarOverrideAssetId", TerraEngine::ToHexString(MakeUuid(3U))},
						{"retargetProfileOverrideAssetId", TerraEngine::ToHexString(MakeUuid(4U))},
						{"ragdollProfileOverrideAssetId", TerraEngine::ToHexString(MakeUuid(5U))},
						{"motionDatabaseOverrideAssetId", TerraEngine::ToHexString(MakeUuid(6U))},
						{"requestedMode", static_cast<std::uint32_t>(Animation::AnimationExecutionPolicy::ForceHybridAuthoritative)},
						{"rootMotionPolicy", static_cast<std::uint32_t>(Animation::AnimatorRootMotionPolicy::Ignore)},
						{"updatePolicy", static_cast<std::uint32_t>(Animation::AnimatorUpdatePolicy::FixedStep)},
						{"timePolicy", static_cast<std::uint32_t>(Animation::AnimatorTimePolicy::Fixed)},
						{"initialParameters", R"({"Grounded":true,"Speed":2.5})"},
						{"eventPolicy", static_cast<std::uint32_t>(Animation::AnimatorEventPolicy::Dispatch)},
						{"cullingPolicy", static_cast<std::uint32_t>(Animation::AnimatorCullingPolicy::Automatic)},
						{"diagnosticOverrides", 3U},
						// These hostile keys model stale runtime data from an invalid
						// producer. Deserialization must ignore every one of them.
						{"runtimeInstanceHandle", 17U},
						{"instanceGeneration", 99U},
						{"effectiveMode", "GpuVisual"},
						{"playbackTick", 1234},
						{"poseCache", {1, 2, 3}},
						{"gpuPaletteAllocation", 42U}
					}}
				}}
			};
		}

		void ExpectNoTransientRuntimeState(TestContext& context, const nlohmann::json& animatorJson)
		{
			constexpr std::string_view transientNames[] = {
				"runtimeInstanceHandle",
				"instanceGeneration",
				"effectiveMode",
				"playbackTick",
				"poseCache",
				"gpuPaletteAllocation",
				"residencyState",
				"recordingBuffer"
			};
			for(const std::string_view name : transientNames) {
				context.Expect(!animatorJson.contains(std::string(name)),
					"Serialized AnimatorComponent must omit transient runtime property '" + std::string(name) + "'.");
			}
		}
	}

	void RegisterAnimatorComponentTests(TestRegistry& registry)
	{
		registry.Add("Animation.Component.CrtpNameAndAuthoredReflectionContract", [](TestContext& context) {
			// Exercise the real startup registration path. A fresh editor must expose
			// AnimatorComponent before any authored scene triggers serializer fallback.
			ComponentRegistry::RegisterAllComponents();
			{
				std::lock_guard lock(ComponentAccessRegistryMutex());
				context.Expect(ComponentAccessRegistry().contains("AnimatorComponent"),
					"Global startup registration must expose AnimatorComponent to runtime/editor access.");
			}
			// EnTT's default meta context is module-local on Windows. Register the
			// header-defined type in this test module as well before querying it here;
			// the engine registry assertion above verifies the shared runtime path.
			AnimatorComponent::registerType();
			const TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType<AnimatorComponent>();
			context.Expect(TerraEngine::Reflection::IsValid(type),
				"AnimatorComponent registration should publish EnTT reflection metadata.");

			constexpr std::string_view authoredProperties[] = {
				"active",
				"schemaVersion",
				"controllerAssetId",
				"defaultClipAssetId",
				"loopDefaultClip",
				"playAutomatically",
				"defaultClipStartFrame",
				"skeletonOverrideAssetId",
				"avatarOverrideAssetId",
				"retargetProfileOverrideAssetId",
				"ragdollProfileOverrideAssetId",
				"motionDatabaseOverrideAssetId",
				"requestedMode",
				"rootMotionPolicy",
				"updatePolicy",
				"timePolicy",
				"initialParameters",
				"eventPolicy",
				"cullingPolicy",
				"diagnosticOverrides"
			};
			for(const std::string_view name : authoredProperties) {
				context.Expect(HasReflectedProperty(name),
					"AnimatorComponent should reflect authored property '" + std::string(name) + "'.");
			}

			context.Expect(!HasReflectedProperty("effectiveMode"),
				"Read-only effective mode belongs to the runtime instance, not authored reflection.");
			context.Expect(!HasReflectedProperty("runtimeInstanceHandle"),
				"Runtime instance handles must not enter reflected scene authoring data.");
			context.Expect(!HasReflectedProperty("runtimePose"),
				"Transient pose buffers must not enter reflected scene authoring data.");
		});

		registry.Add("Animation.Component.NewAnimatorDefaultsToAuto", [](TestContext& context) {
			ResetObjectManager();
			AnimatorComponent::registerType();
			Object object = ObjectManager::instance().createObject("Default Animator");
			AnimatorComponent* animator = ObjectManager::instance().AddComponent<AnimatorComponent>(object);
			context.Expect(animator != nullptr, "A valid object should accept an AnimatorComponent.");
			context.Expect(animator != nullptr
				&& animator->requestedMode == Animation::AnimationExecutionPolicy::Auto,
				"Every newly authored animator must request Auto unless an advanced override is persisted.");
			context.Expect(animator != nullptr && animator->loopDefaultClip,
				"New direct-clip animators should loop by default for useful imported-model playback.");
			context.Expect(animator != nullptr && animator->playAutomatically,
				"New animators should begin playback automatically unless a creator disables it.");
			context.Expect(animator != nullptr && animator->defaultClipStartFrame == 0U,
				"New direct-clip animators should begin at frame zero until a creator chooses another start pose.");

			const nlohmann::json objectJson = Serializer::SerializeObject(object);
			const nlohmann::json& animatorJson = objectJson.at("components").at("AnimatorComponent");
			context.ExpectEqual(static_cast<std::uint32_t>(Animation::AnimationExecutionPolicy::Auto),
				animatorJson.at("requestedMode").get<std::uint32_t>(),
				"The serialized default should remain the stable numeric Auto policy.");
			context.Expect(animatorJson.at("loopDefaultClip").get<bool>(),
				"The default loop choice must be durable scene authoring data.");
			context.Expect(animatorJson.at("playAutomatically").get<bool>(),
				"The automatic-play choice must be durable scene authoring data.");
			context.Expect(animatorJson.at("defaultClipStartFrame").get<std::uint32_t>() == 0U,
				"The default start frame must be durable scene authoring data.");
			ExpectNoTransientRuntimeState(context, animatorJson);

			// Older or minimally authored payloads have no explicit override. A
			// missing property must retain the constructor's Auto default.
			ResetObjectManager();
			nlohmann::json implicitAutoJson = MakeAuthoredAnimatorObjectJson();
			implicitAutoJson["components"]["AnimatorComponent"].erase("requestedMode");
			implicitAutoJson["components"]["AnimatorComponent"].erase("loopDefaultClip");
			implicitAutoJson["components"]["AnimatorComponent"].erase("playAutomatically");
			implicitAutoJson["components"]["AnimatorComponent"].erase("defaultClipStartFrame");
			const entt::entity implicitAutoEntity = Serializer::DeserializeObjectEntity(implicitAutoJson);
			const std::optional<Object> implicitAutoObject =
				ObjectManager::instance().getObject(implicitAutoEntity);
			const AnimatorComponent* implicitAutoAnimator = implicitAutoObject
				? ObjectManager::instance().GetComponent<AnimatorComponent>(*implicitAutoObject)
				: nullptr;
			context.Expect(implicitAutoAnimator != nullptr
				&& implicitAutoAnimator->requestedMode == Animation::AnimationExecutionPolicy::Auto,
				"An animator without a persisted advanced override should load as Auto.");
			context.Expect(implicitAutoAnimator != nullptr
				&& implicitAutoAnimator->loopDefaultClip,
				"Older Animator payloads without a loop switch should preserve the looping default.");
			context.Expect(implicitAutoAnimator != nullptr
				&& implicitAutoAnimator->playAutomatically,
				"Older Animator payloads without an automatic-play switch should preserve historical autoplay.");
			context.Expect(implicitAutoAnimator != nullptr
				&& implicitAutoAnimator->defaultClipStartFrame == 0U,
				"Older Animator payloads without a start frame should migrate to frame zero.");
			ResetObjectManager();
		});

		registry.Add("Animation.Component.SerializationPersistsOnlyAuthoredState", [](TestContext& context) {
			ResetObjectManager();
			const entt::entity entity = Serializer::DeserializeObjectEntity(MakeAuthoredAnimatorObjectJson());
			context.Expect(entity != entt::null,
				"AnimatorComponent scene data should create an object through the canonical serializer path.");

			const std::optional<Object> object = ObjectManager::instance().getObject(entity);
			context.Expect(object.has_value(), "The deserialized AnimatorComponent object should remain live.");
			AnimatorComponent* animator = object
				? ObjectManager::instance().GetComponent<AnimatorComponent>(*object)
				: nullptr;
			context.Expect(animator != nullptr, "The deserializer should restore the authored AnimatorComponent.");
			context.Expect(animator != nullptr && animator->controllerAssetId == MakeUuid(1U),
				"A controller reference should survive without requiring the asset to be resident.");
			context.Expect(animator != nullptr && animator->defaultClipAssetId == MakeUuid(7U),
				"An imported direct-playback clip reference should survive until a controller is authored.");
			context.Expect(animator != nullptr && !animator->loopDefaultClip,
				"The authored one-shot loop choice should survive deserialization.");
			context.Expect(animator != nullptr && !animator->playAutomatically,
				"The authored automatic-play choice should survive deserialization.");
			context.Expect(animator != nullptr && animator->defaultClipStartFrame == 17U,
				"The authored direct-clip start frame should survive deserialization.");
			context.Expect(animator != nullptr
				&& animator->requestedMode == Animation::AnimationExecutionPolicy::ForceHybridAuthoritative,
				"An explicitly authored advanced mode request should round-trip.");

			const nlohmann::json serialized = Serializer::SerializeObject(entity);
			const nlohmann::json& animatorJson = serialized.at("components").at("AnimatorComponent");
			ExpectNoTransientRuntimeState(context, animatorJson);
			context.ExpectEqual(TerraEngine::ToHexString(MakeUuid(1U)),
				animatorJson.at("controllerAssetId").get<std::string>(),
				"Serialization should preserve the authored controller reference exactly.");
			context.ExpectEqual(TerraEngine::ToHexString(MakeUuid(7U)),
				animatorJson.at("defaultClipAssetId").get<std::string>(),
				"Serialization should preserve the direct-playback clip reference exactly.");
			context.Expect(!animatorJson.at("loopDefaultClip").get<bool>(),
				"Serialization should preserve the authored direct-clip loop choice.");
			context.Expect(!animatorJson.at("playAutomatically").get<bool>(),
				"Serialization should preserve the authored automatic-play choice.");
			context.Expect(animatorJson.at("defaultClipStartFrame").get<std::uint32_t>() == 17U,
				"Serialization should preserve the authored direct-clip start frame.");
			context.ExpectEqual(std::string(R"({"Grounded":true,"Speed":2.5})"),
				animatorJson.at("initialParameters").get<std::string>(),
				"Serialization should preserve authored initial parameters without adding runtime values.");
			ResetObjectManager();
		});

		registry.Add("Animation.Component.MalformedPoliciesFailClosedToSafeDefaults", [](TestContext& context) {
			ResetObjectManager();
			nlohmann::json malformed = MakeAuthoredAnimatorObjectJson();
			nlohmann::json& values = malformed["components"]["AnimatorComponent"];
			values["requestedMode"] = 255U;
			values["rootMotionPolicy"] = 255U;
			values["updatePolicy"] = 255U;
			values["timePolicy"] = 255U;
			values["eventPolicy"] = 255U;
			values["cullingPolicy"] = 255U;

			const entt::entity entity = Serializer::DeserializeObjectEntity(malformed);
			const std::optional<Object> object = ObjectManager::instance().getObject(entity);
			const AnimatorComponent* animator = object
				? ObjectManager::instance().GetComponent<AnimatorComponent>(*object)
				: nullptr;
			context.Expect(animator != nullptr, "Malformed policy input should still produce a safe authored component.");
			context.Expect(animator != nullptr
				&& animator->requestedMode == Animation::AnimationExecutionPolicy::Auto
				&& animator->rootMotionPolicy == Animation::AnimatorRootMotionPolicy::Ignore
				&& animator->updatePolicy == Animation::AnimatorUpdatePolicy::EveryFrame
				&& animator->timePolicy == Animation::AnimatorTimePolicy::Scaled
				&& animator->eventPolicy == Animation::AnimatorEventPolicy::Dispatch
				&& animator->cullingPolicy == Animation::AnimatorCullingPolicy::Automatic,
				"Unknown persisted enum values must fall back to the documented safe defaults.");
			ResetObjectManager();
		});

		registry.Add("Animation.Component.RuntimeOwnershipUsesExternalComponentGeneration", [](TestContext& context) {
			ResetObjectManager();
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Generation-safe Animator");
			objects.AddComponent<AnimatorComponent>(object);
			const std::uint64_t firstGeneration = objects.getObjectComponentGeneration(object);

			const nlohmann::json serialized = Serializer::SerializeObject(object);
			const nlohmann::json& animatorJson = serialized.at("components").at("AnimatorComponent");
			context.Expect(!animatorJson.contains("componentGeneration"),
				"Runtime component generations must not become durable authored state.");

			// Removing and recreating the component advances the manager-owned
			// generation. Runtime instance work captured from the first generation
			// can therefore be rejected without storing a handle on the component.
			objects.RemoveComponent<AnimatorComponent>(object);
			objects.AddComponent<AnimatorComponent>(object);
			const std::uint64_t replacementGeneration = objects.getObjectComponentGeneration(object);
			context.Expect(replacementGeneration > firstGeneration,
				"Replacing AnimatorComponent should advance the external component generation used by runtime ownership.");
			ResetObjectManager();
		});
	}
}
