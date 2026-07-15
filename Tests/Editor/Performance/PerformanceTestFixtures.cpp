#include "PerformanceTestFixtures.h"

#include <algorithm>
#include <cstring>

namespace TerraEngine::EditorTests::PerformanceFixtures
{
	ViewportFixture MakeCanonicalViewport()
	{
		return {};
	}

	std::vector<MarkerFixture> MakeDenseMarkers(std::size_t count)
	{
		std::vector<MarkerFixture> markers;
		markers.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
		{
			MarkerFixture marker;
			const std::uint64_t deterministicId = static_cast<std::uint64_t>(index + 1u);
			std::memcpy(marker.objectId.data(), &deterministicId, sizeof(deterministicId));
			// The intentionally small repeating region creates a deterministic
			// worst-case cluster without depending on ImGui or camera matrices.
			marker.screenX = static_cast<float>(index % 4u);
			marker.screenY = static_cast<float>((index / 4u) % 4u);
			marker.depth = static_cast<float>(index) / static_cast<float>(std::max<std::size_t>(1u, count));
			markers.push_back(marker);
		}
		if (!markers.empty())
		{
			markers.front().selected = true;
		}
		if (markers.size() > 1u)
		{
			markers[1u].camera = true;
		}
		if (markers.size() > 2u)
		{
			markers[2u].light = true;
		}
		return markers;
	}

	std::vector<ReadinessPass> MakeReadinessMatrix()
	{
		return { ReadinessPass::Cold, ReadinessPass::Warm, ReadinessPass::Stale, ReadinessPass::Corrupt };
	}

	AutomationExpectation MakeAutomationExpectation(std::string_view operation)
	{
		AutomationExpectation expectation;
		expectation.operation = std::string(operation);
		if (operation == "profiler.capture.start" || operation == "profiler.capture.stop" || operation == "script.compile")
		{
			expectation.expectsDurableJob = true;
		}
		if (operation == "scene_view.state")
		{
			expectation.requiredAdditiveFields = { "largeScene", "markers", "stageTimings" };
		}
		else if (operation == "profiler.capture.summary")
		{
			expectation.requiredAdditiveFields = { "physicsStages", "sceneViewStages", "protectedBoundaries", "workerTelemetry" };
		}
		return expectation;
	}
}
