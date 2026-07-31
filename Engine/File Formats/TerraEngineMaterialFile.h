// TerraEngineMaterialFile.h — Binary format definition for material assets (TMAT).
//
// This file defines the on-disk and in-memory structures for the engine's custom
// material format. It includes definitions for shading models, alpha modes, and
// texture bindings, ensuring that material data is packed efficiently for
// fast loading and GPU binding.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "Utilities/UUID.h"

// Terra Material Format (TMAT) packs authored material settings so runtime code can bind
// PBR parameters and external textures (TETF) consistently alongside meshes (TEMF).

constexpr std::uint32_t kMaterialMagicValue = 0x54414d54u; // 'TMAT' little endian
constexpr std::uint16_t kMaterialCurrentVersion = 1;

// Limits keep the header compact and predictable on disk.
constexpr std::size_t kMaxTextureBindings = 8;
constexpr std::size_t kMaxFloatParameters = 16;

// ShadingModel names the supported states explicitly so serialization and switches stay readable.
enum class ShadingModel : std::uint32_t
{
	Unlit = 0,
	MetallicRoughness = 1,
	SpecularGlossiness = 2
};

// AlphaMode names the supported states explicitly so serialization and switches stay readable.
enum class AlphaMode : std::uint32_t
{
	Opaque = 0,
	Mask = 1,
	Blend = 2
};

// Runtime shadow behavior is derived from the material's opacity class for
// version-1 TMAT files. Keeping this as an explicit file-format enum gives
// importers and future TMAT versions a stable value set without invalidating
// existing material binaries.
enum class MaterialFileShadowBehavior : std::uint32_t
{
	Opaque = 0,
	AlphaCutout = 1,
	TransparentNoShadow = 2,
	TranslucentApproximate = 3,
	EmissiveUsesOpacity = 4
};

// TextureSemantic names the supported states explicitly so serialization and switches stay readable.
enum class TextureSemantic : std::uint32_t
{
	Base = 0,
	Normal = 1,
	MetallicRoughness = 2,
	Occlusion = 3,
	Emissive = 4,
	Height = 5
};

// MaterialRuntimeSlot is the product-facing material contract used by preview,
// export validation, and runtime binding. TMAT can store packed semantics such
// as MetallicRoughness, but the runtime still resolves them into these five
// predictable slots for diagnostics and fallbacks.
enum class MaterialRuntimeSlot : std::uint32_t
{
	Albedo = 0u,
	Normal = 1u,
	Roughness = 2u,
	Metallic = 3u,
	Emissive = 4u
};

// SamplerFilter names the supported states explicitly so serialization and switches stay readable.
enum class SamplerFilter : std::uint32_t
{
	Nearest = 0,
	Bilinear = 1,
	Trilinear = 2,
	Anisotropic = 3
};

// SamplerAddressMode names the supported states explicitly so serialization and switches stay readable.
enum class SamplerAddressMode : std::uint32_t
{
	Repeat = 0,
	MirroredRepeat = 1,
	ClampToEdge = 2,
	ClampToBorder = 3
};

// MaterialFlags names the supported states explicitly so serialization and switches stay readable.
enum MaterialFlags : std::uint32_t
{
	DoubleSided = 1u << 0,
	UseVertexColor = 1u << 1,
	ReceivesShadows = 1u << 2
};

// DeriveMaterialShadowBehavior is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline MaterialFileShadowBehavior DeriveMaterialShadowBehavior(std::uint32_t alphaMode, const float emissiveFactor[3])
{
	const bool hasEmissiveOutput =
		emissiveFactor[0] > 0.0f
		|| emissiveFactor[1] > 0.0f
		|| emissiveFactor[2] > 0.0f;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (alphaMode == static_cast<std::uint32_t>(AlphaMode::Mask))
	{
		return hasEmissiveOutput
			? MaterialFileShadowBehavior::EmissiveUsesOpacity
			: MaterialFileShadowBehavior::AlphaCutout;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (alphaMode == static_cast<std::uint32_t>(AlphaMode::Blend))
	{
		return MaterialFileShadowBehavior::TransparentNoShadow;
	}

	return MaterialFileShadowBehavior::Opaque;
}

#pragma pack(push, 1)
// struct TextureBinding groups related state and operations so callers have one documented contract.
struct TextureBinding
{
	TextureSemantic semantic;        // Binary layout remains uint32_t
	TerraEngine::UUID textureUuid;   // Engine-assigned identifier that maps to a TETF asset
	SamplerFilter samplerFilter;     // Binary layout remains uint32_t
	SamplerAddressMode addressU;     // Binary layout remains uint32_t
	SamplerAddressMode addressV;     // Binary layout remains uint32_t
	SamplerAddressMode addressW;     // Binary layout remains uint32_t
	std::uint32_t uvSetIndex;        // Which UV set to sample (0 or 1)
	float uvScale[2];                // Multiplier applied to incoming UVs
	float uvOffset[2];               // Additive offset applied after scaling
};

// struct FloatParameter groups related state and operations so callers have one documented contract.
struct FloatParameter
{
	std::uint32_t nameId;          // Stable identifier for the target uniform/constant buffer slot
	std::uint32_t componentCount;  // 1-4 components are valid
	float value[4];               // Unused components are ignored
};

// ResolvedMaterialSlot is the runtime-facing result after TMAT texture
// semantics, scalar factors, and fallback policy have been combined. It keeps
// renderer/editor preview code from re-deriving the same missing-texture rules.
struct ResolvedMaterialSlot
{
	MaterialRuntimeSlot slot = MaterialRuntimeSlot::Albedo;
	bool hasTexture = false;
	TerraEngine::UUID textureUuid = TerraEngine::EMPTY_UUID;
	float defaultValue[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	std::string fallbackDescription;
};

// On-disk header placed at the start of every TMAT blob.
// struct MaterialHeader groups related state and operations so callers have one documented contract.
struct MaterialHeader
{
	char magic[4];
	std::uint16_t version;
	std::uint16_t headerSize;           // Always set to sizeof(MaterialHeader)
	std::uint32_t flags;                // MaterialFlags bitmask
	std::uint32_t shadingModel;         // ShadingModel
	std::uint32_t alphaMode;            // AlphaMode
	TerraEngine::UUID shaderCollectionId; // Vertex/fragment pairing used to shade this material
	float baseColorFactor[4];
	float emissiveFactor[3];
	float metallicFactor;
	float roughnessFactor;
	float normalScale;
	float occlusionStrength;
	float alphaCutoff;
	std::uint32_t textureCount;
	std::uint32_t floatParameterCount;
	std::uint32_t textureArrayOffset;   // File-relative byte offset to TextureBinding array
	std::uint32_t floatParameterOffset; // File-relative byte offset to FloatParameter array
};
#pragma pack(pop)

static_assert(sizeof(MaterialHeader) <= 256, "TMAT header should remain compact");

// Optional TMVG extension: TMAT v1 readers stop at MaterialHeader, while
// virtual-geometry-aware readers validate a separately tagged record after the
// normal arrays. This keeps legacy assets readable without treating trailing
// bytes as virtual-geometry policy.
inline constexpr std::uint32_t kVirtualGeometryMaterialExtensionMagicValue = 0x47564d54u; // 'TMVG' little endian
inline constexpr std::uint32_t kVirtualGeometryMaterialExtensionVersion = 1u;
// These bits describe material requirements, not backend capabilities. A
// backend compares them with its own declared feature mask before it selects a
// programmable virtual-geometry pipeline.
enum class VirtualGeometryMaterialFeature : std::uint32_t
{
	VisibilityBuffer = 1u << 0u,
	AlphaTest = 1u << 1u,
	ProgrammableShading = 1u << 2u
};

inline constexpr std::uint32_t ToVirtualGeometryMaterialFeatureMask(
	const VirtualGeometryMaterialFeature feature) noexcept
{
	return static_cast<std::uint32_t>(feature);
}
struct VirtualGeometryMaterialExtensionHeader
{
	std::uint32_t magic = kVirtualGeometryMaterialExtensionMagicValue;
	std::uint32_t payloadSize = 0u;
};
static_assert(sizeof(VirtualGeometryMaterialExtensionHeader) == 8u, "TMVG extension envelope must retain a stable wire layout.");

enum class VirtualGeometryMaterialFallbackPolicy : std::uint32_t { Indexed = 0u, Compute = 1u, DisableVirtualGeometry = 2u };
struct VirtualGeometryMaterialExtension
{
	std::uint32_t version = kVirtualGeometryMaterialExtensionVersion;
	std::uint32_t requiredFeatureMask = 0u;
	std::uint32_t programmableFeatureMask = 0u;
	std::uint32_t shadingBin = 0u;
	std::uint32_t permutationKey = 0u;
	std::uint32_t maximumShadingBins = 0u;
	VirtualGeometryMaterialFallbackPolicy fallbackPolicy = VirtualGeometryMaterialFallbackPolicy::Indexed;
	std::uint32_t diagnosticReasonCode = 0u;
	std::uint64_t compatibilitySignature = 0u;
};
static_assert(sizeof(VirtualGeometryMaterialExtension) == 40u, "Optional TMVG extension must retain a stable wire layout.");

// Validate the closed fallback enum so a corrupt optional extension never
// silently selects an invented render route.
inline constexpr bool IsValidVirtualGeometryMaterialExtension(const VirtualGeometryMaterialExtension& extension)
{
	return extension.version == kVirtualGeometryMaterialExtensionVersion
		&& static_cast<std::uint32_t>(extension.fallbackPolicy)
			<= static_cast<std::uint32_t>(VirtualGeometryMaterialFallbackPolicy::DisableVirtualGeometry);
}

// In-memory representation of a fully loaded material file.
// struct Material groups related state and operations so callers have one documented contract.
struct Material
{
	MaterialHeader header{};
	TerraEngine::UUID shaderCollectionId = TerraEngine::EMPTY_UUID;
	MaterialFileShadowBehavior shadowBehavior = MaterialFileShadowBehavior::Opaque;
	std::vector<std::uint8_t> storage;         // Owns the contiguous payload after the header
	std::span<const TextureBinding> textures;  // Views into storage
	std::span<const FloatParameter> floats;    // Views into storage
	std::vector<ResolvedMaterialSlot> resolvedSlots;
	std::vector<std::string> fallbackWarnings;
	std::optional<VirtualGeometryMaterialExtension> virtualGeometryExtension;
};
