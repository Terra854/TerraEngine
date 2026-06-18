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
};

MaterialRuntimeContract ResolveMaterialRuntimeContract(const MaterialHeader& header, std::span<const TextureBinding> textures);
const ResolvedMaterialSlot* FindResolvedMaterialSlot(const Material& material, MaterialRuntimeSlot slot);
const char* ToString(MaterialRuntimeSlot slot);
