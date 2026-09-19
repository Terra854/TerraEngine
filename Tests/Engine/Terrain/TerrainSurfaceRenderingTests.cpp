// TerrainSurfaceRenderingTests.cpp -- control residency and coupled derived publication coverage.

#include "TestHarness.h"

#include "Renderer/Terrain/TerrainSurfaceCache.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Components/TerrainComponent.h"
#include "Terrain/TerrainDerivedSystem.h"
#include "Terrain/TerrainMaterial.h"
#include "Terrain/TerrainLayerStack.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadTerrainRenderingSource(const std::filesystem::path& relativePath)
		{
			// Shader contracts are inspected as dedicated source files because the
			// CPU unit-test executable cannot execute a Vulkan fragment invocation.
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
			std::ostringstream contents;
			contents << input.rdbuf();
			return contents.str();
		}

		bool ContainsTokensInOrder(
			std::string_view source, const std::initializer_list<std::string_view> tokens)
		{
			std::size_t cursor = 0u;
			for(const std::string_view token : tokens)
			{
				cursor = source.find(token, cursor);
				if(cursor == std::string_view::npos) return false;
				cursor += token.size();
			}
			return true;
		}

		template<typename Binding>
		concept FullPbrTerrainLayerBinding = requires(Binding binding)
		{
			binding.baseColorTextureIndex;
			binding.normalTextureIndex;
			binding.ormTextureIndex;
			binding.emissiveTextureIndex;
			binding.baseColorFactor;
			binding.emissiveFactor;
			binding.normalScale;
			binding.occlusionStrength;
			binding.roughnessFactor;
			binding.metallicFactor;
			binding.tileSizeMetres;
			binding.inverseRotationCosSin;
			binding.triplanarSharpness;
			binding.mappingMode;
			binding.stableLayerId;
		};

		template<typename Binding>
		constexpr bool MatchesTerrainGpuLayerAbi() noexcept
		{
			// One 96-byte record uses only four-byte shader scalars and float vectors.
			// The explicit checks prevent an apparently harmless CPU field insertion
			// from shifting every later StructuredBuffer member in HLSL.
			if constexpr(!FullPbrTerrainLayerBinding<Binding>) return false;
			else return std::is_standard_layout_v<Binding>
				&& alignof(Binding) == 16u && sizeof(Binding) == 96u
				&& offsetof(Binding, baseColorTextureIndex) == 0u
				&& offsetof(Binding, normalTextureIndex) == 4u
				&& offsetof(Binding, ormTextureIndex) == 8u
				&& offsetof(Binding, emissiveTextureIndex) == 12u
				&& offsetof(Binding, baseColorFactor) == 16u
				&& offsetof(Binding, emissiveFactor) == 32u
				&& offsetof(Binding, normalScale) == 48u
				&& offsetof(Binding, occlusionStrength) == 52u
				&& offsetof(Binding, roughnessFactor) == 56u
				&& offsetof(Binding, metallicFactor) == 60u
				&& offsetof(Binding, tileSizeMetres) == 64u
				&& offsetof(Binding, inverseRotationCosSin) == 72u
				&& offsetof(Binding, triplanarSharpness) == 80u
				&& offsetof(Binding, mappingMode) == 84u
				&& offsetof(Binding, stableLayerId) == 88u;
		}

		struct ReferenceTerrainSurface
		{
			std::array<float, 3u> baseColor{};
			std::array<float, 3u> normal{};
			float ambientOcclusion = 1.0F;
			float roughness = 1.0F;
			float metallic = 0.0F;
			std::array<float, 3u> emissive{};
		};

		ReferenceTerrainSurface BlendReferenceTerrainSurface(
			const std::array<ReferenceTerrainSurface, 4u>& contributors,
			const std::array<float, 4u>& weights)
		{
			ReferenceTerrainSurface blended{};
			blended.ambientOcclusion = 0.0F;
			blended.roughness = 0.0F;
			float weightSum = 0.0F;
			for(std::size_t contributor = 0u; contributor < contributors.size(); ++contributor)
			{
				const float weight = std::max(weights[contributor], 0.0F);
				weightSum += weight;
				for(std::size_t channel = 0u; channel < 3u; ++channel)
				{
					blended.baseColor[channel] += contributors[contributor].baseColor[channel] * weight;
					blended.normal[channel] += contributors[contributor].normal[channel] * weight;
					blended.emissive[channel] += contributors[contributor].emissive[channel] * weight;
				}
				blended.ambientOcclusion += contributors[contributor].ambientOcclusion * weight;
				blended.roughness += contributors[contributor].roughness * weight;
				blended.metallic += contributors[contributor].metallic * weight;
			}
			if(weightSum <= 0.0F) return {};
			for(std::size_t channel = 0u; channel < 3u; ++channel)
			{
				blended.baseColor[channel] /= weightSum;
				blended.normal[channel] /= weightSum;
				blended.emissive[channel] /= weightSum;
			}
			blended.ambientOcclusion /= weightSum;
			blended.roughness /= weightSum;
			blended.metallic /= weightSum;
			const float normalLength = std::sqrt(
				blended.normal[0] * blended.normal[0]
				+ blended.normal[1] * blended.normal[1]
				+ blended.normal[2] * blended.normal[2]);
			if(normalLength > 0.0F)
				for(float& channel : blended.normal) channel /= normalLength;
			return blended;
		}

		bool Near(float left, float right, float tolerance = 1.0e-5F) noexcept
		{
			return std::abs(left - right) <= tolerance;
		}
	}

	void RegisterTerrainSurfaceRenderingTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Surface.ComponentPersistsSanitizedPbrInputs", [](TestContext& context) {
			TerrainComponent terrain;
			terrain.baseColor = {-1.0F, 0.4F, 2.0F};
			terrain.metallic = 1.5F;
			terrain.roughness = -0.25F;
			const auto settings = terrain.MaterialSettings();
			context.Expect(TerrainComponent::kSchemaVersion == 2U
				&& settings.baseColor == glm::vec3(0.0F, 0.4F, 1.0F)
				&& settings.metallic == 1.0F && settings.roughness == 0.0F,
				"Terrain scene material inputs must persist under schema v2 and clamp to valid metallic-roughness ranges.");
		});

		registry.Add("Terrain.Surface.DirectMaterialRetainsAuthoredPbrFactors", [](TestContext& context) {
			const TerraEngine::Terrain::TerrainMaterialSettings settings{
				glm::vec3(0.24F, 0.56F, 0.18F), 0.35F, 0.67F};
			const std::unique_ptr<Material> generated = TerraEngine::Terrain::CreateTerrainMaterial(
				ResourceManager::Instance(), settings, nullptr, nullptr);
			context.Expect(generated
				&& Near(generated->header.baseColorFactor[0], settings.baseColor.r)
				&& Near(generated->header.baseColorFactor[1], settings.baseColor.g)
				&& Near(generated->header.baseColorFactor[2], settings.baseColor.b)
				&& Near(generated->header.baseColorFactor[3], 1.0F)
				&& Near(generated->header.metallicFactor, settings.metallic)
				&& Near(generated->header.roughnessFactor, settings.roughness),
				"A terrain with no layer source must copy its sanitized authored base color, metallic, and roughness into the generated material header.");
		});

		registry.Add("Terrain.Surface.GeneratedProxyCopiesFirstLayerTextureAndMapping", [](TestContext& context) {
			Material source{};
			TextureBinding binding{};
			binding.semantic = TextureSemantic::Base;
			binding.textureUuid = TerraEngine::GenerateUUID();
			binding.uvScale[0] = 2.0F;
			binding.uvScale[1] = 3.0F;
			source.storage.resize(sizeof(binding));
			std::memcpy(source.storage.data(), &binding, sizeof(binding));
			source.textures = std::span<const TextureBinding>(
				reinterpret_cast<const TextureBinding*>(source.storage.data()), 1u);
			TerraEngine::Terrain::TerrainLayerMapping mapping{};
			mapping.tileSizeXMetres = 4.0;
			mapping.tileSizeZMetres = 5.0;
			mapping.offsetXMetres = 0.25;
			mapping.offsetZMetres = 0.5;
			auto generated = TerraEngine::Terrain::CreateTerrainMaterial(
				ResourceManager::Instance(), {}, &source, &mapping);
			context.Expect(generated && generated->textures.size() == 1u
				&& generated->textures[0].textureUuid == binding.textureUuid
				&& Near(generated->header.baseColorFactor[0], 1.0F)
				&& Near(generated->header.baseColorFactor[1], 1.0F)
				&& Near(generated->header.baseColorFactor[2], 1.0F)
				&& Near(generated->header.metallicFactor, 1.0F)
				&& Near(generated->header.roughnessFactor, 1.0F)
				&& generated->textures[0].uvScale[0] == 0.5F
				&& generated->textures[0].uvScale[1] == 0.6F
				&& generated->textures[0].uvOffset[0] == 0.0625F
				&& generated->textures[0].uvOffset[1] == 0.1F,
				"The generated terrain proxy must convert metre tile size and offset to physical texture coordinates.");
		});

		registry.Add("Terrain.Surface.RuntimeBindingPublishesAtomically", [](TestContext& context) {
			auto& runtimeRegistry = TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance();
			runtimeRegistry.Clear();
			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeBinding incomplete{};
			incomplete.materialId = TerraEngine::GenerateUUID();
			context.Expect(!runtimeRegistry.Publish(incomplete) && !runtimeRegistry.Resolve(incomplete.materialId),
				"An incomplete terrain control binding must be rejected without leaving partial renderer state.");

			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeBinding complete{};
			complete.materialId = TerraEngine::GenerateUUID();
			complete.weightTextureId = TerraEngine::GenerateUUID();
			complete.layerIdTextureId = TerraEngine::GenerateUUID();
			complete.layerCount = 1u;
			complete.layers[0].stableLayerId = 7u;
			context.Expect(runtimeRegistry.Publish(complete),
				"A complete terrain control binding must publish as one protected value.");
			const auto resolved = runtimeRegistry.Resolve(complete.materialId);
			context.Expect(resolved && resolved->publicationGeneration > 0u
				&& resolved->layers[0].stableLayerId == 7u,
				"The renderer must resolve one immutable generation containing controls and stable layer metadata together.");
			runtimeRegistry.Remove(complete.materialId);
			context.Expect(!runtimeRegistry.Resolve(complete.materialId),
				"Releasing a terrain proxy must also remove its renderer binding.");
		});

		registry.Add("Terrain.Surface.CacheKeepsProtectedControlAndRejectsStalePublication", [](TestContext& context) {
			const auto asset = TerraEngine::GenerateUUID();
			TerraEngine::Renderer::Terrain::TerrainSurfaceCacheRequest pinned{};
			pinned.key = {asset, {0, 0}, {1u, 2u}, 4u}; pinned.byteCount = 80u; pinned.priority = 10u; pinned.protectedFromEviction = true;
			TerraEngine::Renderer::Terrain::TerrainSurfaceCacheRequest ordinary{};
			ordinary.key = {asset, {1, 0}, {3u, 4u}, 4u}; ordinary.byteCount = 80u;
			const auto plan = TerraEngine::Renderer::Terrain::PrepareTerrainSurfaceCachePlan(4u, {}, {pinned, ordinary}, 100u);
			std::vector<TerraEngine::Renderer::Terrain::TerrainSurfacePageHandle> visible;
			std::uint64_t generation = 0u;
			context.Expect(plan.candidate.size() == 1u && plan.candidate.front().key.area == pinned.key.area,
				"Control cache pressure must retain a protected page and evict ordinary material detail.");
			context.Expect(!TerraEngine::Renderer::Terrain::PublishTerrainSurfaceCachePlan(visible, generation, plan, 5u)
				&& TerraEngine::Renderer::Terrain::PublishTerrainSurfaceCachePlan(visible, generation, plan, 4u),
				"A stale control-page candidate must not replace last-known-good visible surface state.");
			TerraEngine::Renderer::Terrain::TerrainSurfaceCacheRequest replacement = pinned;
			replacement.key.sourceHash = {9u, 9u};
			const auto lkgPlan = TerraEngine::Renderer::Terrain::PrepareTerrainSurfaceCachePlan(5u, visible, {replacement}, 80u);
			context.Expect(lkgPlan.candidate.size() == 1u && lkgPlan.candidate.front().lastKnownGood
				&& lkgPlan.candidate.front().key.sourceHash == pinned.key.sourceHash,
				"An over-budget replacement must retain the prior exact control page as last-known-good coverage.");
		});

		registry.Add("Terrain.Surface.DerivedBatchRequiresGeometryHolesAndControlsTogether", [](TestContext& context) {
			auto root = std::make_shared<Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 2u; root->rootContentHash = {8u, 9u}; root->authoredBounds = {0, 0, 1, 1};
			Terrain::TerrainAssetStore store(root);
			Terrain::TerrainDerivedSurfaceBatch batch{};
			batch.source = {root->assetUuid, root->rootContentHash, root->publicationGeneration, true, nullptr};
			batch.derivedGeneration = 1u; batch.geometryIdentity = {1u, 1u}; batch.holeIdentity = {2u, 2u}; batch.surfaceControlIdentity = {3u, 3u};
			batch.geometryComplete = batch.holesComplete = batch.surfaceControlsComplete = true;
			const auto accepted = Terrain::ValidateTerrainDerivedSurfaceBatch(batch, store);
			batch.surfaceControlsComplete = false;
			const auto rejected = Terrain::ValidateTerrainDerivedSurfaceBatch(batch, store);
			context.Expect(accepted.Succeeded() && !rejected.Succeeded(),
				"Geometry, hole coverage, and material controls must pass one generation-checked publication barrier.");
		});

		registry.Add("Terrain.Surface.FourContributorsBlendEveryPbrSemantic", [](TestContext& context) {
			const std::array<ReferenceTerrainSurface, 4u> contributors{{
				{{1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 0.2F, 0.1F, 0.0F, {1.0F, 0.0F, 0.0F}},
				{{0.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 0.4F, 0.3F, 0.25F, {0.0F, 1.0F, 0.0F}},
				{{0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F}, 0.6F, 0.5F, 0.5F, {0.0F, 0.0F, 1.0F}},
				{{1.0F, 1.0F, 1.0F}, {0.0F, 1.0F, 0.0F}, 0.8F, 0.7F, 1.0F, {1.0F, 1.0F, 1.0F}}
			}};
			const ReferenceTerrainSurface blended = BlendReferenceTerrainSurface(
				contributors, {0.1F, 0.2F, 0.3F, 0.4F});
			context.Expect(Near(blended.baseColor[0], 0.5F)
				&& Near(blended.baseColor[1], 0.6F) && Near(blended.baseColor[2], 0.7F)
				&& Near(blended.ambientOcclusion, 0.6F) && Near(blended.roughness, 0.5F)
				&& Near(blended.metallic, 0.6F)
				&& Near(blended.emissive[0], 0.5F) && Near(blended.emissive[1], 0.6F)
				&& Near(blended.emissive[2], 0.7F) && Near(blended.normal[0], 0.14744195F)
				&& Near(blended.normal[1], 0.88465172F) && Near(blended.normal[2], 0.44232586F),
				"All four contributors must use one normalized control weight for Base Color, decoded Normal, AO, roughness, metallic, and Emissive.");

			const std::string shader = ReadTerrainRenderingSource(
				"Engine/Shaders/Terrain/TerrainMaterial.hlsli");
			context.Expect(shader.find("BlendTerrainContributors") != std::string::npos
				&& shader.find("contributor < 4u") != std::string::npos
				&& ContainsTokensInOrder(shader, {"baseColor", "normal", "ambientOcclusion", "roughness", "metallic", "emissive"})
				&& shader.find("accumulatedWeight") != std::string::npos
				&& shader.find("normalize") != std::string::npos,
				"The shared terrain shader include must blend every PBR semantic across four stable-ID contributors and normalize the final canonical-space normal.");
		});

		registry.Add("Terrain.Surface.LayerFactorsAreAppliedExactlyOnce", [](TestContext& context) {
			const std::string shader = ReadTerrainRenderingSource(
				"Engine/Shaders/Terrain/TerrainMaterial.hlsli");
			const std::string visibility = ReadTerrainRenderingSource(
				"Engine/Shaders/Terrain/TerrainVisibility.frag.hlsl");
			const std::string compute = ReadTerrainRenderingSource(
				"Engine/Shaders/Terrain/TerrainSurface.comp.hlsl");
			context.Expect(shader.find("ApplyTerrainLayerFactors") != std::string::npos
				&& shader.find("baseColorFactor") != std::string::npos
				&& shader.find("emissiveFactor") != std::string::npos
				&& shader.find("normalScale") != std::string::npos
				&& shader.find("occlusionStrength") != std::string::npos
				&& shader.find("roughnessFactor") != std::string::npos
				&& shader.find("metallicFactor") != std::string::npos,
				"One shared helper must apply each authored terrain-layer factor to its sampled semantic exactly once.");
			context.Expect(visibility.find("ApplyTerrainLayerFactors") == std::string::npos
				&& compute.find("ApplyTerrainLayerFactors") == std::string::npos,
				"Direct and page-backed terrain routes must consume the shared factor result without multiplying proxy or component factors again.");
		});

		registry.Add("Terrain.Surface.MissingOptionalMapsUseNeutralPbrValues", [](TestContext& context) {
			const std::string shader = ReadTerrainRenderingSource(
				"Engine/Shaders/Terrain/TerrainMaterial.hlsli");
			context.Expect(shader.find("TerrainNeutralBaseColor") != std::string::npos
				&& shader.find("float4(1.0f, 1.0f, 1.0f, 1.0f)") != std::string::npos
				&& shader.find("TerrainNeutralNormal") != std::string::npos
				&& shader.find("float3(0.5f, 0.5f, 1.0f)") != std::string::npos
				&& shader.find("TerrainNeutralOrm") != std::string::npos
				&& shader.find("float3(1.0f, 1.0f, 0.0f)") != std::string::npos
				&& shader.find("TerrainNeutralEmissive") != std::string::npos
				&& shader.find("float3(0.0f, 0.0f, 0.0f)") != std::string::npos,
				"Missing optional terrain maps must sample white Base, flat Normal, AO=1/roughness=1/metallic=0 ORM, and black Emissive rather than diagnostics or layer zero.");
		});

		registry.Add("Terrain.Surface.CpuAndHlslLayerLayoutsMatchExactly", [](TestContext& context) {
			using TerrainGpuLayerBinding = TerraEngine::Renderer::Terrain::TerrainGpuLayerBinding;
			context.Expect(MatchesTerrainGpuLayerAbi<TerrainGpuLayerBinding>(),
				"TerrainGpuLayerBinding must be a 16-byte-aligned 96-byte standard-layout record with frozen offsets for all four textures, factors, mapping, mode, and stable ID.");
			const std::string shader = ReadTerrainRenderingSource(
				"Engine/Shaders/Terrain/TerrainMaterial.hlsli");
			context.Expect(ContainsTokensInOrder(shader, {
				"struct TerrainGpuLayerBinding", "baseColorTextureIndex", "normalTextureIndex",
				"ormTextureIndex", "emissiveTextureIndex", "baseColorFactor", "emissiveFactor",
				"normalScale", "occlusionStrength", "roughnessFactor", "metallicFactor",
				"tileSizeMetres", "inverseRotationCosSin", "triplanarSharpness",
				"mappingMode", "stableLayerId"}),
				"The HLSL TerrainGpuLayerBinding fields must appear in the exact frozen CPU ABI order.");
		});
	}
}
