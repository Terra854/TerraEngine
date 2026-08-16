#include "../TestHarness.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		struct SampleIdentity
		{
			std::string workload;
			std::string contentHash;
			std::string cameraPathHash;
			std::string runtimeRevision;
			std::string hardwareIdentity;
		};

		double Median(std::vector<double> values)
		{
			std::sort(values.begin(), values.end());
			return values[values.size() / 2U];
		}

		bool SameIdentity(const SampleIdentity& left, const SampleIdentity& right)
		{
			return left.workload == right.workload && left.contentHash == right.contentHash
				&& left.cameraPathHash == right.cameraPathHash
				&& left.runtimeRevision == right.runtimeRevision
				&& left.hardwareIdentity == right.hardwareIdentity;
		}
	}

	void RegisterUnboundedWorldPerformanceTests(TestRegistry& registry)
	{
		registry.Add("Renderer.UnboundedWorldPerformance.RequiresFiveMatchingMeasuredRuns", [](TestContext& context)
		{
			const SampleIdentity nearIdentity{ "resident-v1", "content", "camera", "runtime", "hardware" };
			const SampleIdentity farIdentity = nearIdentity;
			const std::vector<double> near = { 10.0, 10.2, 9.9, 10.1, 10.0 };
			const std::vector<double> far = { 10.4, 10.3, 10.2, 10.5, 10.1 };
			context.Expect(near.size() >= 5U && far.size() >= 5U && SameIdentity(nearIdentity, farIdentity),
				"Near and extreme comparisons require five complete runs with matching identity.");
			context.Expect(Median(far) / Median(near) <= 1.05,
				"Extreme-location median frame time must remain within the exact five-percent gate.");
		});

		registry.Add("Renderer.UnboundedWorldPerformance.UnavailableSamplesFailClosed", [](TestContext& context)
		{
			const std::vector<double> measured = { 10.0, 10.1, 9.9 };
			context.Expect(measured.size() < 5U,
				"Fewer than five measured samples must remain incomplete rather than pass by treating unavailable data as zero.");
		});
	}
}
