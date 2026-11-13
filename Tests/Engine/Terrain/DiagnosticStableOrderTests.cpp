#include "TestHarness.h"
#include "Terrain/TerrainDiagnostics.h"

#include <array>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		std::vector<std::string> CategoryMessages(const Terrain::TerrainDiagnosticSnapshot& snapshot, Terrain::TerrainDiagnosticCategory category) {
			std::vector<std::string> messages;
			for (const auto& item : snapshot.items) if (item.category == category) messages.push_back(item.message);
			return messages;
		}
	}
	void RegisterDiagnosticStableOrderTests(TestRegistry& registry) {
		registry.Add("Terrain.Diagnostics.EqualCategoriesPreserveProducerOrder", [](TestContext& context) {
			using namespace Terrain;
			TerrainDiagnosticBuildInput input;
			const std::array categories{TerrainDiagnosticCategory::Source, TerrainDiagnosticCategory::Job, TerrainDiagnosticCategory::Conflict};
			std::array<std::vector<std::string>, 3> expected;
			// More than a tiny sort partition exposes unstable tie rearrangement.
			// Nonlexicographic messages make accidental alphabetical sorting visible.
			for (unsigned index = 0; index < 96U; ++index) {
				const auto bucket = index % categories.size();
				TerrainDiagnosticItem item;
				item.category = categories[bucket];
				item.message = "producer-" + std::to_string((index * 37U) % 101U);
				expected[bucket].push_back(item.message);
				input.snapshot.items.push_back(std::move(item));
			}
			for (unsigned repeat = 0; repeat < 3U; ++repeat) {
				const auto snapshot = BuildTerrainDiagnosticSnapshot(input);
				context.ExpectEqual(std::size_t{96}, snapshot.items.size(), "Sorting must preserve every producer fact.");
				for (std::size_t category = 0; category < categories.size(); ++category)
					context.Expect(CategoryMessages(snapshot, categories[category]) == expected[category], "Each equal-category subsequence must preserve its documented producer insertion order.");
				for (std::size_t index = 1; index < snapshot.items.size(); ++index)
					context.Expect(snapshot.items[index - 1].category <= snapshot.items[index].category, "Categories must remain ordered while ties preserve producer order.");
			}
		});
		registry.Add("Terrain.Diagnostics.AppendedConflictFactsFollowExistingFacts", [](TestContext& context) {
			using namespace Terrain;
			TerrainDiagnosticBuildInput input;
			TerrainDiagnosticItem existing;
			existing.category = TerrainDiagnosticCategory::Conflict;
			existing.message = "preexisting";
			input.snapshot.items.push_back(existing);
			std::vector<std::string> expected{existing.message};
			for (unsigned index = 0; index < 32U; ++index) {
				TerrainConflictRecord conflict;
				conflict.code = TerrainResultCode::Conflict;
				conflict.message = "conflict-" + std::to_string(31U - index);
				expected.push_back(conflict.message);
				input.snapshot.conflicts.push_back(std::move(conflict));
			}
			const auto snapshot = BuildTerrainDiagnosticSnapshot(input);
			context.Expect(CategoryMessages(snapshot, TerrainDiagnosticCategory::Conflict) == expected, "Builder-generated facts must follow existing facts without tie reordering.");
			for (std::size_t index = 1; index < snapshot.items.size(); ++index)
				context.Expect(snapshot.items[index].impact == TerrainDiagnosticImpact::MutationRejected && snapshot.items[index].action == TerrainDiagnosticAction::ResolveConflict, "Stable ordering must retain the actual diagnostic payload.");
		});
	}
}
