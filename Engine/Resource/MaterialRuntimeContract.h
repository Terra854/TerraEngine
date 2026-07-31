// MaterialRuntimeContract.h - TMAT runtime slot resolver
//
// The renderer and editor preview both need the same material answers: which
// texture, scalar default, or fallback should each runtime slot use. Centralizing
// this here keeps missing-texture behavior consistent across preview, export
// validation, and packaged runtime rendering.

#pragma once

#include "File Formats/TerraEngineMaterialFile.h"

#include <string>
#include <vector>

struct MaterialRuntimeContract
{
	std::vector<ResolvedMaterialSlot> slots;
	std::vector<std::string> warnings;
	std::uint64_t compatibilitySignature = 0u;
	bool virtualGeometryCompatible = false;
};

struct MaterialCapabilitySignature
{
	std::uint32_t requiredFeatureMask = 0u;
	std::uint32_t programmableFeatureMask = 0u;
	std::uint32_t shadingBin = 0u;
	std::uint32_t maximumShadingBins = 0u;
	std::uint64_t compatibilitySignature = 0u;
	bool valid = false;
};

// Visibility buffers can represent opaque and alpha-tested surfaces without
// changing their stable primitive identity. Blended materials remain on the
// diagnosed forward/indexed path because order-dependent transparency cannot
// be reconstructed from one depth-winning visibility record.
struct MaterialVisibilityCompatibility
{
	bool supportsVisibilityBuffer = false;
	bool requiresForwardFallback = true;
	std::string reason;
};

MaterialRuntimeContract ResolveMaterialRuntimeContract(const MaterialHeader& header, std::span<const TextureBinding> textures);
const ResolvedMaterialSlot* FindResolvedMaterialSlot(const Material& material, MaterialRuntimeSlot slot);
const char* ToString(MaterialRuntimeSlot slot);
MaterialVisibilityCompatibility EvaluateMaterialVisibilityCompatibility(const MaterialHeader& header);
MaterialCapabilitySignature BuildMaterialCapabilitySignature(const MaterialHeader& header);
MaterialCapabilitySignature BuildMaterialCapabilitySignature(const Material& material);
bool ValidateMaterialVisibilityAbi(const MaterialHeader& header, std::uint32_t visibilityAbiVersion);
