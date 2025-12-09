#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "Utilities/UUID.h"

// Terra Material Format (TMAT) packs authored material settings so runtime code can bind
// PBR parameters and external textures (TETF) consistently alongside meshes (TEMF).

constexpr std::uint32_t kMaterialMagicValue = 0x54414d54u; // 'TMAT' little endian
constexpr std::uint16_t kMaterialCurrentVersion = 1;

// Limits keep the header compact and predictable on disk.
constexpr std::size_t kMaxTextureBindings = 8;
constexpr std::size_t kMaxFloatParameters = 16;

enum class ShadingModel : std::uint32_t
{
	Unlit = 0,
	MetallicRoughness = 1,
	SpecularGlossiness = 2
};

enum class AlphaMode : std::uint32_t
{
	Opaque = 0,
	Mask = 1,
	Blend = 2
};

enum class TextureSemantic : std::uint32_t
{
	BaseColor = 0,
	Normal = 1,
	MetallicRoughness = 2,
	Occlusion = 3,
	Emissive = 4,
	Height = 5
};

enum class SamplerFilter : std::uint32_t
{
	Nearest = 0,
	Bilinear = 1,
	Trilinear = 2,
	Anisotropic = 3
};

enum class SamplerAddressMode : std::uint32_t
{
	Repeat = 0,
	MirroredRepeat = 1,
	ClampToEdge = 2,
	ClampToBorder = 3
};

enum MaterialFlags : std::uint32_t
{
	DoubleSided = 1u << 0,
	UseVertexColor = 1u << 1,
	ReceivesShadows = 1u << 2
};

#pragma pack(push, 1)
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

struct FloatParameter
{
	std::uint32_t nameId;          // Stable identifier for the target uniform/constant buffer slot
	std::uint32_t componentCount;  // 1-4 components are valid
	float value[4];               // Unused components are ignored
};

// On-disk header placed at the start of every TMAT blob.
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

// In-memory representation of a fully loaded material file.
struct Material
{
	MaterialHeader header{};
	TerraEngine::UUID shaderCollectionId = TerraEngine::EMPTY_UUID;
	std::vector<std::uint8_t> storage;         // Owns the contiguous payload after the header
	std::span<const TextureBinding> textures;  // Views into storage
	std::span<const FloatParameter> floats;    // Views into storage
};
