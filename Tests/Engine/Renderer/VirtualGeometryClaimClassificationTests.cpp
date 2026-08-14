#include "TestHarness.h"

#include "Tests/Tools/VirtualGeometry/VirtualGeometryComparativeEvidence.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryClaimClassificationTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Claims.VisualFirstAndProtectedMetricClassification",
			[](TestContext &context)
			{
				Tools::VirtualGeometry::ComparisonReport report{};
				report.visualGatePassed = true;
				report.protectedMetricsRegressed = false;
				report.performanceImprovement = 0.25F;
				report.superiorityMargin = 0.10F;
				context.Expect(Tools::VirtualGeometry::ClassifyComparison(report) == Tools::VirtualGeometry::ComparisonClassification::TerraEngineWin,
					"A visually correct result clearing the improvement margin is a workload-specific TerraEngine win.");
				report.protectedMetricsRegressed = true;
				context.Expect(Tools::VirtualGeometry::ClassifyComparison(report) == Tools::VirtualGeometry::ComparisonClassification::Regression,
					"Protected-metric regressions must override performance improvement claims.");
				report.visualGatePassed = false;
				context.Expect(Tools::VirtualGeometry::ClassifyComparison(report) == Tools::VirtualGeometry::ComparisonClassification::Insufficient,
					"Visual-gate failure must prevent parity or superiority claims.");
			});

		registry.Add("VirtualGeometry.Claims.ParityAndIncomparableAreExplicit",
			[](TestContext &context)
			{
				Tools::VirtualGeometry::ComparisonReport report{};
				report.visualGatePassed = true;
				report.performanceImprovement = 0.01F;
				report.superiorityMargin = 0.10F;
				report.parityRegressionLimit = 0.10F;
				context.Expect(Tools::VirtualGeometry::ClassifyComparison(report) == Tools::VirtualGeometry::ComparisonClassification::Parity,
					"Results inside the declared margin must classify as parity.");
				report.referenceSupported = false;
				context.Expect(Tools::VirtualGeometry::ClassifyComparison(report) == Tools::VirtualGeometry::ComparisonClassification::Incomparable,
					"Unsupported reference paths must remain incomparable.");
			});

		registry.Add("VirtualGeometry.Claims.AggregateNeverPromotesOrHidesRegression",
			[](TestContext& context)
			{
				Tools::VirtualGeometry::ComparisonReport win{};
				win.visualGatePassed = true;
				win.performanceImprovement = 0.25F;
				Tools::VirtualGeometry::ComparisonReport parity{};
				parity.visualGatePassed = true;
				parity.performanceImprovement = 0.0F;
				context.Expect(
					Tools::VirtualGeometry::ClassifyComparisonAggregate({ win, parity })
						== Tools::VirtualGeometry::ComparisonClassification::Parity,
					"Workload-specific wins must not be promoted into an aggregate superiority claim.");
				parity.protectedMetricsRegressed = true;
				context.Expect(
					Tools::VirtualGeometry::ClassifyComparisonAggregate({ win, parity })
						== Tools::VirtualGeometry::ComparisonClassification::Regression,
					"One protected regression must remain visible in the aggregate.");
			});
	}
}
