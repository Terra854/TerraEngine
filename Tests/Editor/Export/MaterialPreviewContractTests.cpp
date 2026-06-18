#include "TestHarness.h"

#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/MaterialRuntimeContract.h"

namespace TerraEngine::EditorTests {
	namespace {
		MaterialHeader MakePreviewHeader() {
			MaterialHeader header{};
			header.magic[0] = 'T';
			header.magic[1] = 'M';
			header.magic[2] = 'A';
			header.magic[3] = 'T';
			header.version = kMaterialCurrentVersion;
			header.headerSize = sizeof(MaterialHeader);
			header.shadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
			header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
			header.baseColorFactor[0] = 0.2f;
			header.baseColorFactor[1] = 0.3f;
			header.baseColorFactor[2] = 0.4f;
			header.baseColorFactor[3] = 1.0f;
			header.metallicFactor = 0.8f;
			header.roughnessFactor = 0.35f;
			header.normalScale = 1.0f;
			header.textureArrayOffset = sizeof(MaterialHeader);
			header.floatParameterOffset = sizeof(MaterialHeader);
			return header;
		}
	}

	void RegisterMaterialPreviewContractTests(TestRegistry& registry) {
		registry.Add("Editor.MaterialPreview.ContractMatchesRuntimeFallbacks", [](TestContext& context) {
			const MaterialRuntimeContract contract = ResolveMaterialRuntimeContract(MakePreviewHeader(), {});

			const ResolvedMaterialSlot* albedo = nullptr;
			const ResolvedMaterialSlot* emissive = nullptr;
			for (const ResolvedMaterialSlot& slot : contract.slots)
			{
				if (slot.slot == MaterialRuntimeSlot::Albedo)
				{
					albedo = &slot;
				}
				if (slot.slot == MaterialRuntimeSlot::Emissive)
				{
					emissive = &slot;
				}
			}

			context.Expect(albedo != nullptr, "Preview contract should include albedo.");
			context.Expect(emissive != nullptr, "Preview contract should include emissive.");
			context.Expect(!albedo->hasTexture, "Preview should identify albedo fallback when no base texture is authored.");
			context.ExpectEqual(0.2f, albedo->defaultValue[0], "Preview albedo fallback should match the TMAT base factor.");
			context.Expect(!emissive->hasTexture, "Preview should identify emissive fallback when no emissive texture is authored.");
			context.Expect(!contract.warnings.empty(), "Preview should receive runtime fallback warnings.");
		});
	}
}
