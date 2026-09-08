// SavedNavigationTests.cpp - provider-neutral saved navigation cache contract.
// The saved-navigation implementation must provide the headers named below; this test intentionally
// establishes the durable API before any cache reader/writer is implemented.

#include "TestHarness.h"
#include "Navigation/Persistence/SavedNavigation.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	void RegisterSavedNavigationTests(TestRegistry& registry) {
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
	}
}
