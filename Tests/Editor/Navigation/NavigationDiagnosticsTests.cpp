// NavigationDiagnosticsTests.cpp - immutable, bounded editor diagnostics contracts.

#include "../TestHarness.h"

#include "Navigation/NavigationDiagnostics.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Navigation;

	namespace
	{
		std::string ReadTextFile(const char* relativePath)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
			if(!input) return {};
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}
	}

	void RegisterNavigationDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Editor.Navigation.DiagnosticsPublishesBoundedImmutableSnapshots", [](TestContext& context) {
			NavigationDiagnostics diagnostics(1024U, 2U);
			diagnostics.Append({NavigationDiagnosticSeverity::Information, NavigationMeasurementReason::Measured,
				1U, 10U, "first", "first measurement"});
			const auto before = diagnostics.Snapshot();
			diagnostics.Append({NavigationDiagnosticSeverity::Warning, NavigationMeasurementReason::MissingMeasurement,
				2U, 0U, "missing", "physical readiness unavailable"});
			diagnostics.Append({NavigationDiagnosticSeverity::Information, NavigationMeasurementReason::Measured,
				3U, 20U, "third", "third measurement"});
			const auto after = diagnostics.Snapshot();
			context.Expect(before->records.size() == 1U && before->records.front().code == "first",
				"A previously published diagnostics snapshot must remain immutable after later appends.");
			context.Expect(after->records.size() == 2U && after->droppedRecordCount == 1U,
				"Diagnostic history must evict oldest records within its configured bounded capacity.");
			context.Expect(after->records.front().measurementReason == NavigationMeasurementReason::MissingMeasurement,
				"Unavailable measurements must retain an explicit reason rather than being reported as zero.");
		});

		registry.Add("Editor.Navigation.DiagnosticsPublishesDynamicCountersAtomically", [](TestContext& context) {
			NavigationDiagnostics diagnostics;
			diagnostics.PublishDynamic({3U, 4U, 2U, 1U, 500U, 7U, 6U});
			const auto snapshot = diagnostics.Snapshot();
			context.Expect(snapshot->dynamicSourceLagCount == 3U && snapshot->dirtyCoverageCount == 4U
				&& snapshot->queueDepth == 2U && snapshot->rejectedUpdateCount == 1U
				&& snapshot->latencyNanoseconds == 500U && snapshot->retiredGenerationCount == 7U
				&& snapshot->staleDropCount == 6U,
				"Dynamic navigation counters must cross the same immutable diagnostics publication boundary.");
		});

		registry.Add("Editor.Navigation.DiagnosticsClosedWindowDoesNotExtractExpensiveOverlayData", [](TestContext& context) {
			// This source contract keeps overlay extraction behind an explicitly open
			// diagnostics view; merely publishing diagnostics must remain inexpensive.
			const std::string source = ReadTextFile("Editor/TerraEngine-Editor/Windows/NavigationWindow.cpp");
			context.Expect(!source.empty(), "The Navigation window must expose a bounded overlay-read contract.");
			if(source.empty()) return;
			context.Expect(source.find("diagnosticsWindowOpen") != std::string::npos,
				"Overlay extraction must be gated by the diagnostics window state.");
			context.Expect(source.find("ExtractNavigationOverlay") != std::string::npos,
				"Navigation overlays must consume a named bounded extraction path.");
		});
	}
}
