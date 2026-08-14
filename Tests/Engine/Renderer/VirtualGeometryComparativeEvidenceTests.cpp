#include "TestHarness.h"

#include "Tests/Tools/VirtualGeometry/VirtualGeometryComparativeEvidence.h"

namespace TerraEngine::Tests
{
	static void PopulateMatchedComparisonInput(Tools::VirtualGeometry::ComparisonInput& input)
	{
		input.terraIdentity = { "content", "camera", "quality", "hardware", "Vulkan", "revision", "commands" };
		input.referenceIdentity = input.terraIdentity;
		input.terraSamples.resize(6U);
		input.referenceSamples.resize(6U);
		for(std::size_t index = 0u; index < input.terraSamples.size(); ++index)
		{
			const std::string identity = "sample-" + std::to_string(index);
			input.terraSamples[index].sampleIdentity = identity;
			input.referenceSamples[index].sampleIdentity = identity;
			input.terraSamples[index].visualCorrect = true;
			input.referenceSamples[index].visualCorrect = true;
		}
	}

	void RegisterVirtualGeometryComparativeEvidenceTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.ComparativeEvidence.RequiresSixMatchedVisualSamples",
			[](TestContext &context)
			{
				Tools::VirtualGeometry::ComparisonInput input{};
				PopulateMatchedComparisonInput(input);
				const auto accepted = Tools::VirtualGeometry::ValidateComparisonInput(input);
				context.Expect(accepted.valid, "A comparison requires six valid matched samples per side and visual correctness gates.");
				input.referenceSamples.pop_back();
				context.Expect(!Tools::VirtualGeometry::ValidateComparisonInput(input).valid,
					"Fewer than six valid samples on either side must remain insufficient.");
			});

		registry.Add("VirtualGeometry.ComparativeEvidence.PreservesUnavailableAndRejectedSamples",
			[](TestContext &context)
			{
				Tools::VirtualGeometry::ComparisonInput input{};
				PopulateMatchedComparisonInput(input);
				input.referenceSamples[2].availability = Tools::VirtualGeometry::SampleAvailability::Unavailable;
				input.referenceSamples[3].rejectionReason = "quality-mismatch";
				const auto report = Tools::VirtualGeometry::BuildComparisonReport(input);
				context.Expect(report.unavailableCount == 1U && report.rejectedCount == 1U,
					"Unavailable and rejected samples must remain attributable rather than becoming zero-valued successes.");
				context.Expect(report.classification == Tools::VirtualGeometry::ComparisonClassification::Insufficient,
					"Missing or rejected samples must fail closed to insufficient evidence.");
			});

		registry.Add("VirtualGeometry.ComparativeEvidence.RejectsMismatchedCaptureOrSampleIdentity",
			[](TestContext& context)
			{
				Tools::VirtualGeometry::ComparisonInput input{};
				PopulateMatchedComparisonInput(input);
				input.referenceIdentity.commandHash = "different-command-stream";
				context.Expect(!Tools::VirtualGeometry::ValidateComparisonInput(input).valid,
					"A matching workload name is insufficient when command provenance differs.");
				input.referenceIdentity = input.terraIdentity;
				input.referenceSamples[5].sampleIdentity = "different-sample";
				context.Expect(!Tools::VirtualGeometry::ValidateComparisonInput(input).valid,
					"Every accepted timing must belong to its exact candidate/reference pair.");
			});
	}
}
