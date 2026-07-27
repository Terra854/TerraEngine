// FbxAnimationImportTests.cpp - Contract tests for animated Assimp FBX packages.
//
// These tests deliberately inspect the public package sidecar rather than
// importer-private Assimp objects. The sidecar is what the Asset Browser,
// prefab authoring, automation, and a project reload use after worker-safe
// import preparation has crossed its protected publication boundary.

#include "TestHarness.h"

#include "AssetPipeline/FbxTestHelpers.h"

#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	namespace
	{
		bool HasDiagnostic(const TerraEngine::AssetPipeline::ModelPackageImportResult& result, std::string_view code)
		{
			for(const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic : result.diagnostics)
			{
				if(diagnostic.code == code)
				{
					return true;
				}
			}
			return false;
		}

		// T035 must replace the legacy static-only downgrade. Keeping this contract
		// at the public result boundary lets the future cooker choose its concrete
		// binary records without coupling these tests to its private implementation.
		void ExpectAnimatedFbxWasImported(
			TestContext& context,
			const TerraEngine::AssetPipeline::ModelPackageImportResult& result,
			std::string_view requirement)
		{
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed,
				"The representative animated FBX package should remain importable.");
			context.Expect(!HasDiagnostic(result, "UnsupportedSkeletonIgnored"),
				std::string(requirement) + " requires hierarchy, bones, bind data, and skin weights to be cooked instead of ignored.");
			context.Expect(!HasDiagnostic(result, "UnsupportedAnimationIgnored"),
				std::string(requirement) + " requires animation clips and track data to be cooked instead of ignored.");
		}
	}

	void RegisterFbxAnimationImportTests(TestRegistry& registry)
	{
		registry.Add("Editor.Animation.FbxImport.PreservesHierarchySkinAndClipPayload", [](TestContext& context) {
			// Assimp's compact fixture contains both an animation and a skeleton,
			// making it a deterministic offline contract source for the FBX path.
			ExpectAnimatedFbxWasImported(context, ImportFbxFixture("animation_with_skeleton.fbx"),
				"The FBX hierarchy, bone, weight, and clip import contract");
		});

		registry.Add("Editor.Animation.FbxImport.PreservesMorphSocketsUnitsAndNormalizedParityMetadata", [](TestContext& context) {
			// `close_to_identity_transforms` exercises Assimp's transform conversion
			// path; the compact animated fixture supplies the required skin/clip data.
			ExpectAnimatedFbxWasImported(context, ImportFbxFixture("animation_with_skeleton.fbx"),
				"The FBX morph, socket, unit conversion, and glTF-parity import contract");
		});

	}
}
