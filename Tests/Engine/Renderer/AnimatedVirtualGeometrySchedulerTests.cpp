// AnimatedVirtualGeometrySchedulerTests.cpp - renderer scheduler visibility.

#include "TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Renderer/RenderSystem.h"

#include <algorithm>
#include <string>

namespace TerraEngine::Tests {

	namespace {

		bool Contains(const std::vector<std::string>& values, const char* expected)
		{
			return std::find(values.begin(), values.end(), expected) != values.end();
		}

	}

	void RegisterAnimatedVirtualGeometrySchedulerTests(TestRegistry& registry)
	{
		registry.Add("AnimatedVirtualGeometry.Scheduler.DeclaresPlanAndPublicationBoundaries", [](TestContext& context)
		{
			const auto declaration = RenderSystem::Instance().SchedulerDeclaration();
			context.Expect(Contains(declaration.writes,
				TerraEngine::Scheduling::kResourceAnimatedGeometryDeformationPlans),
				"RenderSystem must publish the immutable deformation plan.");
			context.Expect(Contains(declaration.writes,
				TerraEngine::Scheduling::kResourceAnimatedGeometryDynamicBounds),
				"RenderSystem must declare dynamic-bound output.");
			context.Expect(Contains(declaration.writes,
				TerraEngine::Scheduling::kResourceAnimatedGeometryPublications),
				"RenderSystem must declare the atomic publication row.");
			context.Expect(declaration.safeSubwork.find("frame-wide animated page demand union")
				!= std::string::npos,
				"Worker-safe preparation must name frame-wide page deduplication.");
			context.Expect(declaration.protectedCommit.find("atomic geometry/history/bounds publication")
				!= std::string::npos,
				"The protected boundary must name atomic publication.");
		});
	}

}
