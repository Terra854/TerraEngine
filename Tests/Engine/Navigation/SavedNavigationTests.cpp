// SavedNavigationTests.cpp - provider-neutral saved navigation cache contract.
// The saved-navigation implementation must provide the headers named below; this test intentionally
// establishes the durable API before any cache reader/writer is implemented.

#include "TestHarness.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Persistence/SavedNavigation.h"
#include "Scene/Serializer.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace TerraEngine::Tests {
	void RegisterSavedNavigationTests(TestRegistry& registry) {
		registry.Add("Navigation.SavedNavigation.ProfileValuesHaveNoIdentityOrRevision", [](TestContext& context) {
			const Navigation::NavigationProfile profile = Navigation::NavigationProfile::Defaults();
			const nlohmann::json serialized = Serializer::SerializeNavigationProfile(profile);
			context.Expect(serialized.value("schemaVersion", 0U) == 1U
				&& !serialized.contains("profileId") && !serialized.contains("revision"),
				"The hard-break profile schema must persist only traversal values.");
			context.Expect(Serializer::DeserializeNavigationProfile(serialized).has_value(),
				"The identity-free profile schema must round-trip its validated values.");

			nlohmann::json legacy = serialized;
			legacy["schemaVersion"] = 1U;
			legacy["profileId"] = 1U;
			legacy["revision"] = 1U;
			context.Expect(!Serializer::DeserializeNavigationProfile(legacy).has_value(),
				"The removed identity-bearing profile schema must reject instead of receiving compatibility translation.");
		});

		registry.Add("Navigation.SavedNavigation.RejectsStaleCorruptAndIncompatibleCachesAtomically", [](TestContext& context) {
			Navigation::SavedNavigationDescriptor descriptor{};
			descriptor.formatVersion = 1U;
			descriptor.backendAbi = "recast-1.6.0-dtpolyref64";
			descriptor.sourceFingerprint = 17U;
			descriptor.settingsFingerprint = 19U;
			Navigation::SavedNavigationChunk chunk{};
			chunk.key = {2, -3, 1, 77U};
			chunk.generation = 9U;
			chunk.bytes = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
			const std::array chunks{chunk};
			const Navigation::SavedNavigationWriteResult written = Navigation::WriteSavedNavigationAtomically(descriptor, chunks);
			context.Expect(written.accepted, "A checked temporary write must replace its final cache atomically.");
			const Navigation::SavedNavigationReadResult fresh = Navigation::ReadSavedNavigation(written.bytes, descriptor);
			context.Expect(fresh.accepted && fresh.matchesFreshRuntimeGeneration,
				"A compatible header/index/chunk checksum cache must match fresh runtime generation.");
			Navigation::SavedNavigationDescriptor stale = descriptor; stale.settingsFingerprint++;
			context.Expect(!Navigation::ReadSavedNavigation(written.bytes, stale).accepted,
				"Backend ABI, source, settings, version, bounds, and checksum mismatches must reject before publication.");

			std::vector<std::byte> corrupt = written.bytes;
			corrupt.back() ^= std::byte{0xff};
			context.Expect(!Navigation::ReadSavedNavigation(corrupt, descriptor).accepted,
				"A corrupt payload must reject before any decoded chunk becomes visible.");
			std::vector<std::byte> truncated(written.bytes.begin(), written.bytes.end() - 1);
			context.Expect(!Navigation::ReadSavedNavigation(truncated, descriptor).accepted,
				"A truncated chunk range must reject as one complete cache, never as a partial navigation root.");
		});

		registry.Add("Navigation.SavedNavigation.UnknownPrefabComponentsRejectBeforePublication", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object activeObject = objects.createObject("Active Scene Must Survive");
			const std::uint64_t activeGeneration = objects.GetSceneGeneration();
			const std::array<std::string_view, 3U> unsupportedComponents{
				"NavigationAgentComponent", "NavigationSourceComponent", "NavigationConnectionComponent"};

			for(const std::string_view componentName : unsupportedComponents) {
				// Each prefab is decoded into detached value records first. Unknown
				// component names must reject that complete candidate without exposing
				// a valid prefix to the protected publication boundary.
				nlohmann::json prefab = {
					{"format", std::string(TerraEngine::Scene::kTerraPrefabUnboundedFormat)},
					{"sourceId", "hard-break-negative-fixture"},
					{"objects", nlohmann::json::array({{
						{"uuid", "580000000000000000000000000010"},
						{"name", "Unsupported Prefab Object"},
						{"components", {{std::string(componentName), nlohmann::json::object()}}}
					}})}
				};
				const TerraEngine::Scene::PreparedSceneDecode prepared = Serializer::PreparePrefabDecode(prefab);
				context.Expect(!prepared.accepted && prepared.objects.empty()
					&& prepared.objectCount == 0U && prepared.componentCount == 0U,
					"An unknown prefab component must reject the complete detached candidate.");
				context.Expect(prepared.reason.find("unregistered component") != std::string::npos,
					"Prefab rejection must use the ordinary unknown-component diagnostic.");
				context.Expect(objects.isObjectValid(activeObject) && objects.GetSceneGeneration() == activeGeneration,
					"Rejected prefab decoding must not mutate the live scene or its generation.");
			}
			objects.clear();
		});
	}
}
