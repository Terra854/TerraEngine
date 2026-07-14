// PhysicsMaterialAssetWriter.h - Safe editor I/O for .tephysmat assets.
//
// Physics materials are authored JSON, but this adapter keeps editor callers
// from having to duplicate schema validation, GUID checks, or atomic writes.

#pragma once

#include "Physics/Assets/PhysicsMaterial.h"

#include <filesystem>
#include <string>

namespace TerraEngine::AssetPipeline
{
	struct PhysicsMaterialAssetWriteResult
	{
		bool published = false;
		std::string message;
		Physics::PhysicsMaterialDiagnostics diagnostics;
	};

	// Writes a fully validated material through a sibling staging file, then
	// replaces the destination only after the staged JSON can be read back.
	[[nodiscard]] PhysicsMaterialAssetWriteResult WritePhysicsMaterialAsset(
		const std::filesystem::path& outputPath,
		const Physics::PhysicsMaterial& material);

	// Loads and validates an authored material without mutating a caller's
	// destination on failure. This makes reimport safe for a last-known-good
	// material already used by an open scene.
	[[nodiscard]] bool LoadPhysicsMaterialAsset(
		const std::filesystem::path& inputPath,
		Physics::PhysicsMaterial& destination,
		Physics::PhysicsMaterialDiagnostics& diagnostics);
}
