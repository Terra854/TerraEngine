#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Terra Shader Program Format (TSPF) is the planned container used by the
// resource system to keep shader stages, their source binaries, and reflection
// metadata together in one place so pipelines can be built without probing SPIR-V at runtime.

constexpr std::uint32_t kMagicValue = 0x46505354u; // 'TSPF' little endian
constexpr std::uint16_t kCurrentVersion = 1;
constexpr std::uint32_t kInvalidStringOffset = 0xFFFFFFFFu;

enum class ShaderSourceLanguage : std::uint32_t
{
	HLSL = 0
};

enum class ShaderBinaryFormat : std::uint32_t
{
	Unknown = 0,
	SPIRV = 1,
	DXIL = 2
};

enum class ShaderStage : std::uint32_t
{
	Vertex = 0,
	TessellationControl = 1,
	TessellationEvaluation = 2,
	Geometry = 3,
	Fragment = 4,
	Compute = 5,
	Mesh = 6,
	Task = 7,
	RayGen = 8,
	AnyHit = 9,
	ClosestHit = 10,
	Miss = 11,
	Intersection = 12,
	Callable = 13
};

enum class DescriptorType : std::uint32_t
{
	UniformBuffer = 0,
	StorageBuffer = 1,
	Sampler = 2,
	SampledImage = 3,
	CombinedImageSampler = 4,
	StorageImage = 5,
	InputAttachment = 6,
	AccelerationStructure = 7
};

using StageVisibilityMask = std::uint32_t;

inline constexpr StageVisibilityMask StageToMask(ShaderStage stage)
{
	return 1u << static_cast<std::uint32_t>(stage);
}

#pragma pack(push, 1)
struct ShaderStageSource
{
	std::uint32_t language;           // ShaderSourceLanguage
	std::uint32_t stage;              // ShaderStage
	std::uint32_t pathOffset;         // String table offset to the original HLSL path
	std::uint32_t entryPointOffset;   // String table offset to the entry point symbol
	std::uint32_t targetProfileOffset; // String table offset to the target profile (e.g. "vs_6_8")
};

struct DescriptorBinding
{
	std::uint32_t binding;            // Binding index within the set
	std::uint32_t type;               // DescriptorType
	std::uint32_t arrayCount;         // Descriptor array length (1 for scalars)
	StageVisibilityMask visibility;   // Bitmask of ShaderStage values
	std::uint32_t nameOffset;         // Optional identifier from reflection (string table)
};

struct DescriptorSetLayoutDesc
{
	std::uint32_t setIndex;
	std::uint32_t firstBinding;       // Index into the DescriptorBinding array
	std::uint32_t bindingCount;
};

struct PushConstantRange
{
	std::uint32_t offsetBytes;
	std::uint32_t sizeBytes;
	StageVisibilityMask visibility;   // Bitmask of ShaderStage values
};

// Reflection block for the single stage stored in a TSPF file.
struct ShaderReflection
{
	ShaderStageSource source;
	std::uint32_t descriptorSetOffset;   // File-relative offset to DescriptorSetLayoutDesc array
	std::uint32_t descriptorSetCount;
	std::uint32_t pushConstantOffset;    // File-relative offset to PushConstantRange array
	std::uint32_t pushConstantCount;
};

// Precompiled stage payload produced by the offline shader pipeline.
struct CompiledStageBinary
{
	std::uint32_t format;            // ShaderBinaryFormat
	std::uint32_t entryPointOffset;  // String table offset (usually same as reflection)
	std::uint32_t targetProfileOffset; // String table offset (usually same as reflection)
	std::uint32_t bytecodeOffset;    // File-relative byte offset to the blob
	std::uint32_t bytecodeSize;      // Unpadded bytecode length
};

// On-disk header placed at the start of every TSPF blob.
struct Header
{
	char magic[4];
	std::uint16_t version;
	std::uint16_t headerSize;            // Always set to sizeof(Header)
	std::uint32_t descriptorSetCount;
	std::uint32_t descriptorBindingCount;
	std::uint32_t pushConstantCount;
	std::uint32_t stringTableOffset;
	std::uint32_t stringTableSize;
	std::uint32_t reflectionOffset;      // ShaderReflection
	std::uint32_t descriptorSetsOffset;  // DescriptorSetLayoutDesc array
	std::uint32_t descriptorBindingsOffset; // DescriptorBinding array
	std::uint32_t pushConstantsOffset;   // PushConstantRange array
	std::uint32_t compiledBinaryOffset;  // CompiledStageBinary block
	std::uint32_t compiledBinarySize;    // Bytecode size (mirrors CompiledStageBinary::bytecodeSize)
};
#pragma pack(pop)

static_assert(sizeof(Header) <= 128, "TSPF header should remain compact");

// Fully loaded representation: contiguous storage plus typed views into arrays
// and the string table. Serialization will populate these fields.
struct Shader
{
	Header header{};
	ShaderReflection reflection{};
	CompiledStageBinary binary{};
	std::vector<std::uint8_t> storage;         // Owns the contiguous payload after the header
	std::span<const DescriptorSetLayoutDesc> descriptorSets;
	std::span<const DescriptorBinding> descriptorBindings;
	std::span<const PushConstantRange> pushConstants;
	std::span<const std::uint8_t> bytecode;
	std::string_view stringTable;
};