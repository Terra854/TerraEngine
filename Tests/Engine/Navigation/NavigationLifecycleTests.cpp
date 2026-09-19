// NavigationLifecycleTests.cpp - engine owner/late-completion lifecycle contracts.

#include "../TestHarness.h"

#include "Navigation/NavigationSystem.h"
#include "Scene/SceneMutationQueue.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <span>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadSource(const char* relativePath)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}
	}

	void RegisterNavigationLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Engine.NavigationLifecycle.RevokeAllOwnersInvalidatesLateWork", [](TestContext& context) {
			Navigation::NavigationSystem& system = Navigation::NavigationSystem::Instance();
			system.RevokeAllOwners(Navigation::NavigationReason::SceneReplaced);
			const auto invalidations = system.ReadInvalidations({}, 0U);
			context.Expect(invalidations.status == Navigation::NavigationHandleStatus::InvalidOwner,
				"Late completion readers must reject an owner-less handle after lifecycle revocation.");
		});

		registry.Add("Engine.NavigationLifecycle.ShutdownAndRevokePathsContainPendingCompletions", [](TestContext& context) {
			const std::string source = ReadSource("Engine/Navigation/NavigationSystem.cpp");
			context.Expect(source.find("RevokeAllOwners") != std::string::npos
				&& source.find("pendingRoutes_") != std::string::npos
				&& source.find("preparedRoutes_") != std::string::npos,
				"Navigation lifecycle code must account for both queued requests and late prepared completions.");
			context.Expect(source.find("pendingRoutes_.clear()") != std::string::npos
				&& source.find("preparedRoutes_.clear()") != std::string::npos,
				"Revocation/shutdown must clear pending and prepared route work before the next world can publish.");
		});

		registry.Add("Engine.NavigationLifecycle.DirectRequestsRemainIndependentOfAgentState", [](TestContext& context) {
			const std::string system = ReadSource("Engine/Navigation/NavigationSystem.cpp");
			context.Expect(system.find("SubmitOneShotRoute") != std::string::npos
				&& system.find("RequestRoute") != std::string::npos,
				"Direct route queries must remain available independently of optional agent state and commands.");
		});

		registry.Add("Engine.NavigationLifecycle.PlayModeTerrainRepublishInvalidatesBootstrap", [](TestContext& context) {
			const std::string header = ReadSource("Engine/Core/MainEngine.h");
			const std::string source = ReadSource("Engine/Core/MainEngine.cpp");
			context.Expect(header.find("lastNavigationBootstrapTerrainCollisionStatus") != std::string::npos
				&& header.find("lastNavigationBootstrapTerrainRecipeGeneration") != std::string::npos
				&& header.find("lastNavigationBootstrapPhysicsWorldGeneration") != std::string::npos,
				"The bootstrap cache must retain collision availability and physics-world ownership beside authored terrain generation.");
			// Match comparison semantics without coupling this source contract to
			// wrapping: diagnostic declarations may split a comparison across lines.
			context.Expect(std::regex_search(source, std::regex(R"(terrainCollisionStatus\s*!=\s*lastNavigationBootstrapTerrainCollisionStatus)"))
				&& std::regex_search(source, std::regex(R"(terrainRecipeGeneration\s*!=\s*lastNavigationBootstrapTerrainRecipeGeneration)"))
				&& std::regex_search(source, std::regex(R"(physicsWorldGeneration\s*!=\s*lastNavigationBootstrapPhysicsWorldGeneration)")),
				"A Pending-to-Current terrain republish or fresh physics world must reattempt automatic navigation capture after Play Mode restart.");
		});

		registry.Add("Engine.NavigationLifecycle.StaticColliderBridgeDoesNotEscalateSpatialPublication", [](TestContext& context) {
			const std::string source = ReadSource("Engine/Core/MainEngine.cpp");
			// A static-collider SetWorldTransform publishes a WorldSpatial snapshot as
			// part of ordinary scene visibility. Its source-specific bridge has already
			// staged the replacement before this Frame End decision, so a global spatial
			// generation must not discard that bounded work and recapture every source.
			context.Expect(source.find("ObserveCommittedNavigationSceneMutations(result)") != std::string::npos
				&& source.find("FindNavigationSourceTransformMismatch") != std::string::npos
				&& source.find("spatialGeneration != lastNavigationBootstrapSpatialGeneration") == std::string::npos,
				"A bridged static-collider transform must use source evidence rather than trigger a full bootstrap solely because spatial publication advanced.");
		});

		registry.Add("Engine.NavigationLifecycle.ResetsDirtySequenceForNewWorld", [](TestContext& context) {
			Navigation::NavigationSystem& system = Navigation::NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			const TerraEngine::UUID sourceId = TerraEngine::GenerateUUID();
			const auto receipt = [sourceId](Navigation::NavigationGeneration sequence) {
				// A committed transform receipt is the same immutable input delivered by
				// the scene boundary. The test varies only its per-world sequence.
				Scene::NavigationSourceMutationReceipt value{};
				value.accepted = true;
				value.transactionStatus = Scene::TransactionStatus::Committed;
				value.intent.sourceId = sourceId;
				value.intent.operation = Scene::NavigationSourceMutationOperation::Move;
				value.intent.oldBounds = {{{0, 0, 0}, {-2.0, -1.0, -2.0}},
					{{0, 0, 0}, {0.0, 2.0, 2.0}}, true};
				value.intent.newBounds = {{{0, 0, 0}, {-1.0, -1.0, -2.0}},
					{{0, 0, 0}, {1.0, 2.0, 2.0}}, true};
				value.intent.committedSourceRevision = sequence;
				value.intent.committedChangeSequence = sequence;
				value.intent.physicalReadyRevision = 1U;
				value.intent.oldEnabled = true;
				value.intent.newEnabled = true;
				return value;
			};

			const auto oldWorldReceipt = receipt(1000U);
			const bool oldWorldAccepted = system.ObserveSceneMutationReceipts(
				std::span<const Scene::NavigationSourceMutationReceipt>{&oldWorldReceipt, 1U}) == 1U;

			// Shutdown/Initialize begins an unrelated scene transaction sequence.
			system.Shutdown();
			system.Initialize();
			const auto newWorldReceipt = receipt(1U);
			const bool newWorldAccepted = system.ObserveSceneMutationReceipts(
				std::span<const Scene::NavigationSourceMutationReceipt>{&newWorldReceipt, 1U}) == 1U;

			// Revocation creates another world in the same singleton, so it must reset
			// the ordering certificate just as a full shutdown does.
			system.RevokeAllOwners(Navigation::NavigationReason::SceneReplaced);
			const auto revokedWorldReceipt = receipt(1U);
			const bool revokedWorldAccepted = system.ObserveSceneMutationReceipts(
				std::span<const Scene::NavigationSourceMutationReceipt>{&revokedWorldReceipt, 1U}) == 1U;

			context.Expect(oldWorldAccepted && newWorldAccepted && revokedWorldAccepted
				&& system.DynamicDirtyIndexForTesting().LatestChangeSequence() == 1U
				&& system.DynamicDirtyIndexForTesting().PendingPreparationCount() == 1U,
				"Each new navigation world must accept its fresh lower receipt sequence instead of inheriting retired-world ordering.");
			system.Shutdown();
		});
	}
}
