// PhysicsFrameMigrationTests.cpp - atomic frame-migration regression contracts.

#include "../TestHarness.h"

#include "Physics/PhysicsTransactions.h"

namespace TerraEngine::Tests {
	void RegisterPhysicsFrameMigrationTests(TestRegistry& registry) {
		registry.Add("Physics.FrameMigration.DestinationFailureRetainsSourceAuthority", [](TestContext& context) {
			Physics::PhysicsFrameMigrationRequest request;
			request.id = "destination-failure";
			request.destinationFrame = {{22U}, 1U};
			request.expectedDestinationResidencyGeneration = 1U;
			Physics::PhysicsFrameMigrationMember member;
			member.objectId = TerraEngine::GenerateUUID();
			member.sourceMembership = {{{21U}, 1U}, {{21U}, 1U}, 4U};
			request.members.push_back(member);
			bool rolledBack = false;
			const Physics::PhysicsFrameMigrationReceipt receipt = Physics::CommitPhysicsFrameMigration(request, {
				.stageDestination = [](const auto&) { return false; },
				.commitDestinationAndReleaseSource = [](const auto&) { return true; },
				.rollbackDestination = [&rolledBack](const auto&) { rolledBack = true; }});
			context.Expect(!receipt.accepted && receipt.state == Physics::PhysicsFrameMigrationState::RolledBack && rolledBack,
				"Failed destination staging must roll back before the source frame is released.");
		});

		registry.Add("Physics.FrameMigration.RejectsIncompleteConnectedClosure", [](TestContext& context) {
			Physics::PhysicsFrameMigrationRequest request;
			request.id = "missing-joint-partner";
			request.destinationFrame = {{32U}, 1U};
			request.expectedDestinationResidencyGeneration = 1U;
			Physics::PhysicsFrameMigrationMember member;
			member.objectId = TerraEngine::GenerateUUID();
			member.sourceMembership = {{{31U}, 1U}, {{31U}, 1U}, 1U};
			member.connectedObjectIds.push_back(TerraEngine::GenerateUUID());
			request.members.push_back(member);
			const Physics::PhysicsFrameMigrationReceipt receipt = Physics::CommitPhysicsFrameMigration(request, {});
			context.Expect(!receipt.accepted, "A cross-frame constraint closure must be rejected before provider staging.");
		});

		registry.Add("Physics.FrameMigration.TenThousandDeterministicMovesRetainSourceUntilCommit", [](TestContext& context) {
			bool sourceAuthoritative = true;
			std::size_t staged = 0U, committed = 0U;
			for (std::uint32_t iteration = 0U; iteration < 10'000U; ++iteration)
			{
				Physics::PhysicsFrameMigrationRequest request;
				request.id = "deterministic-" + std::to_string(iteration);
				request.destinationFrame = {{102U}, 1U}; request.expectedDestinationResidencyGeneration = iteration + 1U;
				Physics::PhysicsFrameMigrationMember member;
				member.objectId = TerraEngine::GenerateUUID();
				member.sourceMembership = {{{101U}, 1U}, {{101U}, 1U}, iteration + 1U};
				member.canonicalPose.position.cell = {1'000'000'000'000LL, static_cast<std::int64_t>(iteration), 0LL};
				request.members = {member}; sourceAuthoritative = true;
				const auto receipt = Physics::CommitPhysicsFrameMigration(request, {
					.stageDestination = [&](const auto&) { ++staged; return sourceAuthoritative; },
					.commitDestinationAndReleaseSource = [&](const auto&) { ++committed; sourceAuthoritative = false; return true; },
					.rollbackDestination = [&](const auto&) { sourceAuthoritative = true; }});
				if (!receipt.accepted || sourceAuthoritative || receipt.migratedMemberCount != 1U) { context.Expect(false, "Every deterministic migration must stage before source release."); return; }
			}
			context.Expect(staged == 10'000U && committed == 10'000U,
				"Repeated distant migrations must retain a deterministic one-stage/one-commit sequence.");
		});

		registry.Add("Physics.FrameMigration.CommitFailureRollsBackAndPreservesSourceAuthority", [](TestContext& context) {
			Physics::PhysicsFrameMigrationRequest request;
			request.id = "commit-failure"; request.destinationFrame = {{42U}, 1U}; request.expectedDestinationResidencyGeneration = 1U;
			Physics::PhysicsFrameMigrationMember member; member.objectId = TerraEngine::GenerateUUID(); member.sourceMembership = {{{41U}, 1U}, {{41U}, 1U}, 1U}; request.members = {member};
			bool sourceAuthoritative = true, rollbackCalled = false;
			const auto receipt = Physics::CommitPhysicsFrameMigration(request, {
				.stageDestination = [&](const auto&) { return sourceAuthoritative; },
				.commitDestinationAndReleaseSource = [&](const auto&) { return false; },
				.rollbackDestination = [&](const auto&) { rollbackCalled = true; sourceAuthoritative = true; }});
			context.Expect(!receipt.accepted && receipt.state == Physics::PhysicsFrameMigrationState::RolledBack && rollbackCalled && sourceAuthoritative,
				"A failed protected commit must erase staged destination state without releasing source authority.");
		});
	}
}
