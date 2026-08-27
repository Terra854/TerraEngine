// TerrainMaterial.cpp -- Generated metallic-roughness terrain material builder.

#include "Terrain/TerrainMaterial.h"

#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/ResourceManager.h"
#include "Terrain/TerrainLayerStack.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace TerraEngine::Terrain
{
	namespace
	{
		glm::dvec2 RotateInverse(glm::dvec2 value, double radians) noexcept
		{
			const double cosine = std::cos(radians);
			const double sine = std::sin(radians);
			return {cosine * value.x + sine * value.y, -sine * value.x + cosine * value.y};
		}

	double PositiveModulo(double value, double modulus) noexcept
		{
			const double reduced = std::fmod(value, modulus);
			return reduced < 0.0 ? reduced + modulus : reduced;
		}
	}

	TerrainSurfaceFactorSample ApplyTerrainLayerFactors(
		TerrainSurfaceFactorSample sample, const TerrainLayerSurfaceSource& layer) noexcept
	{
		// Factors are consumed here exactly once. The generated proxy remains
		// neutral, so neither the direct nor page-backed shader route can apply a
		// second material-wide tint after contributor blending.
		for(std::size_t channel = 0u; channel < sample.baseColor.size(); ++channel)
			sample.baseColor[channel] = std::isfinite(sample.baseColor[channel])
				&& std::isfinite(layer.baseColorFactor[channel])
				? sample.baseColor[channel] * layer.baseColorFactor[channel]
				: kTerrainNeutralBaseColor[channel];
		for(std::size_t channel = 0u; channel < sample.emissive.size(); ++channel)
			sample.emissive[channel] = std::isfinite(sample.emissive[channel])
				&& std::isfinite(layer.emissiveFactor[channel])
				? sample.emissive[channel] * layer.emissiveFactor[channel] : 0.0F;

		const float normalScale = std::isfinite(layer.normalScale)
			? std::max(layer.normalScale, 0.0F) : 1.0F;
		sample.normal[0] *= normalScale;
		sample.normal[1] *= normalScale;
		const float normalLength = std::sqrt(
			sample.normal[0] * sample.normal[0]
			+ sample.normal[1] * sample.normal[1]
			+ sample.normal[2] * sample.normal[2]);
		if(std::isfinite(normalLength) && normalLength > 1.0e-8F)
			for(float& channel : sample.normal) channel /= normalLength;
		else sample.normal = {0.0F, 0.0F, 1.0F};

		const float occlusionStrength = std::isfinite(layer.occlusionStrength)
			? std::clamp(layer.occlusionStrength, 0.0F, 1.0F) : 1.0F;
		sample.ambientOcclusion = std::clamp(
			1.0F + (sample.ambientOcclusion - 1.0F) * occlusionStrength, 0.0F, 1.0F);
		sample.roughness = std::clamp(sample.roughness
			* (std::isfinite(layer.roughnessFactor) ? layer.roughnessFactor : 1.0F), 0.0F, 1.0F);
		sample.metallic = std::clamp(sample.metallic
			* (std::isfinite(layer.metallicFactor) ? layer.metallicFactor : 1.0F), 0.0F, 1.0F);
		return sample;
	}

	TerrainLayerSurfaceSource ResolveTerrainLayerSurfaceSource(
		const Material& material, TerraEngine::UUID generatedOrmTextureId) noexcept
	{
		TerrainLayerSurfaceSource source{};
		source.baseColorFactor = {
			material.header.baseColorFactor[0], material.header.baseColorFactor[1],
			material.header.baseColorFactor[2], material.header.baseColorFactor[3]};
		source.emissiveFactor = {
			material.header.emissiveFactor[0], material.header.emissiveFactor[1],
			material.header.emissiveFactor[2]};
		source.normalScale = material.header.normalScale;
		source.occlusionStrength = material.header.occlusionStrength;
		source.roughnessFactor = material.header.roughnessFactor;
		source.metallicFactor = material.header.metallicFactor;
		for(const TextureBinding& texture : material.textures)
		{
			TerrainSurfaceSamplerRequest sampler{};
			sampler.filter = texture.samplerFilter;
			sampler.addressU = texture.addressU;
			sampler.addressV = texture.addressV;
			sampler.addressW = texture.addressW;
			switch(texture.semantic)
			{
			case TextureSemantic::Base:
				source.baseColorTextureId = texture.textureUuid;
				source.samplers[0] = sampler;
				break;
			case TextureSemantic::Normal:
				source.normalTextureId = texture.textureUuid;
				source.samplers[1] = sampler;
				break;
			case TextureSemantic::MetallicRoughness:
				if(generatedOrmTextureId == TerraEngine::EMPTY_UUID)
					source.ormTextureId = texture.textureUuid;
				source.samplers[2] = sampler;
				break;
			case TextureSemantic::Occlusion:
				// A generated packed artifact owns this semantic when supplied. Without
				// one, retain a usable single-source identity for compatibility.
				if(source.ormTextureId == TerraEngine::EMPTY_UUID)
					source.ormTextureId = texture.textureUuid;
				source.samplers[2] = sampler;
				break;
			case TextureSemantic::Emissive:
				source.emissiveTextureId = texture.textureUuid;
				source.samplers[3] = sampler;
				break;
			default:
				break;
			}
		}
		if(generatedOrmTextureId != TerraEngine::EMPTY_UUID)
			source.ormTextureId = generatedOrmTextureId;
		return source;
	}

	TerrainCodecResult PrepareTerrainAreaMaterialPhase(
		TerrainAreaCoordinate area, double sampleSpacingMetres,
		const TerrainLayerMapping& mapping, TerrainAreaMaterialPhase& outPhase) noexcept
	{
		outPhase = {};
		TerrainLayerMapping canonical{};
		if(const TerrainCodecResult validation = CanonicalizeTerrainLayerMapping(mapping, canonical);
			!validation.Succeeded()) return validation;
		if(!std::isfinite(sampleSpacingMetres) || sampleSpacingMetres <= 0.0)
			return {TerrainResultCode::InvalidArgument, "Terrain material phase requires finite positive sample spacing."};

		outPhase.area = area;
		outPhase.sampleSpacingMetres = sampleSpacingMetres;
		outPhase.areaOriginMetres = {
			static_cast<double>(area.x) * static_cast<double>(kTerrainAreaExtentSamples) * sampleSpacingMetres,
			static_cast<double>(area.z) * static_cast<double>(kTerrainAreaExtentSamples) * sampleSpacingMetres};
		const glm::dvec2 rotatedOrigin = RotateInverse(outPhase.areaOriginMetres, canonical.rotationRadians);
		outPhase.phaseMetres = {
			PositiveModulo(rotatedOrigin.x + canonical.offsetXMetres, canonical.tileSizeXMetres),
			PositiveModulo(rotatedOrigin.y + canonical.offsetZMetres, canonical.tileSizeZMetres)};
		return {};
	}

	glm::dvec2 EvaluateTerrainSurfaceUv(
		const TerrainAreaMaterialPhase& phase, const TerrainLayerMapping& mapping,
		glm::dvec2 localSample) noexcept
	{
		TerrainLayerMapping canonical{};
		if(!CanonicalizeTerrainLayerMapping(mapping, canonical).Succeeded()
			|| !std::isfinite(localSample.x) || !std::isfinite(localSample.y)) return {};
		// Reconstruct in canonical terrain-local metres. Both sides of an area edge
		// therefore evaluate the identical double before camera-relative conversion.
		const glm::dvec2 canonicalMetres = phase.areaOriginMetres + localSample * phase.sampleSpacingMetres;
		const glm::dvec2 rotated = RotateInverse(canonicalMetres, canonical.rotationRadians);
		return {
			(rotated.x + canonical.offsetXMetres) / canonical.tileSizeXMetres,
			(rotated.y + canonical.offsetZMetres) / canonical.tileSizeZMetres};
	}

	glm::dvec2 ComputeTerrainControlUv(glm::dvec2 localSample) noexcept
	{
		return localSample / static_cast<double>(kTerrainAreaExtentSamples);
	}

	TerrainMaterialSettings SanitizeTerrainMaterialSettings(
		TerrainMaterialSettings settings) noexcept
	{
		// A finite clamp prevents NaNs from propagating into descriptor buffers and
		// gives malformed legacy scene values the same predictable fallback as UI.
		for(float* channel : {&settings.baseColor.r, &settings.baseColor.g, &settings.baseColor.b})
			*channel = std::isfinite(*channel) ? std::clamp(*channel, 0.0F, 1.0F) : 0.0F;
		settings.metallic = std::isfinite(settings.metallic)
			? std::clamp(settings.metallic, 0.0F, 1.0F) : 0.0F;
		settings.roughness = std::isfinite(settings.roughness)
			? std::clamp(settings.roughness, 0.0F, 1.0F) : 0.92F;
		return settings;
	}

	std::unique_ptr<Material> CreateTerrainMaterial(
		ResourceManager& resources, TerrainMaterialSettings settings,
		const Material* surfaceSource,
		const TerrainLayerMapping* mapping)
	{
		settings = SanitizeTerrainMaterialSettings(settings);
		auto material = std::make_unique<Material>();
		std::memcpy(material->header.magic, "TMAT", 4U);
		material->header.version = kMaterialCurrentVersion;
		material->header.headerSize = static_cast<std::uint16_t>(sizeof(MaterialHeader));
		// Generated terrain vertices carry slope normals, so the shared PBR path
		// receives scene lights and shadows exactly like an ordinary opaque mesh.
		material->header.shadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
		material->header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
		material->header.flags = DoubleSided;
		material->header.shaderCollectionId = resources.GetDefaultShaderCollectionID();
		// Terrain layers own every authored factor. The proxy is routing state only,
		// and neutral values guarantee factors are never applied once in preparation
		// and again by the ordinary material path.
		material->header.baseColorFactor[0] = 1.0F;
		material->header.baseColorFactor[1] = 1.0F;
		material->header.baseColorFactor[2] = 1.0F;
		material->header.baseColorFactor[3] = 1.0F;
		material->header.emissiveFactor[0] = 0.0F;
		material->header.emissiveFactor[1] = 0.0F;
		material->header.emissiveFactor[2] = 0.0F;
		material->header.metallicFactor = 1.0F;
		material->header.roughnessFactor = 1.0F;
		material->header.normalScale = 1.0F;
		material->header.occlusionStrength = 1.0F;
		material->shaderCollectionId = material->header.shaderCollectionId;
		if(surfaceSource != nullptr && !surfaceSource->textures.empty())
		{
			// Terrain's first healthy layer is a normal TMAT source. Copy its texture
			// bindings into the generated proxy instead of retaining a pointer into the
			// resource cache, then apply the terrain layer's world-space tiling values.
			const std::size_t textureBytes = surfaceSource->textures.size_bytes();
			const std::size_t floatBytes = surfaceSource->floats.size_bytes();
			material->storage.resize(textureBytes + floatBytes);
			std::memcpy(material->storage.data(), surfaceSource->textures.data(), textureBytes);
			if(floatBytes != 0u)
				std::memcpy(material->storage.data() + textureBytes, surfaceSource->floats.data(), floatBytes);
			auto* copiedTextures = reinterpret_cast<TextureBinding*>(material->storage.data());
			if(mapping != nullptr)
			{
				TerrainLayerMapping canonical{};
				if(CanonicalizeTerrainLayerMapping(*mapping, canonical).Succeeded())
				{
					for(std::size_t index = 0u; index < surfaceSource->textures.size(); ++index)
					{
						// Convert the source material transform to physical terrain UV.
						// The shared mapping helper applies the same metre division to
						// direct and virtual-geometry terrain paths.
						copiedTextures[index].uvScale[0] /= static_cast<float>(canonical.tileSizeXMetres);
						copiedTextures[index].uvScale[1] /= static_cast<float>(canonical.tileSizeZMetres);
						copiedTextures[index].uvOffset[0] += static_cast<float>(canonical.offsetXMetres / canonical.tileSizeXMetres);
						copiedTextures[index].uvOffset[1] += static_cast<float>(canonical.offsetZMetres / canonical.tileSizeZMetres);
					}
				}
			}
			material->textures = std::span<const TextureBinding>(copiedTextures, surfaceSource->textures.size());
			material->floats = std::span<const FloatParameter>(
				reinterpret_cast<const FloatParameter*>(material->storage.data() + textureBytes),
				surfaceSource->floats.size());
			material->header.textureCount = static_cast<std::uint32_t>(material->textures.size());
			material->header.floatParameterCount = static_cast<std::uint32_t>(material->floats.size());
		}
		material->shadowBehavior = DeriveMaterialShadowBehavior(
			material->header.alphaMode, material->header.emissiveFactor);
		return material;
	}
}
