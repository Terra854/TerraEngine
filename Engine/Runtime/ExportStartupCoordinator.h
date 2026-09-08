// ExportStartupCoordinator.h - deterministic exported startup validation
//
// This coordinator is intentionally data-oriented: runtime systems report the
// facts they already know, and the coordinator turns those facts into one boot
// summary plus a single runtime-entry decision. Tests can exercise the startup
// policy without creating windows, render devices, or managed runtimes.

#pragma once

#include "Core/Export.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "Physics/Assets/PhysicsMaterial.h"
#include "Physics/PhysicsSettings.h"
#include "Runtime/BootSummary.h"
#include "Runtime/ContentMount/ContentMount.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Runtime {

struct ExportStartupSystemCheck
{
	std::string name;
	bool required = true;
	bool initialized = false;
	std::string detail;
};

// These records are copied from the archive before scene loading creates a
// physics world.  They contain engine-owned values only, never Jolt pointers,
// so worker-safe scene preparation can later consume them without retaining a
// ContentMount or provider-owned object.
struct PackagedPhysicsCollider
{
	TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	std::string packagePath;
	TerraEngine::Physics::CookedColliderEnvelope envelope;
	std::vector<std::byte> payload;
};

struct PackagedPhysicsMaterial
{
	TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	std::string packagePath;
	TerraEngine::Physics::PhysicsMaterialRuntime runtime;
};

struct PackagedPhysicsRuntime
{
	bool declared = false;
	bool loaded = false;
	bool providerRuntimeReady = false;
	bool managedBindingsLoaded = false;
	TerraEngine::Physics::PhysicsSettings settings = TerraEngine::Physics::PhysicsSettings::Defaults();
	std::vector<PackagedPhysicsMaterial> materials;
	std::vector<PackagedPhysicsCollider> colliders;
	std::string diagnostic;
};

	struct PackagedNavigationRuntime
	{
		bool declared = false;
		bool loaded = false;
		bool sourceFree = true;
		std::vector<std::string> geometryPaths;
		std::vector<std::string> optionalCachePaths;
		std::string diagnostic;
	};

// This compact record is shared by Play Mode, the exported validation launcher,
// and the baseline script. It retains only engine-owned observations, so parity
// never depends on a Jolt pointer, enum value, callback object, or byte layout.
struct PhysicsInvariantReport
{
	static constexpr std::uint32_t kSchemaVersion = 1U;
	std::uint32_t schemaVersion = kSchemaVersion;
	bool succeeded = false;
	std::uint32_t bodiesCreated = 0U;
	std::uint32_t fixedStepsCompleted = 0U;
	std::uint32_t filteredQueryHits = 0U;
	std::uint32_t publishedEvents = 0U;
	std::uint32_t constraintsCreated = 0U;
	std::uint32_t charactersCreated = 0U;
	std::uint32_t completedTransitions = 0U;
	std::uint32_t activePhysicsJobsAfterDrain = 0U;
	std::uint32_t queuedPhysicsJobsAfterDrain = 0U;
	// Quantized authored settings make the parity report reject a runtime that
	// silently falls back to default gravity, tick duration, or layer routing.
	std::int32_t gravityYMilliMetresPerSecondSquared = 0;
	std::uint32_t fixedTimeStepMicroseconds = 0U;
	std::uint32_t parityLayerBroadPhaseGroup = 0U;
	bool managedBindingsReady = false;
	bool cookedColliderUsed = false;
	bool orderlyShutdown = false;
	std::string diagnostic;
};

struct ExportStartupCoordinatorInput
{
	bool exportRuntime = false;
	bool contentMounted = false;
	bool metadataLoaded = false;
	bool startupSceneConfigured = false;
	bool startupSceneLoaded = false;
	bool validationEnabled = false;
	bool debugEnabled = false;
	std::string projectName = "TerraEngine";
	std::string version = "1.0.0";
	std::string contentSource = "<none>";
	std::string startupScene = "<none>";
	std::string renderProfile = "profile=Default";
	// Callers which mounted physics content during early startup pass this result
	// here. When omitted, CoordinateExportStartup reads the active mount itself.
	std::optional<PackagedPhysicsRuntime> packagedPhysics;
	std::optional<PackagedNavigationRuntime> packagedNavigation;
	std::vector<ExportStartupSystemCheck> requiredSystems;
};

struct ExportStartupCoordinatorResult
{
	BootSummary summary;

	bool CanEnterRuntime() const
	{
		return summary.runtimeEntryAllowed;
	}
};

TERRAENGINE_API ExportStartupCoordinatorResult CoordinateExportStartup(const ExportStartupCoordinatorInput& input);

// Reads and validates the complete physics manifest block from a mounted
// archive. It performs no live-world mutation: callers queue settings at their
// declared startup boundary, preserving transactional visibility rules.
TERRAENGINE_API PackagedPhysicsRuntime LoadPackagedPhysicsRuntime(
	const ContentMount& mount,
	bool requireManagedBindings = true,
	const std::filesystem::path& externalManagedBindingsPath = {});

	TERRAENGINE_API PackagedNavigationRuntime LoadPackagedNavigationRuntime(
		const ContentMount& mount);

// Stable JSON is used as a durable evidence record by the installed baseline.
// The comparator rejects any schema, field, or observed-value drift.
TERRAENGINE_API std::string SerializePhysicsInvariantReport(const PhysicsInvariantReport& report);
TERRAENGINE_API bool EquivalentPhysicsInvariantReports(
	const PhysicsInvariantReport& editor,
	const PhysicsInvariantReport& exported,
	std::string& diagnostic);

}
