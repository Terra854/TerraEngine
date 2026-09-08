// NavigationGeometryCompilerTests.cpp - deterministic companion-cook contract.

#include "TestHarness.h"

#include "NavigationGeometryCompiler.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace TerraEngine::EditorTests {
	namespace {
		TerraEngine::Navigation::NavigationGeometryDocument TriangleDocument() {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument document;
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{0.0F, 0.0F, 0.0F}, {4.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 4.0F}};
			document.triangles = {{0U, 1U, 2U}};
			return document;
		}

		TerraEngine::Editor::NavigationGeometryCompileRequest Request() {
			TerraEngine::Editor::NavigationGeometryCompileRequest request;
			request.geometry = TriangleDocument();
			request.sourceFingerprint = 0x1122334455667788ULL;
			request.settingsFingerprint = 0x8877665544332211ULL;
			return request;
		}
	}

	void RegisterNavigationGeometryCompilerTests(TestRegistry& registry) {
		registry.Add("Navigation.Editor.Compiler.RepeatedProductionIsByteIdenticalAndFingerprinted", [](TestContext& context) {
			const auto first = TerraEngine::Editor::CompileNavigationGeometry(Request());
			const auto second = TerraEngine::Editor::CompileNavigationGeometry(Request());
			context.Expect(first.accepted && second.accepted && first.bytes == second.bytes,
				"Identical source and settings values must produce byte-identical .tenavgeom companions.");
			context.Expect(first.sourceFingerprint == 0x1122334455667788ULL
				&& first.settingsFingerprint == 0x8877665544332211ULL && first.checksum != 0U,
				"Cook output must retain both semantic fingerprints and a non-zero payload checksum.");
		});

		registry.Add("Navigation.Editor.Compiler.OutputRoundTripsThroughRuntimeDecoder", [](TestContext& context) {
			const auto cooked = TerraEngine::Editor::CompileNavigationGeometry(Request());
			const auto decoded = TerraEngine::Navigation::DecodeNavigationGeometry(cooked.bytes);
			context.Expect(cooked.accepted && decoded.accepted
				&& decoded.document.vertices.size() == 3U && decoded.document.triangles.size() == 1U,
				"The editor companion producer and installed runtime decoder must share one checked wire contract.");
		});

		registry.Add("Navigation.Editor.Compiler.MalformedOrCancelledInputPublishesNoBytes", [](TestContext& context) {
			auto malformed = Request();
			malformed.geometry.vertices.front().x = std::numeric_limits<float>::quiet_NaN();
			const auto rejected = TerraEngine::Editor::CompileNavigationGeometry(malformed);
			context.Expect(!rejected.accepted && !rejected.cancelled && rejected.bytes.empty(),
				"Malformed source geometry must reject before any companion bytes become publishable.");

			std::atomic_bool cancellationRequested = true;
			auto cancelledRequest = Request();
			cancelledRequest.cancellationRequested = &cancellationRequested;
			const auto cancelled = TerraEngine::Editor::CompileNavigationGeometry(cancelledRequest);
			context.Expect(!cancelled.accepted && cancelled.cancelled && cancelled.bytes.empty(),
				"Cancellation must leave no partial bytes for AssetDatabase atomic replacement.");
		});
	}
}
