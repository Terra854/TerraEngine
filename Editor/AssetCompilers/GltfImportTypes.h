// GltfImportTypes.h - Compatibility aliases for the shared model package import contract.
//
// The glTF importer was the first package importer and several editor/tests still
// use the original names. Aliasing them keeps that API stable while FBX and later
// package formats share the same sidecar and generated sub-asset records.

#pragma once

#include "ModelPackageImportTypes.h"

namespace TerraEngine::AssetPipeline
{
	using GltfPackageKind = ModelPackageKind;
	using GltfImportStatus = ModelImportStatus;
	using GltfDiagnosticSeverity = ModelDiagnosticSeverity;
	using GltfTextureUsage = ModelTextureUsage;
	using GltfTextureSemantic = ModelTextureSemantic;
	using ImportedGltfPackage = ImportedModelPackage;
	using GltfImportDiagnostic = ModelImportDiagnostic;
	using GeneratedGltfTextureAsset = GeneratedModelTextureAsset;
	using GeneratedGltfMaterialAsset = GeneratedModelMaterialAsset;
	using GeneratedGltfMeshAsset = GeneratedModelMeshAsset;
	using GltfMaterialAssignment = ModelMaterialAssignment;
	using GltfImportResult = ModelPackageImportResult;
}
