#include "TestHarness.h"

#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/MaterialRuntimeContract.h"

namespace TerraEngine::Tests {
	namespace {
		MaterialHeader MakeMaterialHeader() {
			MaterialHeader header{};
			header.magic[0] = 'T';
			header.magic[1] = 'M';
			header.magic[2] = 'A';
			header.magic[3] = 'T';
			header.version = kMaterialCurrentVersion;
			header.headerSize = sizeof(MaterialHeader);
			header.shadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
			header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
			header.baseColorFactor[0] = 0.25f;
			header.baseColorFactor[1] = 0.5f;
			header.baseColorFactor[2] = 0.75f;
			header.baseColorFactor[3] = 1.0f;
			header.emissiveFactor[0] = 0.1f;
			header.emissiveFactor[1] = 0.2f;
			header.emissiveFactor[2] = 0.3f;
			header.metallicFactor = 0.4f;
			header.roughnessFactor = 0.6f;
			header.normalScale = 1.0f;
			header.occlusionStrength = 1.0f;
			header.textureArrayOffset = sizeof(MaterialHeader);
			header.floatParameterOffset = sizeof(MaterialHeader);
			return header;
		}
	}

	void RegisterMaterialRuntimeContractTests(TestRegistry& registry) {
		registry.Add("Engine.MaterialContract.MissingTexturesUseDefaults", [](TestContext& context) {
			const MaterialHeader header = MakeMaterialHeader();
			const MaterialRuntimeContract contract = ResolveMaterialRuntimeContract(header, {});

			context.ExpectEqual(static_cast<std::size_t>(5), contract.slots.size(), "Runtime material contract should always expose five slots.");
			context.Expect(contract.slots[0].slot == MaterialRuntimeSlot::Albedo, "First runtime slot should be albedo.");
			context.Expect(!contract.slots[0].hasTexture, "Missing albedo texture should use base color fallback.");
			context.ExpectEqual(0.25f, contract.slots[0].defaultValue[0], "Albedo fallback should preserve base color red.");
			context.ExpectEqual(0.6f, contract.slots[2].defaultValue[0], "Roughness fallback should preserve roughness factor.");
			context.ExpectEqual(0.4f, contract.slots[3].defaultValue[0], "Metallic fallback should preserve metallic factor.");
			context.Expect(contract.warnings.size() >= 5u, "Missing slots should report fallback warnings.");
		});

		registry.Add("Engine.MaterialContract.TextureBindingsOverrideFallbacks", [](TestContext& context) {
			MaterialHeader header = MakeMaterialHeader();
			TextureBinding bindings[2]{};
			bindings[0].semantic = TextureSemantic::Base;
			bindings[0].textureUuid[0] = 42u;
			bindings[1].semantic = TextureSemantic::Normal;
			bindings[1].textureUuid[0] = 7u;
			header.textureCount = 2u;

			const MaterialRuntimeContract contract = ResolveMaterialRuntimeContract(header, std::span<const TextureBinding>(bindings, 2u));

			Material material{};
			material.resolvedSlots = contract.slots;
			const ResolvedMaterialSlot* albedo = FindResolvedMaterialSlot(material, MaterialRuntimeSlot::Albedo);
			context.Expect(albedo != nullptr, "Resolved contract should expose albedo lookup.");
			context.Expect(albedo->hasTexture, "Authored base texture should satisfy albedo runtime slot.");
			context.ExpectEqual(static_cast<std::uint8_t>(42u), albedo->textureUuid[0], "Albedo should keep authored texture UUID.");
			context.ExpectEqual(static_cast<std::size_t>(3), contract.warnings.size(), "Only unsatisfied slots should report warnings.");
		});
	}
}
