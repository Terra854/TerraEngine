// Test-first contracts for explicit, gameplay-controlled ground connections.
//
// The store is deliberately provider-neutral. It must validate support and
// clearance at ingestion, capture gameplay conditions as values, and publish
// immutable revisions that route queries can consume without callbacks.
#include "../TestHarness.h"

#include "Components/Navigation/NavigationConnectionComponent.h"
#include "Navigation/Ground/GroundTraversalConnectionStore.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationTypes.h"

#include <cstdint>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;
	using namespace TerraEngine::Navigation::Ground;

	namespace
	{
		NavigationOwnerHandle TestOwner()
		{
			return {{NavigationWorldId{9U}, 2U}, NavigationOwnerId{77U}, 4U};
		}

		NavigationConnectionComponent AuthoredConnection()
		{
			NavigationConnectionComponent connection{};
			connection.firstEndpoint.offsetMetres = {0.0, 0.0, 0.0};
			connection.secondEndpoint.offsetMetres = {2.0, 0.0, 0.0};
			connection.gameplayCondition = true;
			connection.traversalCost = 2.0F;
			return connection;
		}
	}

	void RegisterNavigationConnectionTests(TestRegistry& registry)
	{
		registry.Add("Navigation.Connections.IngestEnableDirectionCapabilityAndCapturedCondition", [](TestContext& context) {
			GroundTraversalConnectionStore store;
			NavigationConnectionComponent authored = AuthoredConnection();
			authored.direction = NavigationConnectionDirection::ForwardOnly;
			authored.capabilityMask = 0x04U;
			const auto accepted = store.Upsert(TestOwner(), authored, GroundTraversalConnectionStore::SupportCertificate::FlatGround(2.0F));
			context.Expect(accepted.accepted, "A supported enabled connection must be ingested into the immutable store.");

			const auto snapshot = store.AcquireSnapshot();
			context.Expect(snapshot.Contains(authored.connectionId), "An accepted connection must be visible in the next immutable snapshot.");
			context.Expect(snapshot.CanTraverse(authored.connectionId, NavigationConnectionDirection::ForwardOnly, 0x04U),
				"Direction and capability filtering must admit a matching ground traversal.");
			context.Expect(!snapshot.CanTraverse(authored.connectionId, NavigationConnectionDirection::ReverseOnly, 0x04U),
				"A forward-only connection must reject reverse traversal.");
			context.Expect(!snapshot.CanTraverse(authored.connectionId, NavigationConnectionDirection::ForwardOnly, 0x08U),
				"A capability mismatch must reject traversal without invoking gameplay code.");

			authored.gameplayCondition = false;
			const auto conditionUpdate = store.Upsert(TestOwner(), authored, GroundTraversalConnectionStore::SupportCertificate::FlatGround(2.0F));
			context.Expect(conditionUpdate.accepted && !store.AcquireSnapshot().CanTraverse(authored.connectionId,
				NavigationConnectionDirection::ForwardOnly, 0x04U),
				"Gameplay conditions must be captured values and must not be worker callbacks.");
		});

		registry.Add("Navigation.Connections.RejectUnsupportedEndpointsAndClearance", [](TestContext& context) {
			GroundTraversalConnectionStore store;
			NavigationConnectionComponent unsupported = AuthoredConnection();
			const auto unsupportedResult = store.Upsert(TestOwner(), unsupported,
				GroundTraversalConnectionStore::SupportCertificate::UnsupportedGap());
			context.Expect(!unsupportedResult.accepted,
				"An explicit connection must not create an unsupported jump or gap in ground navigation.");

			const auto clearanceResult = store.Upsert(TestOwner(), AuthoredConnection(),
				GroundTraversalConnectionStore::SupportCertificate::InsufficientClearance());
			context.Expect(!clearanceResult.accepted,
				"An endpoint or corridor that fails profile clearance must be rejected before publication.");

			NavigationConnectionComponent malformed = AuthoredConnection();
			malformed.traversalCost = -1.0F;
			context.Expect(!store.Upsert(TestOwner(), malformed,
				GroundTraversalConnectionStore::SupportCertificate::FlatGround(2.0F)).accepted,
				"Negative connection costs must reject atomically.");
		});

		registry.Add("Navigation.Connections.LifecycleInvalidationAndRouteUseAreGenerationQualified", [](TestContext& context) {
			GroundTraversalConnectionStore store;
			NavigationConnectionComponent authored = AuthoredConnection();
			const auto accepted = store.Upsert(TestOwner(), authored,
				GroundTraversalConnectionStore::SupportCertificate::FlatGround(2.0F));
			context.Expect(accepted.accepted, "The lifecycle fixture requires one valid published connection.");
			const auto before = store.AcquireSnapshot();

			const auto route = before.FindRouteUsing(authored.connectionId, NavigationConnectionDirection::Bidirectional,
				0xFFFFFFFFU, NavigationProfile::Defaults());
			context.Expect(route.outcome == NavigationOutcome::Complete,
				"A valid explicit connection must be usable by ground route search.");

			context.Expect(store.Disable(authored.connectionId), "Disabling an authored connection must publish a tombstone revision.");
			const auto afterDisable = store.AcquireSnapshot();
			context.Expect(!afterDisable.CanTraverse(authored.connectionId, NavigationConnectionDirection::Bidirectional, 0xFFFFFFFFU),
				"Disabled connections must stop being traversable at the observation boundary.");
			context.Expect(!route.ValidateAgainst(afterDisable).usable,
				"Routes retaining a disabled connection must become invalid before gameplay consumes movement intent.");

			context.Expect(store.Remove(authored.connectionId), "Removing a connection must be idempotently recorded as a lifecycle change.");
			context.Expect(!store.AcquireSnapshot().Contains(authored.connectionId),
				"Removed connections must not survive into a later immutable generation.");
		});
	}
}
