// ImplementationCheck editor persistence tests for validated .tephysmat assets.
#include "../TestHarness.h"

#include "PhysicsMaterialAssetWriter.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::EditorTests
{
	struct MaterialFixture
	{
		std::filesystem::path root = std::filesystem::temp_directory_path()
			/ ("TerraEnginePhysicsMaterialTests_" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::path path = root / "Stone.tephysmat";

		MaterialFixture() { std::filesystem::create_directories(root); }
		~MaterialFixture()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	};

	std::string ReadMaterialFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	}

	void RegisterPhysicsMaterialAssetTests(TestRegistry& registry)
	{
		registry.Add("Physics.Editor.Material.AtomicRoundTripAndSchema", [](TestContext& context) {
			MaterialFixture fixture;
			const auto id = TerraEngine::ParseUUID("0123456789abcdef0123456789abcdef");
			context.Expect(id.has_value(), "Fixture GUID must parse.");
			auto material = TerraEngine::Physics::PhysicsMaterial::Defaults(*id);
			material.dynamicFriction = 0.42F;
			material.restitution = 0.35F;
			material.surfaceTag = "stone";
			const auto write = TerraEngine::AssetPipeline::WritePhysicsMaterialAsset(fixture.path, material);
			context.Expect(write.published, "Validated material must publish.");
			context.Expect(!std::filesystem::exists(fixture.path.string() + ".staged"), "Successful material publication must clean staging files.");
			TerraEngine::Physics::PhysicsMaterial restored;
			TerraEngine::Physics::PhysicsMaterialDiagnostics diagnostics;
			context.Expect(TerraEngine::AssetPipeline::LoadPhysicsMaterialAsset(fixture.path, restored, diagnostics), "Published material must load through the editor adapter.");
			context.Expect(material.assetId == restored.assetId, "Round-trip must preserve the stable asset GUID.");
			context.ExpectEqual(material.dynamicFriction, restored.dynamicFriction, "Round-trip must preserve authored friction.");
			context.ExpectEqual(material.surfaceTag, restored.surfaceTag, "Round-trip must preserve the surface tag.");
		});

		registry.Add("Physics.Editor.Material.InvalidReplacementPreservesLastGood", [](TestContext& context) {
			MaterialFixture fixture;
			const auto id = TerraEngine::ParseUUID("fedcba9876543210fedcba9876543210");
			context.Expect(id.has_value(), "Fixture GUID must parse.");
			const auto valid = TerraEngine::Physics::PhysicsMaterial::Defaults(*id);
			context.Expect(TerraEngine::AssetPipeline::WritePhysicsMaterialAsset(fixture.path, valid).published, "Initial valid material must publish.");
			const std::string prior = ReadMaterialFile(fixture.path);
			auto invalid = valid;
			invalid.dynamicFriction = -1.0F;
			const auto failed = TerraEngine::AssetPipeline::WritePhysicsMaterialAsset(fixture.path, invalid);
			context.Expect(!failed.published, "Invalid material must be rejected.");
			context.ExpectEqual(prior, ReadMaterialFile(fixture.path), "Rejected material must preserve the prior bytes.");
			context.Expect(!std::filesystem::exists(fixture.path.string() + ".staged"), "Rejected material must not leak a staging file.");
		});

		registry.Add("Physics.Editor.Material.MoveRenameAndMigrationPreserveGuid", [](TestContext& context) {
			MaterialFixture fixture;
			const auto id = TerraEngine::ParseUUID("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
			context.Expect(id.has_value(), "Fixture GUID must parse.");
			const auto original = TerraEngine::Physics::PhysicsMaterial::Defaults(*id);
			context.Expect(TerraEngine::AssetPipeline::WritePhysicsMaterialAsset(fixture.path, original).published,
				"A material must publish before its path is moved.");
			const std::filesystem::path movedPath = fixture.root / "Renamed" / "StoneMoved.tephysmat";
			std::filesystem::create_directories(movedPath.parent_path());
			std::filesystem::rename(fixture.path, movedPath);
			TerraEngine::Physics::PhysicsMaterial movedMaterial;
			TerraEngine::Physics::PhysicsMaterialDiagnostics movedDiagnostics;
			context.Expect(TerraEngine::AssetPipeline::LoadPhysicsMaterialAsset(
				movedPath, movedMaterial, movedDiagnostics),
				"A moved and renamed material file must remain loadable.");
			context.Expect(*id == movedMaterial.assetId,
				"A filesystem move and rename must preserve the embedded stable GUID.");

			nlohmann::json moved = TerraEngine::Physics::PhysicsMaterial::Defaults(*id).ToJson();
			moved["path"] = "Materials/Renamed/Stone.tephysmat";
			moved.erase("schemaVersion"); // exercise the explicit v0 -> v1 migration path
			TerraEngine::Physics::PhysicsMaterial restored;
			TerraEngine::Physics::PhysicsMaterialDiagnostics diagnostics;
			context.Expect(TerraEngine::Physics::PhysicsMaterial::LoadJson(moved, restored, diagnostics), "Moved legacy material must remain loadable.");
			context.Expect(*id == restored.assetId, "Move/rename and migration must preserve the stable GUID.");
			context.Expect(!diagnostics.entries.empty(), "Legacy migration must leave an auditable diagnostic.");
		});

		registry.Add("Physics.Editor.Material.ReloadPreservesSelectionAndLastKnownAuthoredBytes", [](TestContext& context) {
			MaterialFixture fixture;
			const auto id = TerraEngine::ParseUUID("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
			context.Expect(id.has_value(), "Fixture GUID must parse.");
			auto authored = TerraEngine::Physics::PhysicsMaterial::Defaults(*id);
			authored.dynamicFriction = 0.25F;
			authored.surfaceTag = "before-reload";
			context.Expect(TerraEngine::AssetPipeline::WritePhysicsMaterialAsset(fixture.path, authored).published,
				"The authored material must publish before a reload can be exercised.");
			const std::string firstAuthoredBytes = ReadMaterialFile(fixture.path);

			// Selection is stored as the durable asset identity, not as a transient
			// loaded object address, so a valid reload must leave it unchanged.
			const TerraEngine::UUID selectedAssetId = *id;
			auto replacement = authored;
			replacement.dynamicFriction = 0.75F;
			replacement.surfaceTag = "after-reload";
			context.Expect(TerraEngine::AssetPipeline::WritePhysicsMaterialAsset(fixture.path, replacement).published,
				"A changed valid material must atomically replace its authored bytes.");
			const std::string replacementAuthoredBytes = ReadMaterialFile(fixture.path);
			context.Expect(firstAuthoredBytes != replacementAuthoredBytes, "A real authored material edit must produce new source bytes.");

			TerraEngine::Physics::PhysicsMaterial reloaded;
			TerraEngine::Physics::PhysicsMaterialDiagnostics reloadDiagnostics;
			context.Expect(TerraEngine::AssetPipeline::LoadPhysicsMaterialAsset(fixture.path, reloaded, reloadDiagnostics),
				"The replacement bytes must reload through the editor adapter.");
			context.Expect(reloaded.assetId == selectedAssetId, "Reloading must preserve the selected material's stable identity.");
			context.ExpectEqual(0.75F, reloaded.dynamicFriction, "Reloading must expose the updated authored value.");

			auto invalid = replacement;
			invalid.restitution = 2.0F;
			context.Expect(!TerraEngine::AssetPipeline::WritePhysicsMaterialAsset(fixture.path, invalid).published,
				"Invalid replacement data must be rejected rather than partially reloaded.");
			context.ExpectEqual(replacementAuthoredBytes, ReadMaterialFile(fixture.path), "A failed reload write must retain the last known good authored bytes.");
			context.Expect(reloaded.assetId == selectedAssetId && reloaded.surfaceTag == "after-reload",
				"A failed replacement must not disturb the selected, already loaded material state.");
		});
	}
}
