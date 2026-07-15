#pragma once

#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests::PerformanceFixtures
{
	// ViewportFixture keeps performance tests independent from a live renderer.
	// The values describe only the stable inputs needed by marker selection tests.
	struct ViewportFixture
	{
		std::uint32_t width = 1920;
		std::uint32_t height = 1080;
		float maximumMarkerDistance = 0.0f;
		float minimumProjectedSizePixels = 1.0f;
	};

	// MarkerFixture describes one deterministic marker without constructing an
	// editor object facade or touching live scene storage from a test worker.
	struct MarkerFixture
	{
		TerraEngine::UUID objectId{};
		float screenX = 0.0f;
		float screenY = 0.0f;
		float depth = 0.0f;
		bool selected = false;
		bool camera = false;
		bool light = false;
		bool physicsBearing = false;
	};

	enum class ReadinessPass : std::uint8_t
	{
		Cold,
		Warm,
		Stale,
		Corrupt
	};

	struct AutomationExpectation
	{
		std::string operation;
		bool expectsDurableJob = false;
		std::vector<std::string> requiredAdditiveFields;
	};

	[[nodiscard]] ViewportFixture MakeCanonicalViewport();
	[[nodiscard]] std::vector<MarkerFixture> MakeDenseMarkers(std::size_t count);
	[[nodiscard]] std::vector<ReadinessPass> MakeReadinessMatrix();
	[[nodiscard]] AutomationExpectation MakeAutomationExpectation(std::string_view operation);
}
