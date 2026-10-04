#include "Runtime/ExportStartupCoordinator.h"
#include "Runtime/ArtifactValidation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

namespace TerraEngine::Runtime {
	namespace {
		void AddGate(BootSummary& summary, std::string name, bool passed, std::string passedMessage, std::string failedMessage) {
			summary.requiredSystems.push_back({
				std::move(name),
				passed ? BootStepStatus::Passed : BootStepStatus::Failed,
				passed ? std::move(passedMessage) : std::move(failedMessage)
			});
		}

		bool HasFailedGate(const BootSummary& summary) {
			for (const BootSystemResult& result : summary.requiredSystems)
			{
				if (result.status == BootStepStatus::Failed)
				{
					return true;
				}
			}

			return false;
		}

		std::string HashBytes(const std::vector<uint8_t>& bytes) {
			// This is the same FNV-1a provenance hash used by the export manifest.
			// It catches archive/manifest drift before any provider bytes are used.
			std::uint64_t hash = 14695981039346656037ULL;
			for(const uint8_t byte : bytes) {
				hash ^= byte;
				hash *= 1099511628211ULL;
			}
			constexpr char hex[] = "0123456789abcdef";
			std::string result(16U, '0');
			for(std::size_t index = 0U; index < result.size(); ++index) {
				result[result.size() - 1U - index] = hex[(hash >> (index * 4U)) & 0xFU];
			}
			return result;
		}

		bool LoadSettings(const nlohmann::json& document, TerraEngine::Physics::PhysicsSettings& settings, std::string& error) {
			if(!document.is_object() || !document.contains("gravity") || !document["gravity"].is_array()
				|| document["gravity"].size() != 3U || !document.contains("layers")
				|| !document["layers"].is_array() || document["layers"].size() != TerraEngine::Physics::kPhysicsCollisionLayerCount) {
				error = "Physics Settings must contain a three-value gravity vector and exactly 32 collision layers.";
				return false;
			}

			settings = TerraEngine::Physics::PhysicsSettings::Defaults();
			settings.schemaVersion = document.value("schemaVersion", settings.schemaVersion);
			settings.gravity = {document["gravity"][0].get<float>(), document["gravity"][1].get<float>(), document["gravity"][2].get<float>()};
			settings.fixedTimeStep = document.value("fixedTimeStep", settings.fixedTimeStep);
			settings.maxSubsteps = document.value("maxSubsteps", settings.maxSubsteps);
			settings.maxAccumulatedTime = document.value("maxAccumulatedTime", settings.maxAccumulatedTime);
			settings.velocityIterations = document.value("velocityIterations", settings.velocityIterations);
			settings.positionIterations = document.value("positionIterations", settings.positionIterations);
			settings.maxCastDistance = document.value("maxCastDistance", settings.maxCastDistance);
			settings.maxShapeExtent = document.value("maxShapeExtent", settings.maxShapeExtent);
			settings.maxHitsPerQuery = document.value("maxHitsPerQuery", settings.maxHitsPerQuery);
			settings.maxRequestsPerBatch = document.value("maxRequestsPerBatch", settings.maxRequestsPerBatch);
			settings.retainedResultTicks = document.value("retainedResultTicks", settings.retainedResultTicks);
			settings.debugVisualizationEnabled = document.value("debugVisualizationEnabled", settings.debugVisualizationEnabled);
			settings.retainedDebugTicks = document.value("retainedDebugTicks", settings.retainedDebugTicks);
			settings.maxDebugPrimitives = document.value("maxDebugPrimitives", settings.maxDebugPrimitives);
			for(std::size_t index = 0U; index < settings.layers.size(); ++index) {
				const nlohmann::json& source = document["layers"][index];
				if(!source.is_object()) { error = "Physics Settings contains a non-object layer."; return false; }
				auto& destination = settings.layers[index];
				destination.id = source.value("id", destination.id);
				destination.name = source.value("name", destination.name);
				destination.collisionMask = source.value("collisionMask", destination.collisionMask);
				destination.broadPhaseGroup = source.value("broadPhaseGroup", destination.broadPhaseGroup);
				destination.enabled = source.value("enabled", destination.enabled);
			}
			const auto validation = settings.Validate();
			if(!validation.accepted) { error = "Physics Settings are invalid: " + validation.message; return false; }
			return true;
		}

		bool MatchesRecordGuid(const nlohmann::json& record, const TerraEngine::UUID& id) {
			const auto parsed = TerraEngine::ParseUUID(record.value("guid", std::string{}));
			return parsed.has_value() && *parsed == id;
		}

	}

	PackagedPhysicsRuntime LoadPackagedPhysicsRuntime(
		const ContentMount& mount,
		bool requireManagedBindings,
		const std::filesystem::path& externalManagedBindingsPath) {
		PackagedPhysicsRuntime result;
		if(!mount.IsMounted() || !mount.Exists("game.json")) {
			result.diagnostic = "Mounted export content does not provide game.json.";
			return result;
		}
		try {
			const std::vector<uint8_t> manifestBytes = mount.ReadAll("game.json");
			const nlohmann::json manifest = nlohmann::json::parse(manifestBytes.begin(), manifestBytes.end());
			if(!manifest.contains("physics")) {
				// Legacy packages remain supported; no physics startup gate is needed.
				result.loaded = true;
				result.diagnostic = "Package does not declare physics content.";
				return result;
			}
			result.declared = true;
			const nlohmann::json& physics = manifest.at("physics");
			if(!physics.is_object() || physics.value("schemaVersion", 0U) != 1U
				|| !physics.contains("provider") || physics["provider"].value("identifier", std::string{}) != "Jolt"
				|| physics["provider"].value("version", std::string{}) != "5.6.1"
				|| physics["provider"].value("revision", std::string{}) != "2e28006e4a367624bb512bf2c37ca99a93d9d1ea"
				|| physics.value("cookFormatVersion", 0U) != TerraEngine::Physics::kJoltCookPayloadFormatVersion
				|| !physics.contains("materials") || !physics["materials"].is_array()
				|| !physics.contains("colliders") || !physics["colliders"].is_array()
				|| !physics.contains("validation") || physics["validation"].value("status", std::string{}) != "validated") {
				result.diagnostic = "Physics export manifest does not require Jolt 5.6.1 revision 2e28006e4a367624bb512bf2c37ca99a93d9d1ea.";
				return result;
			}
			result.providerRuntimeReady = true;
			const nlohmann::json& settingsRecord = physics.at("settings");
			const std::string settingsPath = settingsRecord.value("packagePath", std::string{});
			if(settingsPath.empty() || !mount.Exists(settingsPath)) {
				result.diagnostic = "Physics Settings payload is missing from mounted content.";
				return result;
			}
			const std::vector<uint8_t> settingsBytes = mount.ReadAll(settingsPath);
			if(settingsBytes.empty() || HashBytes(settingsBytes) != settingsRecord.value("contentHash", std::string{})) {
				result.diagnostic = "Physics Settings payload is empty or does not match its manifest hash.";
				return result;
			}
			const nlohmann::json settingsDocument = nlohmann::json::parse(settingsBytes.begin(), settingsBytes.end());
			if(!LoadSettings(settingsDocument, result.settings, result.diagnostic)) return result;
			const std::string layersText = settingsDocument.at("layers").dump();
			const std::vector<uint8_t> layersBytes(layersText.begin(), layersText.end());
			if(HashBytes(layersBytes) != physics.value("layersHash", std::string{})) {
				result.diagnostic = "Physics collision-layer hash does not match mounted Physics Settings.";
				return result;
			}

			for(const nlohmann::json& record : physics.value("materials", nlohmann::json::array())) {
				const std::string path = record.value("packagePath", std::string{});
				if(path.empty() || !mount.Exists(path)) { result.diagnostic = "A packaged physics material is missing."; return result; }
				const std::vector<uint8_t> bytes = mount.ReadAll(path);
				if(bytes.empty() || bytes.size() != record.value("size", 0ULL)
					|| HashBytes(bytes) != record.value("checksum", std::string{})) {
					result.diagnostic = "A packaged physics material is empty or checksum-mismatched.";
					return result;
				}
				const auto validity = ValidateArtifact({std::as_bytes(std::span(bytes)), ArtifactKind::PhysicsMaterial, HostNativeImageTarget(), path});
				if(!validity.IsValid()) { result.diagnostic = validity.code + ": " + path + ": " + validity.diagnostic; return result; }
				const nlohmann::json document = nlohmann::json::parse(bytes.begin(), bytes.end());
				TerraEngine::Physics::PhysicsMaterial material;
				TerraEngine::Physics::PhysicsMaterialDiagnostics diagnostics;
				if(!TerraEngine::Physics::PhysicsMaterial::LoadJson(document, material, diagnostics) || !MatchesRecordGuid(record, material.assetId)) {
					result.diagnostic = "A packaged physics material is malformed or has a GUID mismatch.";
					return result;
				}
				result.materials.push_back({material.assetId, path, material.BuildRuntime()});
			}

			for(const nlohmann::json& record : physics.value("colliders", nlohmann::json::array())) {
				// Provenance binds the opaque provider payload to the authored source
				// and recipe that export validation approved.  Do this before parsing
				// provider bytes so a hand-edited manifest cannot hide a stale cook.
				if(record.value("sourceProvenanceHash", std::string{}).empty()
					|| record.value("recipeHash", std::string{}).empty()) {
					result.diagnostic = "A packaged cooked collider is missing source or recipe provenance.";
					return result;
				}
				const std::string path = record.value("packagePath", std::string{});
				if(path.empty() || !mount.Exists(path)) { result.diagnostic = "A packaged cooked collider is missing."; return result; }
				const std::vector<uint8_t> bytes = mount.ReadAll(path);
				if(bytes.empty() || bytes.size() != record.value("size", 0ULL)
					|| HashBytes(bytes) != record.value("checksum", std::string{})) {
					result.diagnostic = "A packaged cooked collider is empty or checksum-mismatched.";
					return result;
				}
				const auto validity = ValidateArtifact({std::as_bytes(std::span(bytes)), ArtifactKind::CookedCollider, HostNativeImageTarget(), path});
				if(!validity.IsValid()) { result.diagnostic = validity.code + ": " + path + ": " + validity.diagnostic; return result; }
				std::vector<std::byte> binary(bytes.size());
				if(!bytes.empty()) std::memcpy(binary.data(), bytes.data(), bytes.size());
				TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
				const auto collider = TerraEngine::Physics::CookedColliderAsset::Deserialize(binary, diagnostics);
				if(!collider || !MatchesRecordGuid(record, collider->Envelope().assetId)
					|| !collider->CheckCompatibility(TerraEngine::Physics::CookedColliderAsset::HostPlatform(), TerraEngine::Physics::CookedColliderProviderFamily::Jolt,
						TerraEngine::Physics::kJoltCookProviderVersion, TerraEngine::Physics::kJoltCookRevision).compatible) {
					result.diagnostic = "A packaged cooked collider is corrupt, stale, or provider-incompatible.";
					return result;
				}
				result.colliders.push_back({collider->Envelope().assetId, path, collider->Envelope(), std::vector<std::byte>(collider->Payload().begin(), collider->Payload().end())});
			}

			std::vector<uint8_t> managedBytes;
			if(mount.Exists("Managed/Assembly-CSharp.dll")) {
				managedBytes = mount.ReadAll("Managed/Assembly-CSharp.dll");
			} else if(!externalManagedBindingsPath.empty()) {
				// NativeAOT gameplay code is a loadable library staged beside TEPAK,
				// not an asset entry. Validate the same file ScriptingSystem loads.
				std::ifstream managedInput(externalManagedBindingsPath, std::ios::binary);
				if(managedInput) {
					managedInput.seekg(0, std::ios::end);
					const std::streamoff size = managedInput.tellg();
					managedInput.seekg(0, std::ios::beg);
					if(size > 0) {
						managedBytes.resize(static_cast<std::size_t>(size));
						managedInput.read(reinterpret_cast<char*>(managedBytes.data()), size);
						if(!managedInput) managedBytes.clear();
					}
				}
			}
			// Native validity is inspection only; the Scripting startup gate still
			// requires successful native loading and bridge/vtable initialization.
			const auto managedValidity = ValidateArtifact({std::as_bytes(std::span(managedBytes)),
				ArtifactKind::NativeGameplay, HostNativeImageTarget(), "Managed/Assembly-CSharp"});
			result.managedBindingsLoaded = managedValidity.IsValid();
			if(requireManagedBindings && !result.managedBindingsLoaded) {
				result.diagnostic = "Physics export requires a valid native gameplay binding: " + managedValidity.code + ": " + managedValidity.diagnostic;
				return result;
			}
			result.loaded = true;
			result.diagnostic = "Packaged Physics Settings, assets, provider contract, and managed bindings loaded.";
		}
		catch(const std::exception& exception) {
			result.diagnostic = std::string("Packaged physics content is invalid: ") + exception.what();
		}
		return result;
	}

	PackagedNavigationRuntime LoadPackagedNavigationRuntime(const ContentMount& mount)
	{
		PackagedNavigationRuntime result{};
		if(!mount.IsMounted() || !mount.Exists("navigation/runtime-manifest.json")) {
			result.diagnostic = "Package does not declare navigation content.";
			result.loaded = true;
			return result;
		}
		result.declared = true;
		try {
			const std::vector<std::uint8_t> bytes = mount.ReadAll("navigation/runtime-manifest.json");
			const nlohmann::json manifest = nlohmann::json::parse(bytes.begin(), bytes.end());
			if(!manifest.is_object() || manifest.value("schemaVersion", 0U) != 1U
				|| !manifest.contains("dependencies") || !manifest["dependencies"].is_array()
				|| manifest["dependencies"].size() > 4096U) {
				result.diagnostic = "Navigation runtime manifest is malformed or exceeds its dependency limit.";
				return result;
			}
			result.sourceFree = manifest.value("sourceFree", true);
			for(const nlohmann::json& dependency : manifest["dependencies"]) {
				if(!dependency.is_object() || !dependency.contains("path") || !dependency["path"].is_string()) {
					result.diagnostic = "Navigation runtime manifest contains a malformed dependency.";
					return result;
				}
				const std::string path = dependency["path"].get<std::string>();
				const bool optionalCache = dependency.value("optionalCache", false);
				if(path.empty() || std::filesystem::path(path).is_absolute()
					|| path.find("..") != std::string::npos) {
					result.diagnostic = "Navigation dependency path is not a contained logical path.";
					return result;
				}
				if(optionalCache) {
					PackagedNavigationRuntime::OptionalCacheObservation observation{};
					observation.path = path;
					const MountedContentReadResult cacheRead = mount.ReadAllChecked(path, Navigation::kMaximumSavedNavigationBytes);
					observation.present = cacheRead.Succeeded();
					if(observation.present) {
						result.optionalCachePaths.push_back(path);
						PackagedNavigationRuntime::OptionalCacheCandidate candidate{};
						candidate.path = path;
						candidate.bytes.reserve(cacheRead.bytes.size());
						for(const std::uint8_t byte : cacheRead.bytes)
							candidate.bytes.push_back(static_cast<std::byte>(byte));
						result.optionalCacheCandidates.push_back(std::move(candidate));
						// Startup has no authoritative source/settings descriptor here. Never
						// trust fingerprints copied from the optional cache itself; leave route
						// generation to the normal fresh-generation path and expose the reason.
						observation.reason = "present_deferred_until_authoritative_descriptor";
					} else {
						observation.reason = "absent_or_mount_read_failed";
					}
					result.optionalCacheObservations.push_back(std::move(observation));
					continue;
				}
				if(std::filesystem::path(path).extension() != ".tenavgeom" || !mount.Exists(path)) {
					result.diagnostic = "Required navigation geometry companion is missing from mounted content.";
					return result;
				}
				result.geometryPaths.push_back(path);
			}
			if(!result.sourceFree) {
				result.diagnostic = "Navigation runtime manifest declares authored source data; exports must be source-free.";
				return result;
			}
			result.loaded = true;
			result.diagnostic = result.sourceFree
				? "Source-free navigation runtime initialized."
				: "Packaged navigation geometry companions initialized.";
		}
		catch(const std::exception& exception) {
			result.diagnostic = std::string("Packaged navigation content is invalid: ") + exception.what();
		}
		return result;
	}

	ExportStartupCoordinatorResult CoordinateExportStartup(const ExportStartupCoordinatorInput& input) {
		ExportStartupCoordinatorResult result{};
		BootSummary& summary = result.summary;
		summary.projectName = input.projectName;
		summary.version = input.version;
		summary.contentSource = input.contentSource;
		summary.startupScene = input.startupSceneConfigured ? input.startupScene : std::string("<none>");
		summary.renderProfile = input.renderProfile;
		summary.validationEnabled = input.validationEnabled;
		summary.debugEnabled = input.debugEnabled;

		// Exported builds must mount content before metadata or scenes can be
		// trusted. Editor sessions still report the gate as passed because they
		// read directly from the project filesystem instead of a TEPAK mount.
		AddGate(
			summary,
			"Content",
			!input.exportRuntime || input.contentMounted,
			input.exportRuntime ? "Export content mounted" : "Project filesystem content",
			"Export content archive was not mounted");

		AddGate(
			summary,
			"Metadata",
			input.metadataLoaded,
			"Project metadata loaded",
			"Project metadata was not loaded");

		AddGate(
			summary,
			"StartupScene",
			!input.exportRuntime || (input.startupSceneConfigured && input.startupSceneLoaded),
			input.startupSceneConfigured ? "Startup scene loaded" : "No startup scene required",
			input.startupSceneConfigured ? "Startup scene failed to load" : "Startup scene was not configured");

		// The runtime reads all physics payloads from the mounted archive before
		// entry. This records one explicit failure instead of letting a bad cooked
		// shape surface later as a provider-specific startup crash.
		const std::optional<PackagedPhysicsRuntime> mountedPhysics = input.packagedPhysics.has_value()
			? input.packagedPhysics
			: (input.exportRuntime && input.contentMounted
				? std::optional<PackagedPhysicsRuntime>(LoadPackagedPhysicsRuntime(ContentMount::Instance()))
				: std::nullopt);
		if(mountedPhysics.has_value() && mountedPhysics->declared) {
			AddGate(summary, "Physics", mountedPhysics->loaded && mountedPhysics->providerRuntimeReady && mountedPhysics->managedBindingsLoaded,
				"Packaged physics runtime loaded", mountedPhysics->diagnostic);
		}

		const std::optional<PackagedNavigationRuntime> mountedNavigation = input.packagedNavigation.has_value()
			? input.packagedNavigation
			: (input.exportRuntime && input.contentMounted
				? std::optional<PackagedNavigationRuntime>(LoadPackagedNavigationRuntime(ContentMount::Instance()))
				: std::nullopt);
		if(mountedNavigation.has_value() && mountedNavigation->declared) {
			AddGate(summary, "Navigation", mountedNavigation->loaded,
				"Packaged navigation runtime loaded", mountedNavigation->diagnostic);
		}

		for (const ExportStartupSystemCheck& check : input.requiredSystems)
		{
			// Required systems gate runtime entry; optional systems are recorded as
			// warnings so validation mode can still show partial readiness.
			summary.requiredSystems.push_back({
				check.name,
				check.initialized ? BootStepStatus::Passed : (check.required ? BootStepStatus::Failed : BootStepStatus::Warning),
				check.initialized ? "Initialized" : (check.detail.empty() ? "Not initialized" : check.detail)
			});
		}

		summary.runtimeEntryAllowed = !HasFailedGate(summary);
		summary.runtimeEntryReason = summary.runtimeEntryAllowed
			? "All startup gates passed."
			: "One or more startup gates failed.";
		return result;
	}

	std::string SerializePhysicsInvariantReport(const PhysicsInvariantReport& report) {
		return nlohmann::json{{"schemaVersion", report.schemaVersion}, {"succeeded", report.succeeded},
			{"bodiesCreated", report.bodiesCreated}, {"fixedStepsCompleted", report.fixedStepsCompleted},
			{"filteredQueryHits", report.filteredQueryHits}, {"publishedEvents", report.publishedEvents},
			{"constraintsCreated", report.constraintsCreated}, {"charactersCreated", report.charactersCreated},
			{"completedTransitions", report.completedTransitions}, {"activePhysicsJobsAfterDrain", report.activePhysicsJobsAfterDrain},
			{"queuedPhysicsJobsAfterDrain", report.queuedPhysicsJobsAfterDrain}, {"gravityYMilliMetresPerSecondSquared", report.gravityYMilliMetresPerSecondSquared},
			{"fixedTimeStepMicroseconds", report.fixedTimeStepMicroseconds}, {"parityLayerBroadPhaseGroup", report.parityLayerBroadPhaseGroup},
			{"managedBindingsReady", report.managedBindingsReady},
			{"cookedColliderUsed", report.cookedColliderUsed}, {"orderlyShutdown", report.orderlyShutdown}, {"diagnostic", report.diagnostic}}.dump();
	}

	bool EquivalentPhysicsInvariantReports(const PhysicsInvariantReport& editor, const PhysicsInvariantReport& exported, std::string& diagnostic) {
		if(editor.schemaVersion != PhysicsInvariantReport::kSchemaVersion || exported.schemaVersion != PhysicsInvariantReport::kSchemaVersion) {
			diagnostic = "Physics invariant report schema version is unsupported.";
			return false;
		}
		if(!editor.succeeded || !exported.succeeded || !editor.orderlyShutdown || !exported.orderlyShutdown
			|| editor.activePhysicsJobsAfterDrain != 0U || exported.activePhysicsJobsAfterDrain != 0U
			|| editor.queuedPhysicsJobsAfterDrain != 0U || exported.queuedPhysicsJobsAfterDrain != 0U) {
			diagnostic = "Physics invariant reports must both pass, shut down orderly, and drain all provider work.";
			return false;
		}
		if(SerializePhysicsInvariantReport(editor) != SerializePhysicsInvariantReport(exported)) {
			diagnostic = "Editor Play Mode and exported runtime physics invariant reports differ.";
			return false;
		}
		diagnostic.clear();
		return true;
	}
}
