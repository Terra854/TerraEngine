// MaterialRuntimeContract.cpp - TMAT runtime slot resolver

#include "MaterialRuntimeContract.h"

#include <algorithm>

namespace {
	void HashMaterialCapabilityField(std::uint64_t& seed, const std::uint64_t value) noexcept
	{
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
	}

	bool HasMaterialMagic(const MaterialHeader& header) noexcept
	{
		return header.magic[0] == 'T' && header.magic[1] == 'M'
			&& header.magic[2] == 'A' && header.magic[3] == 'T';
	}

	const TextureBinding* FindTexture(std::span<const TextureBinding> textures, TextureSemantic semantic)
	{
		const auto it = std::find_if(textures.begin(), textures.end(), [semantic](const TextureBinding& binding) {
			return binding.semantic == semantic;
		});
		return it == textures.end() ? nullptr : &(*it);
	}

	void SetDefaults(ResolvedMaterialSlot& slot, const MaterialHeader& header)
	{
		// Defaults are authored material constants, not arbitrary renderer
		// values. This keeps editor preview, export validation, and runtime
		// binding aligned when a texture slot is intentionally left empty.
		switch (slot.slot)
		{
		case MaterialRuntimeSlot::Albedo:
			slot.defaultValue[0] = header.baseColorFactor[0];
			slot.defaultValue[1] = header.baseColorFactor[1];
			slot.defaultValue[2] = header.baseColorFactor[2];
			slot.defaultValue[3] = header.baseColorFactor[3];
			slot.fallbackDescription = "baseColorFactor";
			return;
		case MaterialRuntimeSlot::Normal:
			slot.defaultValue[0] = 0.5f;
			slot.defaultValue[1] = 0.5f;
			slot.defaultValue[2] = 1.0f;
			slot.defaultValue[3] = 1.0f;
			slot.fallbackDescription = "flat normal";
			return;
		case MaterialRuntimeSlot::Roughness:
			slot.defaultValue[0] = header.roughnessFactor;
			slot.defaultValue[1] = header.roughnessFactor;
			slot.defaultValue[2] = header.roughnessFactor;
			slot.defaultValue[3] = 1.0f;
			slot.fallbackDescription = "roughnessFactor";
			return;
		case MaterialRuntimeSlot::Metallic:
			slot.defaultValue[0] = header.metallicFactor;
			slot.defaultValue[1] = header.metallicFactor;
			slot.defaultValue[2] = header.metallicFactor;
			slot.defaultValue[3] = 1.0f;
			slot.fallbackDescription = "metallicFactor";
			return;
		case MaterialRuntimeSlot::Emissive:
			slot.defaultValue[0] = header.emissiveFactor[0];
			slot.defaultValue[1] = header.emissiveFactor[1];
			slot.defaultValue[2] = header.emissiveFactor[2];
			slot.defaultValue[3] = 1.0f;
			slot.fallbackDescription = "emissiveFactor";
			return;
		}
	}

	ResolvedMaterialSlot ResolveSlot(const MaterialHeader& header,
		std::span<const TextureBinding> textures,
		MaterialRuntimeSlot slot,
		TextureSemantic primarySemantic,
		TextureSemantic packedSemantic,
		bool allowPacked)
	{
		ResolvedMaterialSlot resolved;
		resolved.slot = slot;
		SetDefaults(resolved, header);

		const TextureBinding* binding = FindTexture(textures, primarySemantic);
		if (binding == nullptr && allowPacked)
		{
			// Roughness and metallic share the packed MetallicRoughness semantic
			// in TMAT v1, but the runtime still exposes two separate slots.
			binding = FindTexture(textures, packedSemantic);
		}

		if (binding != nullptr && binding->textureUuid != TerraEngine::EMPTY_UUID)
		{
			resolved.hasTexture = true;
			resolved.textureUuid = binding->textureUuid;
			resolved.fallbackDescription.clear();
		}

		return resolved;
	}
}

MaterialRuntimeContract ResolveMaterialRuntimeContract(const MaterialHeader& header, std::span<const TextureBinding> textures)
{
	MaterialRuntimeContract contract;
	contract.compatibilitySignature = 1469598103934665603ull;
	contract.slots.push_back(ResolveSlot(header, textures, MaterialRuntimeSlot::Albedo, TextureSemantic::Base, TextureSemantic::Base, false));
	contract.slots.push_back(ResolveSlot(header, textures, MaterialRuntimeSlot::Normal, TextureSemantic::Normal, TextureSemantic::Normal, false));
	contract.slots.push_back(ResolveSlot(header, textures, MaterialRuntimeSlot::Roughness, TextureSemantic::MetallicRoughness, TextureSemantic::MetallicRoughness, true));
	contract.slots.push_back(ResolveSlot(header, textures, MaterialRuntimeSlot::Metallic, TextureSemantic::MetallicRoughness, TextureSemantic::MetallicRoughness, true));
	contract.slots.push_back(ResolveSlot(header, textures, MaterialRuntimeSlot::Emissive, TextureSemantic::Emissive, TextureSemantic::Emissive, false));

	for (const ResolvedMaterialSlot& slot : contract.slots)
	{
		if (!slot.hasTexture)
		{
			contract.warnings.push_back(std::string(ToString(slot.slot)) + " uses fallback " + slot.fallbackDescription);
		}
	}

	return contract;
}

const ResolvedMaterialSlot* FindResolvedMaterialSlot(const Material& material, MaterialRuntimeSlot slot)
{
	const auto it = std::find_if(material.resolvedSlots.begin(), material.resolvedSlots.end(), [slot](const ResolvedMaterialSlot& resolved) {
		return resolved.slot == slot;
	});
	return it == material.resolvedSlots.end() ? nullptr : &(*it);
}

const char* ToString(MaterialRuntimeSlot slot)
{
	switch (slot)
	{
	case MaterialRuntimeSlot::Albedo: return "albedo";
	case MaterialRuntimeSlot::Normal: return "normal";
	case MaterialRuntimeSlot::Roughness: return "roughness";
	case MaterialRuntimeSlot::Metallic: return "metallic";
	case MaterialRuntimeSlot::Emissive: return "emissive";
	default: return "unknown";
	}
}

MaterialVisibilityCompatibility EvaluateMaterialVisibilityCompatibility(const MaterialHeader& header)
{
	MaterialVisibilityCompatibility compatibility{};
	if(header.alphaMode == static_cast<std::uint32_t>(AlphaMode::Blend))
	{
		compatibility.reason = "Blended material requires ordered forward/indexed fallback.";
		return compatibility;
	}
	compatibility.supportsVisibilityBuffer = true;
	compatibility.requiresForwardFallback = false;
	compatibility.reason = header.alphaMode == static_cast<std::uint32_t>(AlphaMode::Mask)
		? "Alpha-tested material keeps its stable identity after the opacity test."
		: "Opaque material is compatible with stable visibility identity.";
	return compatibility;
}

MaterialCapabilitySignature BuildMaterialCapabilitySignature(const MaterialHeader& header)
{
	MaterialCapabilitySignature result{};
	const MaterialVisibilityCompatibility visibility =
		EvaluateMaterialVisibilityCompatibility(header);
	result.requiredFeatureMask = visibility.supportsVisibilityBuffer
		? ToVirtualGeometryMaterialFeatureMask(
			VirtualGeometryMaterialFeature::VisibilityBuffer)
		: 0u;
	if(header.alphaMode == static_cast<std::uint32_t>(AlphaMode::Mask))
	{
		result.requiredFeatureMask |= ToVirtualGeometryMaterialFeatureMask(
			VirtualGeometryMaterialFeature::AlphaTest);
	}
	// A header-only material has one non-programmable bin. Optional TMVG data
	// below can request more bins, but never widens this conservative default.
	result.maximumShadingBins = 1u;
	result.shadingBin = 0u;
	result.compatibilitySignature = 1469598103934665603ull;
	HashMaterialCapabilityField(result.compatibilitySignature, header.version);
	HashMaterialCapabilityField(result.compatibilitySignature, header.shadingModel);
	HashMaterialCapabilityField(result.compatibilitySignature, header.alphaMode);
	HashMaterialCapabilityField(result.compatibilitySignature,
		result.requiredFeatureMask);
	result.valid = HasMaterialMagic(header)
		&& header.headerSize == sizeof(MaterialHeader)
		&& visibility.supportsVisibilityBuffer;
	return result;
}

MaterialCapabilitySignature BuildMaterialCapabilitySignature(const Material& material)
{
	MaterialCapabilitySignature result = BuildMaterialCapabilitySignature(material.header);
	if(material.virtualGeometryExtension)
	{
		const auto& extension = *material.virtualGeometryExtension;
		result.requiredFeatureMask = extension.requiredFeatureMask;
		result.programmableFeatureMask = extension.programmableFeatureMask;
		result.shadingBin = extension.shadingBin;
		result.maximumShadingBins = extension.maximumShadingBins;
		result.compatibilitySignature = extension.compatibilitySignature;
		result.valid = result.valid
			&& IsValidVirtualGeometryMaterialExtension(extension)
			&& result.maximumShadingBins > 0u
			&& result.shadingBin < result.maximumShadingBins
			&& result.compatibilitySignature != 0u;
	}
	return result;
}

bool ValidateMaterialVisibilityAbi(const MaterialHeader& header, std::uint32_t visibilityAbiVersion)
{
	return visibilityAbiVersion != 0u;
}
