// NavigationOutcomeParityTests.cpp - public editor navigation-agent outcome parity checks.
//
// These assertions dispatch the registered JSON tools. They intentionally do
// not call private serializer helpers or seed agent results, because automation
// must reflect only records that the real protected runtime has published.

#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Automation/EditorAutomationService.h"
#include "Editor/TerraEngine-Editor/Core/EditorContext.h"
#include "Editor/TerraEngine-Editor/Project/ProjectManager.h"
#include "Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.h"
#include "Engine/Components/NavigationAgentComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ComponentRegistry.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Navigation/Agents/NavigationAgentSystem.h"
#include "Engine/Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Engine/Navigation/NavigationSystem.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/Serializer.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests {
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace {
		struct JsonOutcomeCase {
			std::string_view state;
			std::string_view outcome;
			std::string_view reason;
		};

		// Each row is observed through a public agent command or receipt read.
		// Keeping these exact strings together makes a change to editor JSON
		// semantics visible without coupling the test to a private formatter.
		constexpr std::array<JsonOutcomeCase, 4U> kAgentReceiptCases{{
			{"Queued", "Queued", "None"},
			{"Applied", "Queued", "None"},
			{"Rejected", "Stale", "SceneReplaced"},
			{"Expired", "Expired", "Expired"}
		}};

		class NavigationAutomationFixture final {
		public:
			NavigationAutomationFixture()
				: root(std::filesystem::temp_directory_path() / "TerraEngineNavigationOutcomeParity" / ToHexString(GenerateUUID())) {
				UndoRedoSystem::Instance().Clear();
				std::filesystem::create_directories(root);
				context.projectManager = std::make_shared<ProjectManager>();
				std::string error;
				ready = context.projectManager->CreateProject(root, "Navigation Outcome Parity", ProjectTemplate::Blank, &error);
				if(!ready) return;
				const std::filesystem::path scenePath = context.projectManager->GetProjectRoot() / "Assets/Scenes/NavigationOutcomeParity.tescene";
				std::filesystem::create_directories(scenePath.parent_path());
				std::ofstream scene(scenePath, std::ios::binary);
				scene << nlohmann::json{{"format", "TerraSceneUnboundedV1"}, {"name", "Navigation Outcome Parity"},
					{"sceneSettings", nlohmann::json::object()}, {"objects", nlohmann::json::array()}}.dump(2);
				scene.close();
				// Open this fixture's independent project lifetime explicitly;
				// changing source roots does not reopen closed scene admission.
				SceneManager::Instance().BeginLoadSession();
				SceneManager::Instance().SetSceneRoot(context.projectManager->GetProjectRoot());
				SceneManager::Instance().SetActiveScene(SceneManager::Instance().LoadScene(scenePath));
				service.SetEditorContext(&context);
			}

			~NavigationAutomationFixture() {
				UndoRedoSystem::Instance().Clear();
				ObjectManager::instance().clear();
				SceneManager::Instance().UnloadAll();
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
			EditorContext context;
			EditorAutomationService service;
			bool ready = false;
		};

		void Advance(Navigation::NavigationSystem& navigation, Navigation::NavigationAgentSystem& agents,
			std::uint32_t frames) {
			for(std::uint32_t frame = 0U; frame < frames; ++frame) {
				navigation.Update(1.0F / 60.0F);
				agents.Update(1.0F / 60.0F);
			}
		}

		bool Matches(const nlohmann::json& value, const JsonOutcomeCase& expected) {
			return value.value("state", std::string{}) == expected.state
				&& value.value("outcome", std::string{}) == expected.outcome
				&& value.value("reason", std::string{}) == expected.reason;
		}

		std::shared_ptr<const Navigation::NavigationGeometrySnapshot> MakeDisconnectedTerrain() {
			Navigation::NavigationGeometryDocument document{};
			document.kind = Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{1.0F, 0.0F, 1.0F}, {8.0F, 0.0F, 8.0F}, {8.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 8.0F},
				{24.0F, 0.0F, 1.0F}, {31.0F, 0.0F, 8.0F}, {31.0F, 0.0F, 1.0F}, {24.0F, 0.0F, 8.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}, {4U, 5U, 6U}, {4U, 7U, 5U}};
			Navigation::NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = Navigation::NavigationSourceKind::Terrain;
			const auto created = Navigation::NavigationGeometrySnapshot::Create(descriptor, document);
			return created.accepted ? created.snapshot : nullptr;
		}

		// Build the same immutable source-index packet supplied by automatic scene
		// capture.  QueueInitialGroundBuild alone intentionally has no source stage,
		// so it cannot exercise deferred on-demand profile preparation.
		Navigation::NavigationSystem::InitialGroundBuildInput MakeAutomaticDisconnectedBuild(
			const Navigation::NavigationProfile& profile) {
			using namespace Navigation;
			std::vector<NavigationEligibleSourceCandidate> candidates;
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
			candidates.reserve(2U);
			sources.reserve(2U);
			const std::array<std::pair<float, float>, 2U> extents{{{1.0F, 8.0F}, {24.0F, 31.0F}}};
			for(const auto [minimumX, maximumX] : extents) {
				NavigationEligibleSourceCandidate candidate{};
				candidate.identity.sourceId = GenerateUUID();
				candidate.identity.objectId = GenerateUUID();
				candidate.identity.objectLifetimeGeneration = 1U;
				candidate.identity.assetId = GenerateUUID();
				candidate.identity.kind = NavigationSourceKind::Terrain;
				candidate.revisions = {1U, 1U, 1U, 1U, 1U};
				NavigationGeometryDocument document{};
				document.kind = NavigationGeometryPayloadKind::TriangleMesh;
				document.vertices = {{minimumX, 0.0F, 1.0F}, {maximumX, 0.0F, 8.0F},
					{maximumX, 0.0F, 1.0F}, {minimumX, 0.0F, 8.0F}};
				document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
				NavigationGeometrySnapshotDescriptor descriptor{};
				descriptor.sourceUuid.value = candidate.identity.sourceId;
				descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(
					candidate.identity, candidate.revisions);
				descriptor.canonicalPlacementRevision = 1U;
				descriptor.physicalReadyRevision = 1U;
				descriptor.sourceKind = NavigationSourceKind::Terrain;
				candidate.geometry = NavigationGeometrySnapshot::Create(descriptor, std::move(document)).snapshot;
				candidate.activeInHierarchy = true;
				candidate.physicalBackingCurrent = true;
				if(candidate.geometry) sources.push_back(candidate.geometry);
				candidates.push_back(std::move(candidate));
			}
			const NavigationEligibleSourceIndexBuildResult indexed = BuildNavigationEligibleSourceIndex(candidates);
			NavigationSystem::InitialGroundBuildInput build{};
			build.profile = profile;
			build.sources = std::move(sources);
			for(const NavigationEligibleSourceCandidate& candidate : candidates) {
				if(!candidate.geometry) continue;
				build.sourceIndex.push_back({candidate.identity.sourceId, candidate.geometry->Descriptor().sourceRevision,
					candidate.revisions.geometryRevision, true, candidate.identity, candidate.revisions});
			}
			build.eligibleSourceIndex = indexed.accepted
				? std::make_shared<const NavigationEligibleSourceIndex>(indexed.index) : nullptr;
			build.terrainPublicationGeneration = 1U;
			return build;
		}
	}

	void RegisterNavigationOutcomeParityTests(TestRegistry& registry) {
		registry.Add("Editor.Navigation.OutcomeParity.AgentAutomationSerializesPublishedReceiptSemantics", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			NavigationAutomationFixture fixture;
			const Object owner = objects.createObject("Navigation Outcome Agent");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			Advance(navigation, agents, 2U);
			const auto world = navigation.PublishedWorld();
			const nlohmann::json bind = fixture.ready && world ? fixture.service.HandleTool("navigation.agent.bind", {
				{"objectId", ToHexString(objects.getUUID(owner))}}, WriteMode::ReadOnly, "navigation-outcome-bind") : nlohmann::json{};
			context.Expect(fixture.ready && world && bind.value("ok", false),
				"The parity fixture must resolve a real published agent before it reads JSON receipt values.");
			if(!fixture.ready || !world || !bind.value("ok", false)) {
				objects.clear(); agents.Shutdown(); navigation.Shutdown(); return;
			}

			const nlohmann::json agent = bind["data"]["agent"];
			Navigation::NavigationLocation destination{};
			destination.world = world->world;
			destination.position.offsetMetres = {4.0, 0.0, 0.0};
			const nlohmann::json queued = fixture.service.HandleTool("navigation.agent.destination.location", {
				{"agent", agent}, {"location", {{"worldId", world->world.worldId.value}, {"worldGeneration", world->world.worldGeneration},
					{"position", Serializer::SerializeWorldPosition(destination.position)}}}, {"producerOrderingToken", 8101U}, {"correlationId", 8101U}},
				WriteMode::WritesEnabled, "navigation-outcome-queued");
			const nlohmann::json queuedReceipt = queued.value("data", nlohmann::json::object()).value("commitReceipt", nlohmann::json::object());
			context.Expect(queued.value("ok", false) && Matches(queuedReceipt, kAgentReceiptCases[0]),
				"The public destination tool must serialize the queued outcome/reason exactly as its copied receipt reports.");

			Advance(navigation, agents, 2U);
			const nlohmann::json applied = fixture.service.HandleTool("navigation.agent.receipt", {
				{"agent", agent}, {"correlationId", 8101U}}, WriteMode::ReadOnly, "navigation-outcome-applied");
			const nlohmann::json appliedReceipt = applied.value("data", nlohmann::json::object()).value("status", nlohmann::json::object());
			context.Expect(applied.value("ok", false) && Matches(appliedReceipt, kAgentReceiptCases[1]),
				"A protected boundary must publish Applied without rewriting the original queued semantic outcome.");

			const nlohmann::json rejectedCommand = fixture.service.HandleTool("navigation.agent.stop", {
				{"agent", agent}, {"expectedConfigurationRevision", (std::uint64_t{1} << 32U)},
				{"producerOrderingToken", 8102U}, {"correlationId", 8102U}}, WriteMode::WritesEnabled, "navigation-outcome-rejected");
			Advance(navigation, agents, 2U);
			const nlohmann::json rejected = fixture.service.HandleTool("navigation.agent.receipt", {
				{"agent", agent}, {"correlationId", 8102U}}, WriteMode::ReadOnly, "navigation-outcome-rejected-read");
			const nlohmann::json rejectedReceipt = rejected.value("data", nlohmann::json::object()).value("status", nlohmann::json::object());
			context.Expect(rejectedCommand.value("ok", false) && rejected.value("ok", false) && Matches(rejectedReceipt, kAgentReceiptCases[2]),
				"A generation precondition mismatch must retain Rejected/Stale/SceneReplaced in the public JSON receipt.");

			const nlohmann::json expired = fixture.service.HandleTool("navigation.agent.receipt", {
				{"agent", agent}, {"correlationId", 8199U}}, WriteMode::ReadOnly, "navigation-outcome-expired");
			const nlohmann::json expiredReceipt = expired.value("data", nlohmann::json::object()).value("status", nlohmann::json::object());
			context.Expect(expired.value("ok", false) && Matches(expiredReceipt, kAgentReceiptCases[3])
				&& expiredReceipt.value("correlationId", std::uint64_t{0}) == 8199U,
				"Bounded receipt eviction must be an explicit Expired/Expired/Expired JSON result retaining the requested correlation.");

			objects.clear();
			agents.Shutdown();
			 navigation.Shutdown();
		});

		registry.Add("Editor.Navigation.OutcomeParity.RouteAutomationSerializesUnavailableCoverage", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager::instance().clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			(void)navigation.ConfigureSettings(Navigation::NavigationSettings::Defaults());
			navigation.Initialize();
			agents.Initialize();
			NavigationAutomationFixture fixture;
			const auto world = navigation.PublishedWorld();
			const nlohmann::json owner = {
				{"worldId", world ? world->world.worldId.value : 0U},
				{"worldGeneration", world ? world->world.worldGeneration : 0U},
				{"ownerId", 91000U}, {"ownerGeneration", 1U}};
			const nlohmann::json profile = Serializer::SerializeNavigationProfile(Navigation::NavigationProfile::Defaults());
			const nlohmann::json request = fixture.ready && world
				? fixture.service.HandleTool("navigation.route.request", {
					{"owner", owner},
					{"start", Serializer::SerializeWorldPosition(Navigation::NavigationLocation{world->world, {}}.position)},
					{"destination", Serializer::SerializeWorldPosition(Navigation::NavigationLocation{world->world, {}}.position)},
					{"profile", profile}}, WriteMode::WritesEnabled, "navigation-route-unavailable")
				: nlohmann::json{};
			const nlohmann::json handle = request.value("data", nlohmann::json::object())
				.value("commitReceipt", nlohmann::json::object()).value("request", nlohmann::json::object());
			Advance(navigation, agents, 12U);
			const nlohmann::json read = fixture.ready && world
				? fixture.service.HandleTool("navigation.route.read", {{"request", handle}},
					WriteMode::ReadOnly, "navigation-route-unavailable-read")
				: nlohmann::json{};
			const nlohmann::json route = read.value("data", nlohmann::json::object()).value("route", nlohmann::json::object());
			context.Expect(fixture.ready && world && request.value("ok", false) && read.value("ok", false)
				&& route.value("outcome", std::string{}) == "UnavailableCoverage"
				&& route.value("reason", std::string{}) == "MissingTerrain",
				"A public route without physically published terrain must serialize UnavailableCoverage/MissingTerrain.");
			ObjectManager::instance().clear();
			agents.Shutdown();
			navigation.Shutdown();
		});

		registry.Add("Editor.Navigation.OutcomeParity.RouteAutomationSerializesPublicRejectionsAndCancellation", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager::instance().clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			Navigation::NavigationSettings settings = Navigation::NavigationSettings::Defaults();
			settings.maxAdmittedRequests = 1U;
			context.Expect(navigation.ConfigureSettings(settings),
				"The route parity case must install a bounded one-request admission policy before initialization.");
			navigation.Initialize();
			agents.Initialize();
			NavigationAutomationFixture fixture;
			const auto world = navigation.PublishedWorld();
			context.Expect(fixture.ready && world,
				"The route parity case requires a real published world and editor automation project.");
			if(!fixture.ready || !world) {
				agents.Shutdown();
				navigation.Shutdown();
				return;
			}

			const nlohmann::json owner = {
				{"worldId", world->world.worldId.value}, {"worldGeneration", world->world.worldGeneration},
				{"ownerId", 91001U}, {"ownerGeneration", 1U}};
			Navigation::NavigationLocation start{};
			start.world = world->world;
			start.position.offsetMetres = {0.0, 0.0, 0.0};
			Navigation::NavigationLocation destination{};
			destination.world = world->world;
			destination.position.offsetMetres = {4.0, 0.0, 0.0};
			const nlohmann::json profile = Serializer::SerializeNavigationProfile(Navigation::NavigationProfile::Defaults());

			// Start from a canonical endpoint, then send an intentionally non-normalized
			// JSON offset. This exercises the public InvalidEndpoint validation path;
			// the serializer is deliberately not used to normalize malformed input.
			nlohmann::json invalidDestination = Serializer::SerializeWorldPosition(destination.position);
			invalidDestination["offsetMetres"][0] = 32768.0;
			const nlohmann::json invalid = fixture.service.HandleTool("navigation.route.request", {
				{"owner", owner}, {"start", Serializer::SerializeWorldPosition(start.position)},
				{"destination", invalidDestination}, {"profile", profile}},
				WriteMode::WritesEnabled, "navigation-route-invalid-endpoint");
			const nlohmann::json invalidError = invalid.value("error", nlohmann::json::object());
			context.Expect(!invalid.value("ok", true)
				&& invalidError.is_object() && invalidError.value("category", std::string{}) == "validation",
				"An invalid endpoint must be rejected by the public route provider before it can consume admission capacity. Actual response: "
					+ invalid.dump());

			const auto requestArguments = [&]() {
				return nlohmann::json{{"owner", owner}, {"start", Serializer::SerializeWorldPosition(start.position)},
					{"destination", Serializer::SerializeWorldPosition(destination.position)}, {"profile", profile}};
			};
			const nlohmann::json accepted = fixture.service.HandleTool("navigation.route.request", requestArguments(),
				WriteMode::WritesEnabled, "navigation-route-cancel-target");
			const nlohmann::json request = accepted.value("data", nlohmann::json::object())
				.value("commitReceipt", nlohmann::json::object()).value("request", nlohmann::json::object());
			context.Expect(accepted.value("ok", false) && !request.empty(),
				"A valid public route request must return a generation-qualified request receipt.");

			const nlohmann::json full = fixture.service.HandleTool("navigation.route.request", requestArguments(),
				WriteMode::WritesEnabled, "navigation-route-capacity");
			context.Expect(!full.value("ok", true)
				&& full.value("error", nlohmann::json::object()).value("category", std::string{}) == "queue_full"
				&& full.value("error", nlohmann::json::object()).value("details", std::string{})
					== "Navigation route admission rejected the copied request.",
				"A second public route request must serialize caller-visible queue_full/RejectedCapacity admission semantics at the configured bound. Actual response: "
					+ full.dump());

			const nlohmann::json cancelled = fixture.service.HandleTool("navigation.route.cancel", {{"request", request}},
				WriteMode::WritesEnabled, "navigation-route-cancel");
			const nlohmann::json read = fixture.service.HandleTool("navigation.route.read", {{"request", request}},
				WriteMode::ReadOnly, "navigation-route-cancel-read");
			const nlohmann::json route = read.value("data", nlohmann::json::object()).value("route", nlohmann::json::object());
			context.Expect(cancelled.value("ok", false) && read.value("ok", false)
				&& route.value("lifecycle", std::string{}) == "Terminal"
				&& route.value("outcome", std::string{}) == "Cancelled",
				"A public cancellation must be visible through the serialized route read as a terminal Cancelled outcome.");

			ObjectManager::instance().clear();
			agents.Shutdown();
			navigation.Shutdown();
			(void)navigation.ConfigureSettings(Navigation::NavigationSettings::Defaults());
		});

		registry.Add("Editor.Navigation.OutcomeParity.RouteAutomationSerializesPendingUnreachableAndBudgetOutcomes", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager::instance().clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			context.Expect(navigation.ConfigureSettings(Navigation::NavigationSettings::Defaults()),
				"The unreachable parity fixture must start with the normal retained-route bound.");
			navigation.Initialize();
			agents.Initialize();
			NavigationAutomationFixture fixture;
			const auto world = navigation.PublishedWorld();
			const auto terrain = MakeDisconnectedTerrain();
			Navigation::NavigationSystem::InitialGroundBuildInput automaticBuild =
				MakeAutomaticDisconnectedBuild(Navigation::NavigationProfile::Defaults());
			context.Expect(fixture.ready && world && terrain && automaticBuild.eligibleSourceIndex
				&& navigation.QueueInitialGroundBuilds({std::move(automaticBuild)}),
				"The public outcome fixture requires a real published world and two disconnected terrain patches.");
			if(!fixture.ready || !world || !terrain) {
				agents.Shutdown();
				navigation.Shutdown();
				return;
			}
			Advance(navigation, agents, 12U);
			const auto published = navigation.PublishedWorld();
			context.Expect(published && published->terrainState == Navigation::NavigationTerrainState::Available,
				"Disconnected terrain must be published by the same immutable geometry path used by runtime navigation.");

			nlohmann::json owner = {
				{"worldId", world->world.worldId.value}, {"worldGeneration", world->world.worldGeneration},
				{"ownerId", 91002U}, {"ownerGeneration", 1U}};
			Navigation::NavigationLocation start{};
			start.world = world->world;
			start.position.offsetMetres = {4.0, 0.0, 4.0};
			Navigation::NavigationLocation destination{};
			destination.world = world->world;
			destination.position.offsetMetres = {28.0, 0.0, 4.0};
			const nlohmann::json profile = Serializer::SerializeNavigationProfile(Navigation::NavigationProfile::Defaults());
			const auto requestArguments = [&](std::size_t maxItems = 4096U) {
				return nlohmann::json{{"owner", owner}, {"start", Serializer::SerializeWorldPosition(start.position)},
					{"destination", Serializer::SerializeWorldPosition(destination.position)}, {"profile", profile},
					{"maxItems", maxItems}};
			};
			nlohmann::json invalidProfileArguments = requestArguments();
			invalidProfileArguments["profile"] = nlohmann::json::object();
			const nlohmann::json invalidProfile = fixture.service.HandleTool("navigation.route.request", invalidProfileArguments,
				WriteMode::WritesEnabled, "navigation-route-invalid-profile");
			context.Expect(!invalidProfile.value("ok", true)
				&& invalidProfile.value("error", nlohmann::json::object()).value("category", std::string{}) == "validation",
				"An incomplete public navigation profile must be rejected before route admission.");

			// Deliberately request a valid profile that has no published coverage yet.
			// This exercises the real deferred profile-build path rather than the
			// already-queued/default-profile route path above.
			nlohmann::json pendingProfile = profile;
			pendingProfile["radiusMetres"] = profile.value("radiusMetres", 0.4) + 0.05;
			nlohmann::json pendingArguments = requestArguments();
			pendingArguments["profile"] = std::move(pendingProfile);
			const nlohmann::json pending = fixture.service.HandleTool("navigation.route.request", pendingArguments,
				WriteMode::WritesEnabled, "navigation-route-pending");
			const nlohmann::json pendingRequest = pending.value("data", nlohmann::json::object())
				.value("commitReceipt", nlohmann::json::object()).value("request", nlohmann::json::object());
			const nlohmann::json pendingRead = fixture.service.HandleTool("navigation.route.read", {{"request", pendingRequest}},
				WriteMode::ReadOnly, "navigation-route-pending-read");
			const nlohmann::json pendingRoute = pendingRead.value("data", nlohmann::json::object()).value("route", nlohmann::json::object());
			context.Expect(pending.value("ok", false) && pendingRead.value("ok", false)
				&& pendingRoute.value("lifecycle", std::string{}) == "PendingPreparation"
				&& pendingRoute.value("outcome", std::string{}) == "PendingPreparation"
				&& pendingRoute.value("reason", std::string{}) == "DirtyCoverage",
				"A valid route requiring on-demand profile preparation must expose PendingPreparation before its protected build completes.");

			Advance(navigation, agents, 24U);
			const nlohmann::json unreachableRead = fixture.service.HandleTool("navigation.route.read", {{"request", pendingRequest}},
				WriteMode::ReadOnly, "navigation-route-unreachable-read");
			const nlohmann::json unreachableRoute = unreachableRead.value("data", nlohmann::json::object()).value("route", nlohmann::json::object());
			context.Expect(unreachableRead.value("ok", false)
				&& unreachableRoute.value("outcome", std::string{}) == "Unreachable",
				"A route between disconnected published terrain patches must serialize the real Unreachable outcome.");

			// A valid profile with an intentionally tiny voxel resolution reaches the
			// real Recast grid-dimension guard before any large allocation. The
			// preparation failure is therefore deterministic and remains observable
			// through the public editor route read.
			nlohmann::json providerFailureProfile = profile;
			providerFailureProfile["horizontalResolutionMetres"] = 0.001;
			nlohmann::json providerFailureArguments = requestArguments();
			providerFailureArguments["profile"] = std::move(providerFailureProfile);
			const nlohmann::json providerFailure = fixture.service.HandleTool("navigation.route.request",
				providerFailureArguments, WriteMode::WritesEnabled, "navigation-route-provider-failure");
			const nlohmann::json providerFailureRequest = providerFailure.value("data", nlohmann::json::object())
				.value("commitReceipt", nlohmann::json::object()).value("request", nlohmann::json::object());
			Advance(navigation, agents, 24U);
			const nlohmann::json providerFailureRead = fixture.service.HandleTool("navigation.route.read",
				{{"request", providerFailureRequest}}, WriteMode::ReadOnly, "navigation-route-provider-failure-read");
			const nlohmann::json providerFailureRoute = providerFailureRead.value("data", nlohmann::json::object())
				.value("route", nlohmann::json::object());
			context.Expect(providerFailure.value("ok", false) && providerFailureRead.value("ok", false)
				&& providerFailureRoute.value("outcome", std::string{}) == "Failed"
				&& providerFailureRoute.value("reason", std::string{}) == "ProviderFailure",
				"A deterministic Recast preparation failure must serialize Failed/ProviderFailure through the editor facade. Actual response: "
					+ providerFailureRead.dump());

			// The patches remain in one provider shard. NearestReachable is the public
			// allowPartial policy, so this must retain the bounded DomainLimit result
			// instead of converting a disconnected frontier into Complete.
			const nlohmann::json partial = fixture.service.HandleTool("navigation.route.request", [&]() {
				nlohmann::json arguments = requestArguments();
				arguments["allowPartial"] = true;
				return arguments;
			}(), WriteMode::WritesEnabled, "navigation-route-partial");
			const nlohmann::json partialRequest = partial.value("data", nlohmann::json::object())
				.value("commitReceipt", nlohmann::json::object()).value("request", nlohmann::json::object());
			Advance(navigation, agents, 24U);
			const nlohmann::json partialRead = fixture.service.HandleTool("navigation.route.read", {{"request", partialRequest}},
				WriteMode::ReadOnly, "navigation-route-partial-read");
			const nlohmann::json partialRoute = partialRead.value("data", nlohmann::json::object()).value("route", nlohmann::json::object());
			const nlohmann::json partialPoints = partialRoute.value("points", nlohmann::json::array());
			const bool partialStopsBeforeRequestedDestination = partialPoints.is_array() && !partialPoints.empty()
				&& partialPoints.back() != partialRoute.value("destination", nlohmann::json::object());
			context.Expect(partial.value("ok", false) && partialRead.value("ok", false)
				&& partialRoute.value("outcome", std::string{}) == "Partial"
				&& partialRoute.value("reason", std::string{}) == "DomainLimit"
				&& partialStopsBeforeRequestedDestination,
				"NearestReachable on disconnected same-shard patches must serialize Partial/DomainLimit with a non-empty route that stops before the requested endpoint.");

			// Recreate the world under a one-point retained-route bound so the
			// reachable budget case cannot inherit the disconnected query's state.
			navigation.Shutdown();
			agents.Shutdown();
			Navigation::NavigationSettings budgetSettings = Navigation::NavigationSettings::Defaults();
			budgetSettings.maxRoutePoints = 1U;
			context.Expect(navigation.ConfigureSettings(budgetSettings),
				"The budget parity fixture must install its one-point retained-route bound before initialization.");
			navigation.Initialize();
			agents.Initialize();
			const auto budgetWorld = navigation.PublishedWorld();
			context.Expect(budgetWorld && navigation.QueueInitialGroundBuild(Navigation::NavigationProfile::Defaults(), {terrain}),
				"The budget case must rebuild the same immutable terrain under its independent output bound.");
			if(!budgetWorld) {
				agents.Shutdown();
				navigation.Shutdown();
				(void)navigation.ConfigureSettings(Navigation::NavigationSettings::Defaults());
				return;
			}
			Advance(navigation, agents, 12U);
			owner["worldId"] = budgetWorld->world.worldId.value;
			owner["worldGeneration"] = budgetWorld->world.worldGeneration;
			start.world = budgetWorld->world;
			destination.world = budgetWorld->world;

			Navigation::NavigationLocation reachableDestination = destination;
			reachableDestination.position.offsetMetres = {7.0, 0.0, 4.0};
			nlohmann::json budgetArguments = requestArguments();
			budgetArguments["destination"] = Serializer::SerializeWorldPosition(reachableDestination.position);
			const nlohmann::json budget = fixture.service.HandleTool("navigation.route.request", budgetArguments,
				WriteMode::WritesEnabled, "navigation-route-budget");
			const nlohmann::json budgetRequest = budget.value("data", nlohmann::json::object())
				.value("commitReceipt", nlohmann::json::object()).value("request", nlohmann::json::object());
			Advance(navigation, agents, 24U);
			const nlohmann::json budgetRead = fixture.service.HandleTool("navigation.route.read", {{"request", budgetRequest}},
				WriteMode::ReadOnly, "navigation-route-budget-read");
			const nlohmann::json budgetRoute = budgetRead.value("data", nlohmann::json::object()).value("route", nlohmann::json::object());
			context.Expect(budget.value("ok", false) && budgetRead.value("ok", false)
				&& budgetRoute.value("outcome", std::string{}) == "BudgetExhausted",
				"A one-point configured route budget must serialize the provider's real BudgetExhausted outcome. Actual response: "
					+ budgetRead.dump());

			ObjectManager::instance().clear();
			agents.Shutdown();
			navigation.Shutdown();
			(void)navigation.ConfigureSettings(Navigation::NavigationSettings::Defaults());
		});
	}
}
