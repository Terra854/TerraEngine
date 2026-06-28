#include "TestHarness.h"

#include "Components/ScriptComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Scripting/ScriptingSystem.h"
#include "ThreadPool/ThreadPool.h"

#include <cstdint>
#include <cstring>

namespace TerraEngine::Tests {
	namespace {
		constexpr std::uint64_t kScriptId = 0x12345678ULL;

		struct FakeScriptInstance {
			std::uint64_t scriptId = 0;
			std::uint32_t objectId = 0;
			float speed = 0.0f;
		};

		struct FakeExportRuntimeState {
			int scriptCountCalls = 0;
			int createCalls = 0;
			int awakeCalls = 0;
			int startCalls = 0;
			int updateCalls = 0;
			int batchUpdateCalls = 0;
			int batchItemCount = 0;
			int destroyCalls = 0;
			float lastDeltaTime = 0.0f;
			std::uint64_t lastFrameOrder = 0;
			std::uint64_t lastObjectGeneration = 0;
			std::uint64_t lastComponentGeneration = 0;
			std::uint64_t lastBindingGeneration = 0;
			FakeScriptInstance instance;
		};

		FakeExportRuntimeState g_FakeState;

		std::uint64_t HashFieldName(const char* text) {
			const std::uint64_t fnvOffsetBasis = 14695981039346656037ULL;
			const std::uint64_t fnvPrime = 1099511628211ULL;
			std::uint64_t hash = fnvOffsetBasis;
			for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text); *cursor != 0; ++cursor) {
				hash ^= static_cast<std::uint64_t>(*cursor);
				hash *= fnvPrime;
			}
			return hash;
		}

#ifdef _WIN32
#define TERRAENGINE_TEST_CALL __cdecl
#else
#define TERRAENGINE_TEST_CALL
#endif

		int32_t TERRAENGINE_TEST_CALL FakeGetScriptCount() {
			++g_FakeState.scriptCountCalls;
			return 1;
		}

		void TERRAENGINE_TEST_CALL FakeGetScriptIds(std::uint64_t* outBuffer, int32_t capacity) {
			if (outBuffer != nullptr && capacity > 0) {
				outBuffer[0] = kScriptId;
			}
		}

		void* TERRAENGINE_TEST_CALL FakeCreateScript(std::uint64_t scriptId, std::uint32_t objectId) {
			++g_FakeState.createCalls;
			g_FakeState.instance = {};
			g_FakeState.instance.scriptId = scriptId;
			g_FakeState.instance.objectId = objectId;
			return &g_FakeState.instance;
		}

		void TERRAENGINE_TEST_CALL FakeDestroyScript(void*) {
			++g_FakeState.destroyCalls;
		}

		void TERRAENGINE_TEST_CALL FakeInvokeAwake(void*) {
			++g_FakeState.awakeCalls;
		}

		void TERRAENGINE_TEST_CALL FakeInvokeStart(void*) {
			++g_FakeState.startCalls;
		}

		void TERRAENGINE_TEST_CALL FakeInvokeUpdate(void*, float deltaTime) {
			++g_FakeState.updateCalls;
			g_FakeState.lastDeltaTime = deltaTime;
		}

		void TERRAENGINE_TEST_CALL FakeInvokeUpdateBatch(const ExportScriptBatchItem* items, int32_t itemCount) {
			++g_FakeState.batchUpdateCalls;
			g_FakeState.batchItemCount = itemCount;
			if (items != nullptr && itemCount > 0) {
				++g_FakeState.updateCalls;
				g_FakeState.lastDeltaTime = items[0].deltaTime;
				g_FakeState.lastFrameOrder = items[0].frameOrder;
				g_FakeState.lastObjectGeneration = items[0].objectGeneration;
				g_FakeState.lastComponentGeneration = items[0].componentGeneration;
				g_FakeState.lastBindingGeneration = items[0].bindingGeneration;
			}
		}

		void TERRAENGINE_TEST_CALL FakeInvokeDisable(void*) {
		}

		int32_t TERRAENGINE_TEST_CALL FakeGetFieldCount(std::uint64_t scriptId) {
			return scriptId == kScriptId ? 1 : 0;
		}

		void TERRAENGINE_TEST_CALL FakeGetFieldDescriptors(std::uint64_t scriptId, ExportFieldDescriptor* outBuffer, int32_t capacity) {
			if (scriptId != kScriptId || outBuffer == nullptr || capacity <= 0) {
				return;
			}

			ExportFieldDescriptor descriptor{};
			descriptor.fieldIndex = 0;
			descriptor.valueKind = static_cast<int32_t>(ExportFieldValueKind::Float);
			descriptor.valueSize = sizeof(float);
			descriptor.nameHash = HashFieldName("Speed");
			std::strncpy(descriptor.nameUtf8, "Speed", sizeof(descriptor.nameUtf8) - 1U);
			outBuffer[0] = descriptor;
		}

		void TERRAENGINE_TEST_CALL FakeGetFieldValue(void* handle, int32_t fieldIndex, void* outValue, int32_t valueSize) {
			if (handle == nullptr || outValue == nullptr || fieldIndex != 0 || valueSize != sizeof(float)) {
				return;
			}

			const FakeScriptInstance* instance = static_cast<const FakeScriptInstance*>(handle);
			std::memcpy(outValue, &instance->speed, sizeof(instance->speed));
		}

		void TERRAENGINE_TEST_CALL FakeSetFieldValue(void* handle, int32_t fieldIndex, const void* inValue, int32_t valueSize) {
			if (handle == nullptr || inValue == nullptr || fieldIndex != 0 || valueSize != sizeof(float)) {
				return;
			}

			FakeScriptInstance* instance = static_cast<FakeScriptInstance*>(handle);
			std::memcpy(&instance->speed, inValue, sizeof(instance->speed));
		}

		ManagedVTable MakeFakeVTable() {
			ManagedVTable vtable{};
			vtable.version = 1;
			vtable.getScriptCount = &FakeGetScriptCount;
			vtable.getScriptIds = &FakeGetScriptIds;
			vtable.createScript = &FakeCreateScript;
			vtable.destroyScript = &FakeDestroyScript;
			vtable.invokeAwake = &FakeInvokeAwake;
			vtable.invokeStart = &FakeInvokeStart;
			vtable.invokeUpdate = &FakeInvokeUpdate;
			vtable.invokeDisable = &FakeInvokeDisable;
			vtable.invokeUpdateBatch = &FakeInvokeUpdateBatch;
			vtable.getFieldCount = &FakeGetFieldCount;
			vtable.getFieldDescriptors = &FakeGetFieldDescriptors;
			vtable.getFieldValue = &FakeGetFieldValue;
			vtable.setFieldValue = &FakeSetFieldValue;
			return vtable;
		}

		class ExportScriptingFixture {
		public:
			ExportScriptingFixture() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ObjectManager::instance().clear();
				ScriptingSystem::Instance().Shutdown();
				g_FakeState = {};
			}

			~ExportScriptingFixture() {
				ScriptingSystem::Instance().Shutdown();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}
		};
	}

	void RegisterExportScriptingRuntimeTests(TestRegistry& registry) {
		registry.Add("Engine.ExportScriptingRuntime.CreatesHydratesAndUpdatesStagedScriptBindings", [](TestContext& context) {
			ExportScriptingFixture fixture;
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Scripted Object");
			ScriptComponent& scriptComponent = *objects.AddComponent<ScriptComponent>(object.EntityId());
			ScriptBinding& binding = scriptComponent.AddBinding(TerraEngine::EMPTY_UUID, "Game.PlayerController");
			binding.scriptId = kScriptId;
			ScriptFieldValue speedValue;
			speedValue.fieldName = "Speed";
			speedValue.fieldPath = "Speed";
			speedValue.displayName = "Speed";
			speedValue.valueKind = "Float";
			speedValue.value = 7.25f;
			binding.serializedValues.push_back(speedValue);
			scriptComponent.SynchronizeStorageFromBindings();

			ScriptingSystem::Instance().InitializeExportModeForTesting(MakeFakeVTable());
			ScriptingSystem::Instance().Update(0.125f);

			context.ExpectEqual(1, g_FakeState.scriptCountCalls, "Export initialization should query the generated script registry.");
			context.ExpectEqual(1, g_FakeState.createCalls, "The export runtime should create one managed script instance for the staged binding.");
			context.ExpectEqual(kScriptId, g_FakeState.instance.scriptId, "The runtime should create the script instance by staged script ID.");
			context.ExpectEqual(static_cast<std::uint32_t>(object.EntityId()), g_FakeState.instance.objectId, "The runtime should pass the owning object ID to the managed script.");
			context.ExpectEqual(7.25f, g_FakeState.instance.speed, "Authored serialized field values should hydrate before lifecycle callbacks.");
			context.ExpectEqual(1, g_FakeState.awakeCalls, "Awake should be invoked when the export script instance is created.");
			context.ExpectEqual(1, g_FakeState.startCalls, "Start should be invoked on the first export script update.");
			context.ExpectEqual(1, g_FakeState.updateCalls, "Update should be invoked on the first export script update.");
			context.ExpectEqual(1, g_FakeState.batchUpdateCalls, "Export runtimes that expose batch dispatch should receive batched script updates.");
			context.ExpectEqual(1, g_FakeState.batchItemCount, "The batch dispatch should include the staged script work item.");
			context.Expect(g_FakeState.lastFrameOrder > 0, "Batch dispatch should carry deterministic frame order metadata.");
			context.Expect(g_FakeState.lastObjectGeneration > 0, "Export batch dispatch should carry object cache generation metadata.");
			context.Expect(g_FakeState.lastComponentGeneration > 0, "Export batch dispatch should carry component cache generation metadata.");
			context.Expect(g_FakeState.lastBindingGeneration > 0, "Export batch dispatch should carry binding cache generation metadata.");
			context.ExpectEqual(0.125f, g_FakeState.lastDeltaTime, "Export script Update should receive the frame delta time.");
		});
	}
}
