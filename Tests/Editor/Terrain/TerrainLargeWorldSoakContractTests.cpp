// TerrainLargeWorldSoakContractTests.cpp -- Editor acceptance contract for terrain soaks.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainPicking.h"

#include "Terrain/TerrainTypes.h"
#include "Terrain/TerrainAreaCodec.h"

#include <array>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		struct TerrainSoakCounter
		{
			double routeMetres = 0.0;
			std::uint64_t residentBytes = 0U;
			std::uint64_t residentHighWaterBytes = 0U;
			bool coverageAvailable = true;
			bool requiredAreaAvailable = true;
		};

		bool MeetsSoakContract(const std::vector<TerrainSoakCounter>& counters)
		{
			if(counters.size() < 2U) return false;
			const TerrainSoakCounter& first = counters.front();
			const TerrainSoakCounter& last = counters.back();
			if(last.routeMetres < 1'000'000.0 || first.residentBytes == 0U) return false;
			for(const auto& counter : counters)
				if(!counter.coverageAvailable || !counter.requiredAreaAvailable || counter.residentHighWaterBytes < counter.residentBytes) return false;
			return last.residentBytes <= first.residentBytes + first.residentBytes / 20U;
		}
	}

	void RegisterTerrainLargeWorldSoakContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainLargeWorldSoak.ReturnRouteBoundedGrowthAndCoverage", [](TestContext& context) {
			const std::vector<TerrainSoakCounter> route{
				{0.0, 100U, 120U, true, true},
				{64'000.0, 105U, 124U, true, true},
				{128'000.0, 110U, 128U, true, true},
				{500'000.0, 116U, 128U, true, true},
				{1'000'000.0, 104U, 128U, true, true}};
			context.Expect(MeetsSoakContract(route),
				"A production terrain soak must return after a 1,000-km route with coverage/current work available and no more than 5% resident-byte growth.");
			std::vector<TerrainSoakCounter> unavailable = route;
			unavailable[1].coverageAvailable = false;
			context.Expect(!MeetsSoakContract(unavailable), "Unavailable coverage must fail the soak contract instead of being treated as a successful fallback.");
		});

		registry.Add("Editor.TerrainLargeWorldSoak.CanonicalDistantPickQualifiesFrame", [](TestContext& context) {
			using namespace TerraEngine::Editor::Terrain;
			TerrainDistantPickRequest request;
			request.rayOrigin = {{15, 0, -16}, {10.25, 250.0, 4.75}};
			request.referenceFrameOrigin = {{15, 0, -16}, {0.0, 0.0, 0.0}};
			request.localDirection = {0.0, -1.0, 0.0};
			request.expectedReferenceFrameRevision = 8U;
			request.observedReferenceFrameRevision = 8U;
			request.maximumDistance = 1'000.0;
			request.stepMetres = 0.25;
			const auto hit = TerrainPicking::PickDistantHeightfield(request, [](std::int64_t, std::int64_t) { return std::optional<double>{100.0}; });
			context.Expect(hit.has_value() && hit->canonicalWorldPosition.cell.x == 15 && hit->canonicalWorldPosition.cell.z == -16
				&& hit->canonicalWorldPosition.offsetMetres.y == 100.0,
				"Distant CPU picks must preserve canonical world cells and only localize after the reference frame qualifies.");
			request.observedReferenceFrameRevision = 9U;
			context.Expect(!TerrainPicking::PickDistantHeightfield(request, [](std::int64_t, std::int64_t) { return std::optional<double>{100.0}; }),
				"A stale reference frame must reject a distant terrain pick before local sampling.");
		});

		registry.Add("Editor.TerrainLargeWorldSoak.ArtifactIdentityIsContentAddressed", [](TestContext& context) {
			const std::array<std::uint8_t, 8U> rawCounters{0x01U, 0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U};
			const auto artifactHash = TerraEngine::Terrain::HashTerrainBytes(rawCounters);
			context.Expect(artifactHash.IsValid() && artifactHash == TerraEngine::Terrain::HashTerrainBytes(rawCounters),
				"A soak artifact must expose a stable content identity alongside raw counters and high-water marks.");
		});
	}
}
