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

struct PackagedPhysicsRuntime
{
	bool declared = false;
	bool loaded = false;
	bool providerRuntimeReady = false;
	bool managedBindingsLoaded = false;
	TerraEngine::Physics::PhysicsSettings settings = TerraEngine::Physics::PhysicsSettings::Defaults();
	std::vector<TerraEngine::Physics::PhysicsMaterialRuntime> materials;
	std::vector<PackagedPhysicsCollider> colliders;
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
	bool requireManagedBindings = true);

}
