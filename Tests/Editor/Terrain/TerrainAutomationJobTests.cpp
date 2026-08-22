// TerrainAutomationJobTests.cpp -- Durable terminal terrain operation records.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Automation/EditorAutomationIdempotencyStore.h"
#include "Editor/TerraEngine-Editor/Automation/EditorAutomationJobService.h"
#include "Editor/TerraEngine-Editor/Automation/EditorAutomationService.h"
#include "Editor/TerraEngine-Editor/Automation/Providers/TerrainAutomationProvider.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainAutomationAdapter.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Core/Scheduling/JobGraph.h"

#include <algorithm>

namespace TerraEngine::EditorTests
{
	void RegisterTerrainAutomationJobTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainAutomation.ProviderRegistersExecutableBoundedCapabilities", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			const AutomationHandlerResolver resolver = [](std::string_view name) {
				return AutomationHandler{[operation = std::string(name)](const AutomationRequestContext&) {
					return AutomationDispatchResult{false, ErrorCategory::Unavailable, operation + " requires an injected terrain adapter.", {}};
				}};
			};
			TerrainAutomationProvider provider(resolver);
			const auto capabilities = provider.GetCapabilities();
			const auto rebuild = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& value) { return value.descriptor.name == "terrain.rebuild"; });
			const bool allExecutable = std::all_of(capabilities.begin(), capabilities.end(), [](const auto& value) { return static_cast<bool>(value.handler) && !value.descriptor.execution.visibilityBoundary.empty(); });
			const auto hasCapability = [&](std::string_view name) { return std::any_of(capabilities.begin(), capabilities.end(), [&](const auto& value) { return value.descriptor.name == name; }); };
			context.Expect(provider.GetProviderId() == "terrain" && capabilities.size() == 18u && allExecutable && rebuild != capabilities.end()
				&& hasCapability("terrain.asset.inspect") && hasCapability("terrain.session.begin")
				&& hasCapability("terrain.stroke.apply") && hasCapability("terrain.layers.set")
				&& hasCapability("terrain.holes.set") && hasCapability("terrain.operation.start")
				&& hasCapability("terrain.diagnostics.get")
				&& rebuild->descriptor.approvalPolicy == AutomationApprovalPolicy::HighImpact && rebuild->descriptor.execution.protectedCommit,
				"Terrain discovery must register executable handlers, bounded visibility metadata, and high-impact approval for rebuild work.");
		});

		registry.Add("Editor.TerrainAutomation.DurableJobReconnectCancellationAndTerminalReceipt", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			EditorAutomationJobService jobs(2u);
			const auto job = jobs.Create({"terrain.rebuild", "terrain.rebuild", "request", "correlation", "key", true});
			const bool progressed = jobs.SetState(job.jobId, AutomationJobState::Running, "terrain_collision_prepare")
				&& jobs.PublishProgress(job.jobId, 0.5, "terrain_collision_prepare");
			const auto reconnect = jobs.Get(job.jobId);
			const bool cancelled = jobs.RequestCancellation(job.jobId)
				&& jobs.CompleteCanceled(job.jobId, {{"terminalReceipt", true}, {"lastKnownGoodActive", true}, {"visibilityBoundary", "terrain_authoring_publish"}});
			const auto terminal = jobs.Get(job.jobId);
			context.Expect(progressed && reconnect && reconnect->progress == 0.5 && cancelled && terminal
				&& terminal->state == AutomationJobState::Canceled && terminal->result.value("lastKnownGoodActive", false)
				&& terminal->result.value("visibilityBoundary", "") == "terrain_authoring_publish",
				"A reconnectable terrain job must retain progress and a terminal cancelled receipt while explicitly preserving last-known-good derived state.");
		});

		registry.Add("Editor.TerrainAutomation.InspectPublishesExactAreaObservations", [](TestContext& context) {
			using namespace TerraEngine;
			auto root = std::make_shared<Terrain::TerrainAssetRoot>();
			root->assetUuid = GenerateUUID();
			root->publicationGeneration = 7u;
			root->rootContentHash = {101u, 202u};
			root->authoredBounds = {0, 0, 256, 256};
			Terrain::TerrainAreaDescriptor descriptor;
			descriptor.coordinate = {0, 0};
			descriptor.editGeneration = 4u;
			descriptor.authoredContentHash = {303u, 404u};
			descriptor.channels.heightHash = Terrain::ContentHash{505u, 606u};
			descriptor.minHeightMm = -12;
			descriptor.maxHeightMm = 34;
			Terrain::TerrainDirectoryShard shard;
			shard.descriptors.emplace(descriptor.coordinate, descriptor);
			root->directoryShards.push_back(std::move(shard));

			Terrain::TerrainAssetStore store(root);
			Editor::Terrain::TerrainAuthoringSystem authoring;
			Editor::Terrain::TerrainAutomationAdapter adapter;
			adapter.Configure(store, authoring);
			Editor::Automation::EditorAutomationService service;
			service.SetTerrainAutomationAdapter(&adapter);
			Editor::Automation::AutomationRequestContext request;
			request.requestId = "terrain-inspect-request";
			request.correlationId = "terrain-inspect-correlation";
			request.state.projectAvailable = true;
			request.grantedPermission = Editor::Automation::AutomationPermission::Read;
			request.arguments = nlohmann::json::object();
			const auto inspected = service.GetRegistry().Dispatch("terrain.asset.inspect", 1u, request);
			const auto areas = inspected.data.value("areas", nlohmann::json::array());

			context.Expect(inspected.ok && areas.size() == 1u
				&& areas[0].value("areaX", "") == "0"
				&& areas[0].value("editGeneration", "") == "4"
				&& areas[0].value("authoredContentHash", "")
					== Terrain::ToHexString(descriptor.authoredContentHash)
				&& areas[0].value("heightHash", "")
					== Terrain::ToHexString(*descriptor.channels.heightHash)
				&& inspected.data.value("totalAreaCount", 0u) == 1u
				&& !inspected.data.value("areasTruncated", true),
				"Terrain inspection must expose the exact immutable observation tuple required by a later generation-checked edit.");
		});

		registry.Add("Editor.TerrainAutomation.IdempotencyReplaysExactTerminalResultAndRejectsMismatch", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			EditorAutomationIdempotencyStore idempotency(4u);
			const IdempotencyRequest first{"local", "terrain.stroke", "stroke-1", "fingerprint-a", {{"state", "queued"}}};
			const auto admitted = idempotency.Begin(first);
			const bool published = idempotency.PublishResult("local", "terrain.stroke", "stroke-1", {{"publishedRootHash", "abcd"}}, true);
			const auto replay = idempotency.Begin(first);
			auto mismatch = first; mismatch.fingerprint = "fingerprint-b";
			const auto conflict = idempotency.Begin(mismatch);
			context.Expect(admitted.disposition == IdempotencyDisposition::NewRequest && published
				&& replay.disposition == IdempotencyDisposition::Replay && replay.terminal && replay.result.value("publishedRootHash", "") == "abcd"
				&& conflict.disposition == IdempotencyDisposition::Conflict,
				"An identical retry must replay one terminal terrain result while a reused key with different inputs fails before mutation.");
		});

		registry.Add("Editor.TerrainAutomation.CollisionRebuildJobCompletesOnlyAfterProtectedPhysicsPublication", [](TestContext& context) {
			using namespace TerraEngine;
			auto root = std::make_shared<Terrain::TerrainAssetRoot>();
			root->assetUuid = GenerateUUID(); root->publicationGeneration = 4u;
			root->rootContentHash = {41u, 42u}; root->authoredBounds = {0, 0, 256, 256};
			Terrain::TerrainAssetStore store(root);
			Physics::TerrainHeightFieldRecipe recipe;
			recipe.terrainAssetId = root->assetUuid; recipe.authoredGeneration = 4u; recipe.authoredHash = 4142u;
			recipe.heightMillimetres.assign(Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			recipe.holeBits.assign(Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);

			auto& collision = Physics::Terrain::TerrainCollisionSystem::Instance();
			collision.Initialize();
			collision.ConfigurePublication(&store, [](Physics::TerrainCollisionReplacementPacket) {
				Physics::TerrainCollisionReplacementReceipt receipt; receipt.committed = true; return receipt;
			});
			Editor::Terrain::TerrainAutomationAdapter adapter;
			adapter.SetCommandCallback([&](std::string_view capability, const nlohmann::json&) {
				if(capability != "terrain.rebuild")
					return Editor::Terrain::TerrainAutomationCommandResult{false, "UnsupportedOperation", "Unexpected command."};
				collision.SetPreparedCandidate(Physics::Terrain::PrepareTerrainCollisionCandidate(
					recipe, root->rootContentHash, root->publicationGeneration));
				return Editor::Terrain::TerrainAutomationCommandResult{true, "Pending", "Queued.", std::nullopt,
					{{"sourceRootHash", Terrain::ToHexString(root->rootContentHash)},
					 {"sourcePublicationGeneration", root->publicationGeneration}}};
			});
			Editor::Automation::EditorAutomationService service;
			service.SetTerrainAutomationAdapter(&adapter);
			Editor::Automation::AutomationRequestContext request;
			request.requestId = "terrain-collision-request"; request.correlationId = "terrain-collision-correlation";
			request.idempotencyKey = "terrain-collision-key"; request.arguments = {{"target", "collision"}};
			request.state.projectAvailable = true;
			request.grantedPermission = Editor::Automation::AutomationPermission::ProjectWrite;
			const auto accepted = service.GetRegistry().Dispatch("terrain.rebuild", 1u, request);
			const std::string jobId = accepted.data.value("jobId", std::string{});
			const auto beforeBoundary = service.GetJobService().Get(jobId);
			auto workloads = collision.DeclareFrameWorkloads(0.0f);
			TerraEngine::Scheduling::JobGraphContext graphContext;
			if(workloads.size() == 2u && workloads.back().work) workloads.back().work(graphContext);
			service.PumpTerrainAutomationJobsAtProtectedFrame();
			const auto afterBoundary = service.GetJobService().Get(jobId);
			context.Expect(accepted.ok && beforeBoundary && beforeBoundary->state == Editor::Automation::AutomationJobState::Running
				&& afterBoundary && afterBoundary->state == Editor::Automation::AutomationJobState::Succeeded
				&& afterBoundary->result.value("visibilityBoundary", "") == "terrain_collision_publish_before_physics",
				"A durable terrain collision job must remain reconnectable until the protected physics workload publishes its exact source generation.");
			collision.ClearPublication(&store);
			collision.Shutdown();
		});
	}
}
