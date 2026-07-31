// TerraEngineShaderFile.h — Binary format definition for shader program assets (TSPF).
//
// This file defines the container format for precompiled shader stages and their
// reflection metadata. It stores descriptor set layouts, push constant ranges,
// and SPIR-V/DXIL bytecode, allowing the resource system to build pipelines
// without expensive runtime shader inspection.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Terra Shader Program Format (TSPF) is the planned container used by the
// resource system to keep shader stages, their source binaries, and reflection
// metadata together in one place so pipelines can be built without probing SPIR-V at runtime.

constexpr std::uint32_t kShaderMagicValue = 0x46505354u; // 'TSPF' little endian
constexpr std::uint16_t kShaderCurrentVersion = 1;
constexpr std::uint32_t kInvalidStringOffset = 0xFFFFFFFFu;

// ShaderSourceLanguage names the supported states explicitly so serialization and switches stay readable.
enum class ShaderSourceLanguage : std::uint32_t
{
	HLSL = 0
};

// ShaderBinaryFormat names the supported states explicitly so serialization and switches stay readable.
enum class ShaderBinaryFormat : std::uint32_t
{
	Unknown = 0,
	SPIRV = 1,
	DXIL = 2
};

// ShaderStage names the supported states explicitly so serialization and switches stay readable.
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

// IoComponentType names the supported states explicitly so serialization and switches stay readable.
enum class IoComponentType : std::uint32_t
{
	Unknown = 0,
	Float = 1,
	Int = 2,
	UInt = 3,
	Bool = 4
};

// DescriptorType names the supported states explicitly so serialization and switches stay readable.
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

// StageToMask is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline constexpr StageVisibilityMask StageToMask(ShaderStage stage)
{
	return 1u << static_cast<std::uint32_t>(stage);
}

#pragma pack(push, 1)
// struct ShaderStageSource groups related state and operations so callers have one documented contract.
struct ShaderStageSource
{
	std::uint32_t language;           // ShaderSourceLanguage
	std::uint32_t stage;              // ShaderStage
	std::uint32_t pathOffset;         // String table offset to the original HLSL path
	std::uint32_t entryPointOffset;   // String table offset to the entry point symbol
	std::uint32_t targetProfileOffset; // String table offset to the target profile (e.g. "vs_6_8")
};

// struct StageIOAttribute groups related state and operations so callers have one documented contract.
struct StageIOAttribute
{
	std::uint32_t location;           // Location decoration on the input/output variable
	std::uint32_t componentCount;     // Number of scalar components (e.g. float3 -> 3)
	std::uint32_t componentType;      // IoComponentType
	std::uint32_t nameOffset;         // Optional string table offset with the variable name (kInvalidStringOffset when absent)
};

// struct DescriptorBinding groups related state and operations so callers have one documented contract.
struct DescriptorBinding
{
	std::uint32_t binding;            // Binding index within the set
	std::uint32_t type;               // DescriptorType
	std::uint32_t arrayCount;         // Descriptor array length (1 for scalars)
	StageVisibilityMask visibility;   // Bitmask of ShaderStage values
	std::uint32_t nameOffset;         // Optional identifier from reflection (string table)
};

// struct DescriptorSetLayoutDesc groups related state and operations so callers have one documented contract.
struct DescriptorSetLayoutDesc
{
	std::uint32_t setIndex;
	std::uint32_t firstBinding;       // Index into the DescriptorBinding array
	std::uint32_t bindingCount;
};

// struct PushConstantRange groups related state and operations so callers have one documented contract.
struct PushConstantRange
{
	std::uint32_t offsetBytes;
	std::uint32_t sizeBytes;
	StageVisibilityMask visibility;   // Bitmask of ShaderStage values
};

// Reflection block for the single stage stored in a TSPF file.
// struct ShaderReflection groups related state and operations so callers have one documented contract.
struct ShaderReflection
{
	ShaderStageSource source;
	std::uint32_t descriptorSetOffset;   // File-relative offset to DescriptorSetLayoutDesc array
	std::uint32_t descriptorSetCount;
	std::uint32_t pushConstantOffset;    // File-relative offset to PushConstantRange array
	std::uint32_t pushConstantCount;
	std::uint32_t inputOffset;           // File-relative offset to StageIOAttribute array (vertex inputs / fragment inputs)
	std::uint32_t inputCount;
	std::uint32_t outputOffset;          // File-relative offset to StageIOAttribute array (vertex outputs / fragment outputs)
	std::uint32_t outputCount;
};

// Precompiled stage payload produced by the offline shader pipeline.
// struct CompiledStageBinary groups related state and operations so callers have one documented contract.
struct CompiledStageBinary
{
	std::uint32_t format;            // ShaderBinaryFormat
	std::uint32_t entryPointOffset;  // String table offset (usually same as reflection)
	std::uint32_t targetProfileOffset; // String table offset (usually same as reflection)
	std::uint32_t bytecodeOffset;    // File-relative byte offset to the blob
	std::uint32_t bytecodeSize;      // Unpadded bytecode length
};

// On-disk header placed at the start of every TSPF blob.
// struct ShaderHeader groups related state and operations so callers have one documented contract.
struct ShaderHeader
{
	char magic[4];
	std::uint16_t version;
	std::uint16_t headerSize;            // Always set to sizeof(ShaderHeader)
	std::uint32_t descriptorSetCount;
	std::uint32_t descriptorBindingCount;
	std::uint32_t pushConstantCount;
	std::uint32_t stageInputCount;
	std::uint32_t stageOutputCount;
	std::uint32_t stringTableOffset;
	std::uint32_t stringTableSize;
	std::uint32_t reflectionOffset;      // ShaderReflection
	std::uint32_t descriptorSetsOffset;  // DescriptorSetLayoutDesc array
	std::uint32_t descriptorBindingsOffset; // DescriptorBinding array
	std::uint32_t pushConstantsOffset;   // PushConstantRange array
	std::uint32_t stageInputsOffset;     // StageIOAttribute array (inputs)
	std::uint32_t stageOutputsOffset;    // StageIOAttribute array (outputs)
	std::uint32_t compiledBinaryOffset;  // CompiledStageBinary block
	std::uint32_t compiledBinarySize;    // Bytecode size (mirrors CompiledStageBinary::bytecodeSize)
};
#pragma pack(pop)

static_assert(sizeof(ShaderHeader) <= 128, "TSPF header should remain compact");

// Optional TSPV provenance follows the v1 payload. The separately tagged
// envelope lets a v1 reader preserve its fixed header while newer readers can
// distinguish a real extension from arbitrary trailing bytes in legacy files.
inline constexpr std::uint32_t kShaderProvenanceExtensionMagicValue = 0x56505354u; // 'TSPV' little endian
inline constexpr std::uint32_t kShaderProvenanceExtensionVersion = 1u;
struct ShaderProvenanceExtensionHeader
{
	std::uint32_t magic = kShaderProvenanceExtensionMagicValue;
	std::uint32_t payloadSize = 0u;
};
static_assert(sizeof(ShaderProvenanceExtensionHeader) == 8u, "TSPV extension envelope must remain byte-stable.");

struct ShaderProvenanceBackendVariantExtension
{
	std::uint32_t version = kShaderProvenanceExtensionVersion;
	std::uint32_t featureMask = 0u;
	std::uint64_t normalizedSourceHash = 0u;
	std::uint64_t normalizedIncludeHash = 0u;
	std::uint64_t interfaceAbiHash = 0u;
	std::uint32_t compilerVersion = 0u;
	std::uint32_t toolchainVersion = 0u;
	std::uint32_t spirvAvailable = 0u;
	std::uint32_t dxilAvailable = 0u;
	std::uint64_t spirvHash = 0u;
	std::uint64_t dxilHash = 0u;
};
static_assert(sizeof(ShaderProvenanceBackendVariantExtension) == 64u, "TSPV extension layout must remain byte-stable.");


// Validate explicit availability values before runtime code uses their hashes.
// A missing backend must carry no hash so an absent DXIL artifact is never
// accidentally presented as a usable backend variant.
inline constexpr bool IsValidShaderProvenanceExtension(const ShaderProvenanceBackendVariantExtension& extension)
{
	return extension.version == kShaderProvenanceExtensionVersion
		&& extension.spirvAvailable <= 1u
		&& extension.dxilAvailable <= 1u
		&& (extension.spirvAvailable != 0u || extension.spirvHash == 0u)
		&& (extension.dxilAvailable != 0u || extension.dxilHash == 0u);
}

// Fully loaded representation: contiguous storage plus typed views into arrays
// and the string table. Serialization will populate these fields.
struct Shader
{
	ShaderHeader header{};
	ShaderReflection reflection{};
	CompiledStageBinary binary{};
	std::vector<std::uint8_t> storage;         // Owns the contiguous payload after the header
	std::span<const DescriptorSetLayoutDesc> descriptorSets;
	std::span<const DescriptorBinding> descriptorBindings;
	std::span<const PushConstantRange> pushConstants;
	std::span<const StageIOAttribute> inputs;
	std::span<const StageIOAttribute> outputs;
	std::span<const std::uint8_t> bytecode;
	std::string_view stringTable;
	std::optional<ShaderProvenanceBackendVariantExtension> provenance;
};
