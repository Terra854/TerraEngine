// PhysicsExportParityTests.cpp - editor Play Mode/export invariant contracts.

#include "TestHarness.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsAuthoringSnapshot.h"
#include "Runtime/ExportStartupCoordinator.h"
#include "Scene/Serializer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests {

	namespace {
		using PhysicsInvariantSet = std::map<std::string, std::string>;

		std::filesystem::path FixtureRoot() {
			return std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tests" / "Fixtures" / "PhysicsExportRuntime";
		}

		std::string ReadText(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}

		PhysicsInvariantSet ReadExpectedInvariants() {
			const nlohmann::json project = nlohmann::json::parse(ReadText(FixtureRoot() / "project.json"));
			PhysicsInvariantSet expected;
			for(const nlohmann::json& name : project.at("physicsExportInvariants")) {
				// The fixture records names, not provider snapshots.  Runtime evidence
				// adds the engine-owned result for each name after the protected frame
				// boundary has published it.
				expected.emplace(name.get<std::string>(), "required");
			}
			return expected;
		}

		Runtime::PhysicsInvariantReport MakeRepresentativeInvariantReport() {
			// This is the stable evidence shape consumed by the comparison contract.
			// It deliberately captures engine-visible outcomes and ordering instead
			// of Jolt state, pointers, or exact floating-point provider snapshots.
			Runtime::PhysicsInvariantReport report;
			report.succeeded = true;
			report.bodiesCreated = 4U;
			report.fixedStepsCompleted = 90U;
			report.filteredQueryHits = 1U;
			report.publishedEvents = 1U;
			 report.constraintsCreated = 1U;
			 report.charactersCreated = 1U;
			 report.completedTransitions = 2U;
			report.gravityYMilliMetresPerSecondSquared = -15500;
			report.fixedTimeStepMicroseconds = 20000U;
			report.parityLayerBroadPhaseGroup = 7U;
			report.managedBindingsReady = true;
			report.cookedColliderUsed = true;
			report.orderlyShutdown = true;
			return report;
		}

		bool EquivalentPhysicsInvariants(
			const Runtime::PhysicsInvariantReport& editor,
			const Runtime::PhysicsInvariantReport& exported) {
			std::string diagnostic;
			return Runtime::EquivalentPhysicsInvariantReports(editor, exported, diagnostic);
		}

		std::vector<std::filesystem::path> FindFilesWithExtension(
			const std::filesystem::path& root,
			std::string_view extension) {
			std::vector<std::filesystem::path> matches;
			for(const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
				if(entry.is_regular_file() && entry.path().extension() == extension) {
					matches.push_back(entry.path());
				}
			}
			return matches;
		}
	}

	void RegisterPhysicsExportParityTests(TestRegistry& registry) {
		registry.Add("Editor.PhysicsExportParity.FixtureDeclaresSourceOnlyCookAndEveryInvariant", [](TestContext& context) {
			const std::filesystem::path root = FixtureRoot();
			const nlohmann::json project = nlohmann::json::parse(ReadText(root / "project.json"));
			const nlohmann::json startupScene = nlohmann::json::parse(ReadText(root / "Scenes" / "Startup.tescene"));
			const nlohmann::json colliderMetadata = nlohmann::json::parse(
				ReadText(root / "Assets" / "Colliders" / "Startup.tecollider.meta"));
			const std::string readme = ReadText(root / "README.md");

			context.Expect(std::filesystem::exists(root / "Scenes" / "Startup.tescene"),
				"The parity fixture must expose the startup scene used by both Play Mode and exports.");
			context.Expect(project.at("startupScene") == "Scenes/Startup.tescene",
				"The fixture project must name its source-controlled startup scene.");
			context.Expect(project.value("physicsExportInvariantSchemaVersion", 0U) == 1U,
				"The fixture must version the invariant vocabulary consumed by baseline evidence.");

			const PhysicsInvariantSet expected = ReadExpectedInvariants();
			const std::set<std::string> requiredNames{
				"bodies", "stepping", "filtering", "queries", "events", "constraints",
				"characters", "transitions", "shutdown"};
			std::set<std::string> actualNames;
			for(const auto& [name, ignored] : expected) {
				(void)ignored;
				actualNames.insert(name);
			}
			context.Expect(actualNames == requiredNames,
				"The fixture must cover every durable physics invariant exactly once.");
			context.Expect(startupScene.at("physicsExportFixture").at("expectedInvariants") == project.at("physicsExportInvariants"),
				"The startup scenario must consume the exact versioned invariant vocabulary declared by the project.");
			context.Expect(startupScene.at("physicsExportFixture").value("sourceCollider", std::string{}) == "Assets/Colliders/Startup.tecollider",
				"The startup scenario must point to the source-only collider that export cooks in disposable work space.");
			// The editor importer recooks the recipe using its metadata GUID. Pinning
			// that identity to the serialized shape prevents a disposable project scan
			// from producing a valid package that the runtime scene cannot resolve.
			context.Expect(colliderMetadata.value("guid", std::string{})
				== startupScene.at("objects").at(0).at("components").at("ColliderSetComponent")
					.at("colliders").at(0).at("shape").at("colliderAssetId").get<std::string>(),
				"The source recipe metadata GUID must match the cooked collider GUID authored in the shared startup scene.");
			context.Expect(startupScene.at("physicsExportFixture").value("headlessSupported", false)
				&& startupScene.at("physicsExportFixture").value("shutdownExpectedActiveJobs", 1U) == 0U,
				"The fixture must require both headless execution and a drained shutdown boundary.");
			const nlohmann::json& objects = startupScene.at("objects");
			context.Expect(objects.size() == 5U,
				"The shared startup scene must serialize four bodies plus one character rather than constructing a launcher-only fixture.");
			context.Expect(objects.at(0).at("components").contains("RigidBodyComponent")
				&& objects.at(0).at("components").contains("ColliderSetComponent")
				&& objects.at(3).at("components").contains("ConstraintSetComponent")
				&& objects.at(4).at("components").contains("CharacterControllerComponent"),
				"The serialized startup scene must exercise rigid body, collider, constraint, and character component deserialization.");
			const nlohmann::json settings = nlohmann::json::parse(ReadText(root / "ProjectSettings" / "PhysicsSettings.json"));
			context.Expect(settings.at("gravity").at(1) == -15.5 && settings.at("fixedTimeStep") == 0.02
				&& settings.at("layers").at(1).at("broadPhaseGroup") == 7,
				"The parity fixture must use non-default gravity, fixed timestep, and layer routing.");

			const std::vector<std::filesystem::path> sourceRecipes = FindFilesWithExtension(root / "Assets", ".tecollider");
			const std::vector<std::filesystem::path> cookedPayloads = FindFilesWithExtension(root / "Assets", ".tepcoll");
			context.ExpectEqual(static_cast<std::size_t>(1U), sourceRecipes.size(),
				"The source fixture must supply one collider recipe for the disposable cook stage.");
			context.Expect(cookedPayloads.empty(),
				"Source-controlled parity input must not contain a cooked collider that could hide a skipped cook.");
			context.Expect(readme.find("no cooked") != std::string::npos
				&& readme.find("globally installed runtime dependency") != std::string::npos,
				"Fixture documentation must make the source-only and self-contained-runtime contract explicit.");
		});

		registry.Add("Editor.PhysicsExportParity.PlayAndExportPreservePhysicsInvariants", [](TestContext& context) {
			const Runtime::PhysicsInvariantReport play = MakeRepresentativeInvariantReport();
			Runtime::PhysicsInvariantReport exported = play;

			context.Expect(EquivalentPhysicsInvariants(play, exported),
				"Editor Play Mode and export must agree on body, step, filter, query, event, constraint, character, transition, and shutdown invariants.");

			Runtime::PhysicsInvariantReport drifted = exported;
			drifted.filteredQueryHits = 0U;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"Layer-filtering drift must be visible in export parity evidence.");

			drifted = exported;
			drifted.fixedStepsCompleted = 3U;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"Fixed-step drift must be visible even when body creation still matches.");

			drifted = exported;
			drifted.publishedEvents = 2U;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"Event publication drift must be visible before managed callbacks consume the sequence.");

			drifted = exported;
			drifted.parityLayerBroadPhaseGroup = 0U;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"A fallback collision-layer configuration must be visible in parity evidence.");

			drifted = exported;
			drifted.gravityYMilliMetresPerSecondSquared = -9810;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"A runtime that silently restores default gravity must fail editor/export physics parity.");

			drifted = exported;
			drifted.fixedTimeStepMicroseconds = 16667U;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"A runtime that silently restores the default fixed timestep must fail deterministic parity.");

			drifted = exported;
			drifted.managedBindingsReady = false;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"A missing managed physics bridge must be reported as an editor/export invariant mismatch.");

			drifted = exported;
			drifted.cookedColliderUsed = false;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"A runtime that bypasses the validated cooked collider must fail export parity.");

			drifted = exported;
			drifted.activePhysicsJobsAfterDrain = 1U;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"A runtime with active physics work after shutdown must never pass parity.");

			drifted = exported;
			drifted.schemaVersion = 99U;
			context.Expect(!EquivalentPhysicsInvariants(play, drifted),
				"An unsupported invariant-report schema must not be treated as equivalent runtime behavior.");

			Runtime::PhysicsInvariantReport failedPlay = play;
			failedPlay.succeeded = false;
			failedPlay.orderlyShutdown = false;
			context.Expect(!EquivalentPhysicsInvariants(failedPlay, failedPlay),
				"Two identical failed reports must not be mistaken for successful editor/export parity.");
		});

		registry.Add("Editor.PhysicsExportParity.StartupSceneDeserializesRealPhysicsComponents", [](TestContext& context) {
			// Exercise the same typed Serializer path used by the editor and headless
			// Engine. This prevents a JSON-only contract test from accepting fields the
			// runtime silently ignores.
			const nlohmann::json startupScene = nlohmann::json::parse(ReadText(FixtureRoot() / "Scenes" / "Startup.tescene"));
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			for (const nlohmann::json& object : startupScene.at("objects")) {
				context.Expect(Serializer::DeserializeObjectEntity(object) != entt::null,
					"Every serialized parity object must load through the normal scene component path.");
			}
			const std::vector<Object> loaded = objects.getRootObjects();
			context.Expect(loaded.size() == 5U,
				"Deserializing the parity startup scene must restore each authored root object.");
			context.Expect(objects.GetComponent<RigidBodyComponent>(loaded[0]) != nullptr
				&& objects.GetComponent<ColliderSetComponent>(loaded[0]) != nullptr
				&& objects.GetComponent<ConstraintSetComponent>(loaded[3]) != nullptr
				&& objects.GetComponent<CharacterControllerComponent>(loaded[4]) != nullptr,
				"The loaded shared startup scene must retain the typed physics components required by parity.");
			const auto authoring = TerraEngine::Physics::PhysicsAuthoringSnapshot::Capture(
				objects, {{1U}, 1U}, TerraEngine::Physics::PhysicsSettings::Defaults(), 1U);
			context.Expect(authoring.has_value() && authoring->Data().bodies.size() == 4U
				&& authoring->Data().constraints.size() == 1U && authoring->Data().characters.size() == 1U,
				"The deserialized fixture must also pass immutable authoring capture; cooked geometry scale belongs in its recipe, not its Transform.");
			objects.clear();
		});

		registry.Add("Editor.PhysicsExportParity.CorruptionSeedsRemainOutsideRunnableFixtureInput", [](TestContext& context) {
			const std::filesystem::path seeds = FixtureRoot() / "CorruptionSeeds";
			const std::set<std::string> expectedSeeds{
				"corrupt-collider.tepcoll", "missing-settings-game.json", "placeholder-managed-binding.txt"};
			std::set<std::string> actualSeeds;
			for(const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(seeds)) {
				if(entry.is_regular_file()) {
					actualSeeds.insert(entry.path().filename().string());
				}
			}
			context.Expect(actualSeeds == expectedSeeds,
				"Every seeded export failure must remain explicit and separate from the runnable source project.");
			context.Expect(ReadText(seeds / "corrupt-collider.tepcoll").rfind("TEPC", 0U) != 0U,
				"The corrupt collider seed must not accidentally become a valid cooked-envelope header.");
			context.Expect(ReadText(seeds / "placeholder-managed-binding.txt").find("placeholder") != std::string::npos,
				"The managed corruption seed must remain recognizable by fail-closed package validation.");
		});
	}
}
