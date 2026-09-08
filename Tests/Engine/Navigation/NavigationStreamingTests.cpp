// NavigationStreamingTests.cpp - mounted-content and procedural-source contracts.

#include "../TestHarness.h"

#include "Navigation/Geometry/ProceduralNavigationSource.h"
#include "Navigation/Streaming/NavigationContentProvider.h"

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;

	void RegisterNavigationStreamingTests(TestRegistry& registry)
	{
		registry.Add("Navigation.Streaming.ProceduralPublicationRetainsImmutableSourceAndSupportsReload", [](TestContext& context) {
			ProceduralNavigationSource source;
			ProceduralNavigationSourceInput first{};
			first.descriptor.sourceRevision = 1U;
			first.geometry.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			first.geometry.primitives.push_back({NavigationGeometryPrimitiveKind::Box, {0.0F, 0.0F, 0.0F}, {2.0F, 0.1F, 2.0F}});
			const auto published = source.Publish(first);
			context.Expect(published.accepted && source.Acquire() == published.snapshot,
				"Procedural sources must publish one immutable snapshot through the shared source path.");
			ProceduralNavigationSourceInput replacement = first;
			replacement.descriptor.sourceRevision = 2U;
			replacement.geometry.primitives.front().halfExtents.x = 4.0F;
			const auto reloaded = source.Publish(replacement);
			context.Expect(reloaded.accepted && reloaded.snapshot != published.snapshot
				&& published.snapshot->Descriptor().sourceRevision == 1U,
				"Reload must replace the live snapshot without mutating retained old geometry.");
		});

		registry.Add("Navigation.Streaming.ProceduralPublicationRejectsMalformedAndBoundsRetention", [](TestContext& context) {
			ProceduralNavigationSource source;
			ProceduralNavigationSourceInput malformed{};
			malformed.descriptor.sourceRevision = 3U;
			malformed.geometry.kind = NavigationGeometryPayloadKind::TriangleMesh;
			malformed.geometry.triangles.push_back({0U, 1U, 2U});
			const auto rejected = source.Publish(malformed);
			context.Expect(!rejected.accepted && !source.Acquire(),
				"Malformed procedural geometry must publish no partial source snapshot.");
			context.Expect(NavigationContentProvider::kMaximumGeometryBytes <= 128ULL * 1024ULL * 1024ULL,
				"Mounted geometry reads must keep an explicit bounded allocation limit.");
		});

		registry.Add("Navigation.Streaming.ContentProviderFailsClosedWithoutMountedCompanion", [](TestContext& context) {
			ContentMount unmounted;
			NavigationGeometrySnapshotDescriptor descriptor{};
			const auto geometry = NavigationContentProvider::LoadGeometry(unmounted, "Navigation/Geometry/mesh.tenavgeom", descriptor);
			const auto cache = NavigationContentProvider::LoadCacheChunk(unmounted, "Navigation/Cache/chunk.tenav");
			context.Expect(!geometry.Succeeded() && !cache.Succeeded(),
				"Missing mounted companions and cache chunks must fail closed without loose-file fallback.");
		});
	}
}
