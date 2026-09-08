// AnimationManagedLifecycleTests.cpp - Managed animation event lifetime and batch-contract checks.

#include "../TestHarness.h"

#include "Scripting/AnimationScriptAbi.h"
#include "Scripting/ExportVTable.h"
#include "Scripting/ScriptGlue.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace TerraEngine::Tests {
	namespace {
		std::string ReadAnimationLifecycleSource(const char* relativePath)
		{
			// The source contract supplements managed compilation by freezing the
			// reload and callback-containment behavior that C++ owns at the boundary.
			std::ifstream source{std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary};
			return {std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
		}

		std::string SliceSource(
			const std::string& source,
			std::string_view beginMarker,
			std::string_view endMarker)
		{
			// Isolate one generated method so a legacy export cannot accidentally
			// pass merely because a later capacity-aware method contains safe code.
			const std::size_t begin = source.find(beginMarker);
			if (begin == std::string::npos) {
				return {};
			}
			const std::size_t end = source.find(endMarker, begin + beginMarker.size());
			if (end == std::string::npos) {
				return {};
			}
			return source.substr(begin, end - begin);
		}
	}

	void RegisterAnimationManagedLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Animation.ManagedLifecycle.EventAndBufferPacketsStayBounded", [](TestContext& context) {
			context.ExpectEqual(static_cast<std::size_t>(464U), sizeof(NativeAnimationEvent),
				"Managed event delivery must copy one fixed record with bounded tag and payload storage.");
			context.ExpectEqual(static_cast<std::size_t>(48U), sizeof(NativeAnimationBufferInfo),
				"Every two-call read must report required, written, overflow, tick, and generation metadata.");
			context.ExpectEqual(static_cast<std::size_t>(144U), offsetof(NativeAnimationEvent, tag),
				"Event filtering metadata must precede the fixed UTF-8 tag at its pinned v1 offset.");
			context.ExpectEqual(static_cast<std::size_t>(208U), offsetof(NativeAnimationEvent, payload),
				"The fixed UTF-8 payload must follow the complete deterministic event identity.");
			context.ExpectEqual(static_cast<std::uint32_t>(64U), kNativeAnimationEventTagBytes,
				"Tags must remain bounded before crossing into managed memory.");
			context.ExpectEqual(static_cast<std::uint32_t>(256U), kNativeAnimationEventPayloadBytes,
				"Payloads must remain bounded before crossing into managed memory.");
		});

		registry.Add("Animation.ManagedLifecycle.NativeAotTailsRemainAppendOnly", [](TestContext& context) {
			context.Expect(kManagedVTableAnimationVersion == 3U
				&& kManagedVTableV2Size == 136U
				&& offsetof(ManagedVTable, getAnimationAbiVersion) == kManagedVTableV2Size
				&& offsetof(ManagedVTable, dispatchAnimationEventBatch) == 144U
				&& kManagedVTableV3Size == 152U
				&& kManagedVTableNavigationVersion == 4U
				&& offsetof(ManagedVTable, getNavigationAbiVersion) == kManagedVTableV3Size
				&& kManagedVTableV4Size == 160U
				&& sizeof(ManagedVTable) == kManagedVTableV4Size,
				"NativeAOT animation v3 fields must stay fixed while navigation appends its separate v4 tail.");

			const std::string mirror = ReadAnimationLifecycleSource("Engine/Managed/Source/Export/ExportVTable.cs");
			const std::string generator = ReadAnimationLifecycleSource(
				"Editor/Managed/TerraEngine.Export.Generator/ScriptRegistryGenerator.cs");
			const std::string host = ReadAnimationLifecycleSource("Engine/DotNet/DotNetHost.cpp");
			const std::string scripting = ReadAnimationLifecycleSource("Engine/Scripting/ScriptingSystem.cpp");
			context.Expect(mirror.find("Size = 160") != std::string::npos
				&& mirror.find("public const ulong V3Size = 152") != std::string::npos
				&& mirror.find("public const ulong V4Size = 160") != std::string::npos
				&& mirror.find("FieldOffset(136)") != std::string::npos
				&& mirror.find("FieldOffset(144)") != std::string::npos
				&& mirror.find("DispatchAnimationEventBatch") != std::string::npos
				&& mirror.find("FieldOffset(152)") != std::string::npos
				&& mirror.find("GetNavigationAbiVersion") != std::string::npos,
				"The managed NativeAOT table must retain exact v3 offsets and append the navigation probe at v4.");
			context.Expect(generator.find("outTable->Version = 3") != std::string::npos
				&& generator.find("outTable->GetAnimationAbiVersion = &GetAnimationAbiVersion") != std::string::npos
				&& generator.find("outTable->DispatchAnimationEventBatch = &DispatchAnimationEventBatch") != std::string::npos
				&& generator.find("outTable->GetNavigationAbiVersion = &GetNavigationAbiVersion") != std::string::npos
				&& generator.find("AnimationEvents.DispatchProtectedNativeCore") != std::string::npos,
				"Generated NativeAOT bootstraps must retain the animation callback and append the navigation ABI probe.");
			context.Expect(host.find("outTable->version >= kManagedVTableAnimationVersion") != std::string::npos
				&& host.find("outTable->tableSize < kManagedVTableV3Size") != std::string::npos
				&& host.find("getAnimationAbiVersion() != kManagedAnimationAbiVersion") != std::string::npos
				&& host.find("outTable->version >= kManagedVTableNavigationVersion") != std::string::npos
				&& host.find("getNavigationAbiVersion() != kManagedNavigationAbiVersion") != std::string::npos
				&& scripting.find("m_ExportVTable.version >= kManagedVTableAnimationVersion") != std::string::npos,
				"NativeAOT hosts must reject malformed advertised tails while preserving v1 through v3 compatibility.");
		});

		registry.Add("Animation.ManagedLifecycle.NativeAotInitializersPreserveOlderHostBuffers", [](TestContext& context) {
			const std::string generator = ReadAnimationLifecycleSource(
				"Editor/Managed/TerraEngine.Export.Generator/ScriptRegistryGenerator.cs");
			const std::string host = ReadAnimationLifecycleSource("Engine/DotNet/DotNetHost.cpp");
			const std::string legacyInitializer = SliceSource(
				generator, "public static int InitManagedRuntime(", "InitManagedRuntimeV4");
			const std::string v4Initializer = SliceSource(
				generator, "public static int InitManagedRuntimeV4(", "InitManagedRuntimeV3");
			const std::string v3Initializer = SliceSource(
				generator, "public static int InitManagedRuntimeV3(", "private static void PopulateV1");
			const std::string v1Population = SliceSource(
				generator, "private static void PopulateV1(", "private static void PopulateV3");
			const std::string v3Population = SliceSource(
				generator, "private static void PopulateV3(", "private static void PopulateV4");
			const std::string v4Population = SliceSource(
				generator, "private static void PopulateV4(", "private static void LogRegisteredScripts");

			context.Expect(!legacyInitializer.empty()
				&& legacyInitializer.find("PopulateV1(outTable)") != std::string::npos
				&& legacyInitializer.find("PopulateV3(outTable)") == std::string::npos,
				"The capacity-less legacy export must select only the v1 writer.");
			context.Expect(!v1Population.empty()
				&& v1Population.find("outTable->Version = 1") != std::string::npos
				&& v1Population.find("outTable->TableSize") == std::string::npos
				&& v1Population.find("outTable->GetPhysicsAbiVersion") == std::string::npos
				&& v1Population.find("outTable->GetAnimationAbiVersion") == std::string::npos,
				"A current gameplay library must not overwrite a v1 or v2 host buffer through its legacy export.");
			context.Expect(!v3Initializer.empty()
				&& v3Initializer.find("hostCapacity < global::TerraEngine.Export.ManagedVTable.V3Size") != std::string::npos
				&& v3Initializer.find("return -2") != std::string::npos
				&& v3Initializer.find("PopulateV3(outTable)") != std::string::npos
				&& v3Initializer.find("V4Size") == std::string::npos,
				"The v3 export must retain its 152-byte capacity gate without requiring the later v4 tail.");
			context.Expect(!v3Population.empty()
				&& v3Population.find("outTable->Version = 3") != std::string::npos
				&& v3Population.find("outTable->TableSize = global::TerraEngine.Export.ManagedVTable.V3Size") != std::string::npos
				&& v3Population.find("outTable->GetPhysicsAbiVersion") != std::string::npos
				&& v3Population.find("outTable->GetAnimationAbiVersion") != std::string::npos
				&& v3Population.find("outTable->GetNavigationAbiVersion") == std::string::npos,
				"A capacity-approved v3 export must retain both physics and animation tails.");
			context.Expect(!v4Initializer.empty()
				&& v4Initializer.find("ManagedVTable.V4Size") != std::string::npos
				&& v4Initializer.find("hostCapacity < global::TerraEngine.Export.ManagedVTable.V4Size") != std::string::npos
				&& v4Initializer.find("ValidateNavigationAbiLayout") != std::string::npos
				&& v4Initializer.find("PopulateV4(outTable)") != std::string::npos,
				"The v4 export must validate managed navigation layout before publishing its new tail.");
			context.Expect(!v4Population.empty()
				&& v4Population.find("outTable->Version = 4") != std::string::npos
				&& v4Population.find("outTable->TableSize = global::TerraEngine.Export.ManagedVTable.V4Size") != std::string::npos
				&& v4Population.find("outTable->GetNavigationAbiVersion") != std::string::npos,
				"The v4 population must append only the navigation ABI probe after the complete v3 table.");
			context.Expect(host.find("GetNativeExport(nativeAotLibraryHandle, \"InitManagedRuntimeV4\")") != std::string::npos
				&& host.find("GetNativeExport(nativeAotLibraryHandle, \"InitManagedRuntimeV3\")") != std::string::npos
				&& host.find("GetNativeExport(nativeAotLibraryHandle, \"InitManagedRuntime\")") != std::string::npos
				&& host.find("initV4Fn(outTable, sizeof(ManagedVTable))") != std::string::npos
				&& host.find("initV3Fn(outTable, kManagedVTableV3Size)") != std::string::npos
				&& host.find(": initFn(outTable)") != std::string::npos
				&& host.find("outTable->version >= kManagedVTablePhysicsVersion") != std::string::npos
				&& host.find("outTable->version >= kManagedVTableAnimationVersion") != std::string::npos
				&& host.find("outTable->version >= kManagedVTableNavigationVersion") != std::string::npos,
				"Current hosts must prefer v4, fall back to v3 then legacy v1, and guard every optional tail.");
		});

		registry.Add("Animation.ManagedLifecycle.SubscriptionsFilterDisposeAndInvalidateOnReload", [](TestContext& context) {
			const std::string events = ReadAnimationLifecycleSource("Engine/Managed/Source/Animation/AnimationEvents.cs");
			const std::string lifecycle = ReadAnimationLifecycleSource("Engine/Managed/Source/InternalCalls.cs");
			context.Expect(events.find("sealed class AnimationEventSubscription : IDisposable") != std::string::npos
				&& events.find("MaximumSubscriptions") != std::string::npos,
				"Subscription tokens must be explicitly disposable and bounded.");
			context.Expect(events.find("Animator") != std::string::npos
				&& events.find("Controller") != std::string::npos
				&& events.find("StateId") != std::string::npos
				&& events.find("LayerIndex") != std::string::npos
				&& events.find("EventId") != std::string::npos
				&& events.find("Tag") != std::string::npos
				&& events.find("PayloadCategories") != std::string::npos,
				"Managed filters must cover every animator/controller/state/layer/event/tag/payload selector from FunctionalContract.");
			context.Expect(events.find("ResetManagedSubscriptions") != std::string::npos
				&& events.find("InvalidateForReload") != std::string::npos
				&& events.find("layerIndex: AnyLayer") != std::string::npos
				&& events.find("payloadCategories: AnimationEventPayloadCategory.All") != std::string::npos,
				"Domain reload and Play Mode stop must make every old token permanently inert.");
			context.Expect(lifecycle.find("Animation.AnimationEvents.ResetManagedSubscriptions") != std::string::npos,
				"All gameplay assembly reset paths must release delegates before unloading their collectible context.");
		});

		registry.Add("Animation.ManagedLifecycle.BatchCopyOrdersAndContainsCallbacks", [](TestContext& context) {
			const std::string events = ReadAnimationLifecycleSource("Engine/Managed/Source/Animation/AnimationEvents.cs");
			const std::string scripting = ReadAnimationLifecycleSource("Engine/Scripting/ScriptingSystem.cpp");
			context.Expect(events.find("MaximumManagedBatchEvents = 4096") != std::string::npos
				&& events.find("rejected atomically") != std::string::npos,
				"Managed delivery must reject malformed or oversized copied batches before publishing any event.");
			context.Expect(events.find("Array.Sort") != std::string::npos
				&& events.find("ProducerOrder") != std::string::npos,
				"Managed delivery must establish deterministic packet order even if a provider batch arrives unsorted.");
			context.Expect(events.find("GetInvocationList") != std::string::npos
				&& events.find("catch (Exception exception)") != std::string::npos
				&& events.find("CorrelationId") != std::string::npos,
				"One throwing callback must be correlated and isolated without aborting later subscribers.");
			context.Expect(scripting.find("Native_Animation_CopyEventBatch") != std::string::npos
				&& scripting.find("DispatchProtectedNative") != std::string::npos,
				"ScriptingSystem must copy and deliver animation packets only through the protected managed boundary.");
			context.Expect(scripting.find("Mark the copied publication before invoking managed code") != std::string::npos,
				"Re-entrant editor or callback pumps must not deliver the same immutable publication twice.");
		});

		registry.Add("Animation.ManagedLifecycle.BufferOverflowRemainsExplicit", [](TestContext& context) {
			NativeAnimationBufferInfo info{};
			info.header = {sizeof(info), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::BufferInfo)};
			const NativeAnimationStatus status = Native_Animation_CopyEventBatch(nullptr, 0U, &info);
			context.Expect(status == NativeAnimationStatus::Ok || status == NativeAnimationStatus::BufferTooSmall
				|| status == NativeAnimationStatus::NotReady,
				"A zero-capacity event probe must return structured availability instead of treating null storage as invalid.");
			context.Expect(info.header.size == sizeof(info) && info.header.version == kNativeAnimationAbiVersion,
				"The provider must preserve compatible buffer metadata on every probe outcome.");
			context.Expect(info.writtenCount == 0U,
				"A count probe must never partially write a semantic event result.");
		});
	}
}
