// TerrainMaterial.h -- Shared authored settings for generated terrain materials.

#pragma once

#include "Core/Export.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Utilities/UUID.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <memory>

struct Material;
class ResourceManager;

namespace TerraEngine::Terrain
{
	struct TerrainLayerMapping;

	inline constexpr std::array<float, 4u> kTerrainNeutralBaseColor{1.0F, 1.0F, 1.0F, 1.0F};
	inline constexpr std::array<float, 3u> kTerrainNeutralNormal{0.5F, 0.5F, 1.0F};
	inline constexpr std::array<float, 3u> kTerrainNeutralOrm{1.0F, 1.0F, 0.0F};
	inline constexpr std::array<float, 3u> kTerrainNeutralEmissive{0.0F, 0.0F, 0.0F};

	// A sampler request remains attached to its source UUID until the Vulkan
	// backend resolves a combined image-sampler alias. Numeric anisotropy is a
	// render-quality decision, so authored data stores intent rather than a level.
	struct TerrainSurfaceSamplerRequest
	{
		SamplerFilter filter = SamplerFilter::Trilinear;
		SamplerAddressMode addressU = SamplerAddressMode::Repeat;
		SamplerAddressMode addressV = SamplerAddressMode::Repeat;
		SamplerAddressMode addressW = SamplerAddressMode::Repeat;

		[[nodiscard]] constexpr bool operator==(const TerrainSurfaceSamplerRequest&) const noexcept = default;
	};

	// Immutable normalized source inputs for one enabled terrain layer. Separate
	// source UUIDs preserve streaming identity while the generated ORM source
	// collapses AO/roughness/metallic into one deterministic sampled semantic.
	struct TerrainLayerSurfaceSource
	{
		TerraEngine::UUID baseColorTextureId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID normalTextureId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID ormTextureId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID emissiveTextureId = TerraEngine::EMPTY_UUID;
		std::array<TerrainSurfaceSamplerRequest, 4u> samplers{};
		std::array<float, 4u> baseColorFactor = kTerrainNeutralBaseColor;
		std::array<float, 3u> emissiveFactor{1.0F, 1.0F, 1.0F};
		float normalScale = 1.0F;
		float occlusionStrength = 1.0F;
		float roughnessFactor = 1.0F;
		float metallicFactor = 1.0F;
	};

	struct TerrainGeneratedOrmDerivationKey
	{
		TerraEngine::UUID metallicRoughnessTextureId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID occlusionTextureId = TerraEngine::EMPTY_UUID;
		std::uint64_t metallicRoughnessGeneration = 0u;
		std::uint64_t occlusionGeneration = 0u;
		std::uint32_t channelPolicyVersion = 1u;

		[[nodiscard]] constexpr bool operator==(const TerrainGeneratedOrmDerivationKey&) const noexcept = default;
	};

	// CPU reference value used by material preparation and shader-parity tests.
	// The input normal is unpacked tangent space (+Z away from the surface).
	struct TerrainSurfaceFactorSample
	{
		std::array<float, 4u> baseColor = kTerrainNeutralBaseColor;
		std::array<float, 3u> normal{0.0F, 0.0F, 1.0F};
		float ambientOcclusion = 1.0F;
		float roughness = 1.0F;
		float metallic = 0.0F;
		std::array<float, 3u> emissive = kTerrainNeutralEmissive;
	};

	[[nodiscard]] TERRAENGINE_API TerrainSurfaceFactorSample ApplyTerrainLayerFactors(
		TerrainSurfaceFactorSample sample, const TerrainLayerSurfaceSource& layer) noexcept;
	[[nodiscard]] TERRAENGINE_API TerrainLayerSurfaceSource ResolveTerrainLayerSurfaceSource(
		const Material& material,
		TerraEngine::UUID generatedOrmTextureId = TerraEngine::EMPTY_UUID) noexcept;

	// Workers prepare this value from immutable root metadata. phaseMetres is the
	// bounded value uploaded to draw data; areaOriginMetres remains double
	// precision so validation and CPU tools can evaluate the exact physical map.
	struct TerrainAreaMaterialPhase
	{
		TerrainAreaCoordinate area{};
		glm::dvec2 phaseMetres{};
		glm::dvec2 areaOriginMetres{};
		double sampleSpacingMetres = 1.0;
	};

	[[nodiscard]] TERRAENGINE_API TerrainCodecResult PrepareTerrainAreaMaterialPhase(
		TerrainAreaCoordinate area, double sampleSpacingMetres,
		const TerrainLayerMapping& mapping, TerrainAreaMaterialPhase& outPhase) noexcept;
	[[nodiscard]] TERRAENGINE_API glm::dvec2 EvaluateTerrainSurfaceUv(
		const TerrainAreaMaterialPhase& phase, const TerrainLayerMapping& mapping,
		glm::dvec2 localSample) noexcept;
	[[nodiscard]] TERRAENGINE_API glm::dvec2 ComputeTerrainControlUv(glm::dvec2 localSample) noexcept;
	// Terrain keeps a compact set of physically based inputs in the scene. The
	// generated editor and runtime materials both consume this value object, so an
	// exported terrain cannot silently fall back to different lighting values.
	struct TerrainMaterialSettings
	{
		glm::vec3 baseColor{0.18F, 0.55F, 0.16F};
		float metallic = 0.0F;
		float roughness = 0.92F;

		[[nodiscard]] bool operator==(const TerrainMaterialSettings&) const = default;
	};

	// Invalid scene input is clamped before it reaches renderer-owned resources.
	// This makes old or manually edited scene files safe to load.
	[[nodiscard]] TERRAENGINE_API TerrainMaterialSettings SanitizeTerrainMaterialSettings(
		TerrainMaterialSettings settings) noexcept;

	// Builds the ordinary TMAT-backed PBR representation used by both editor and
	// packaged terrain proxies. Resource registration remains with the caller.
	[[nodiscard]] TERRAENGINE_API std::unique_ptr<Material> CreateTerrainMaterial(
		ResourceManager& resources, TerrainMaterialSettings settings,
		const Material* surfaceSource = nullptr,
		const TerrainLayerMapping* mapping = nullptr);
}
