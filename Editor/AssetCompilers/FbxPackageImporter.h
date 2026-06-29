// FbxPackageImporter.h - Assimp-backed FBX package import entry point.
//
// FBX is imported as a package so static meshes, generated materials, extracted
// or referenced textures, assignments, and diagnostics travel through the same
// editor contract as glTF instead of becoming a single loose mesh compile.

#pragma once

#include "ModelPackageImporter.h"

namespace TerraEngine::AssetPipeline
{
	using FbxPackageImportOptions = ModelPackageImportOptions;

	[[nodiscard]] ModelPackageImportResult ImportFbxPackage(const FbxPackageImportOptions& options);
}
