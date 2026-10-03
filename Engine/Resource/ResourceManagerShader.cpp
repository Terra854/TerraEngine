// ResourceManagerShader.cpp — Shader Resource Management Implementation
//
// Implements the logic for loading and managing individual shader stage resources.
// It handles the retrieval of shader bytecode and reflection metadata from the engine's packaged shader formats.
// This module serves as the foundational layer for building more complex shader collections and graphics pipelines.

#include "Resource/ResourceManager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <optional>
#include <vector>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineShaderFile.h"

namespace
{
	std::string_view ShaderStringAt(
		const Shader& shader,
		std::uint32_t offset)
	{
		if(offset == kInvalidStringOffset || offset >= shader.stringTable.size())
		{
			return {};
		}
		const char* begin = shader.stringTable.data() + offset;
		const char* end = static_cast<const char*>(
			std::memchr(begin, '\0', shader.stringTable.size() - offset));
		return end != nullptr
			? std::string_view(begin, static_cast<std::size_t>(end - begin))
			: std::string_view{};
	}

	bool ValidateRuntimeGeometryContract(
		const Shader& shader,
		const std::filesystem::path& path)
	{
		if(shader.reflection.descriptorSetCount != shader.header.descriptorSetCount
			|| shader.reflection.pushConstantCount
				!= shader.header.pushConstantCount
			|| shader.reflection.inputCount != shader.header.stageInputCount
			|| shader.reflection.outputCount != shader.header.stageOutputCount
			|| shader.binary.format
				!= static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV)
			|| shader.bytecode.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Shader reflection is incomplete for " << path);
			return false;
		}
		for(const DescriptorSetLayoutDesc& set : shader.descriptorSets)
		{
			if(set.firstBinding > shader.descriptorBindings.size()
				|| set.bindingCount
					> shader.descriptorBindings.size() - set.firstBinding)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"Shader descriptor reflection is out of range for " << path);
				return false;
			}
		}

		std::string source(ShaderStringAt(
			shader, shader.reflection.source.pathOffset));
		std::transform(source.begin(), source.end(), source.begin(),
			[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		// TSPF provenance preserves the compiler host's native path separators.
		// Normalize Windows backslashes before classifying engine-owned geometry
		// stages so an installed shader cannot be mistaken for authored content.
		std::replace(source.begin(), source.end(), '\\', '/');
		if(source.find("engine/shaders/") != std::string::npos
			|| source.find("editor/shaders/") != std::string::npos)
		{
			return true;
		}
		const ShaderStage stage =
			static_cast<ShaderStage>(shader.reflection.source.stage);
		if(stage == ShaderStage::Mesh || stage == ShaderStage::Task)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Custom geometry stage lacks the CompatibleReplacement ABI: "
					<< path);
			return false;
		}
		if(stage != ShaderStage::Fragment)
		{
			return true;
		}
		if(!shader.pushConstants.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Custom surface shader declares incompatible push constants: "
					<< path);
			return false;
		}
		for(const DescriptorSetLayoutDesc& set : shader.descriptorSets)
		{
			if(set.setIndex <= 2u)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"Custom surface shader uses a renderer-reserved descriptor set: "
						<< path);
				return false;
			}
			for(std::uint32_t bindingIndex = 0u;
				bindingIndex < set.bindingCount;
				++bindingIndex)
			{
				const DescriptorBinding& binding =
					shader.descriptorBindings[set.firstBinding + bindingIndex];
				std::string name(ShaderStringAt(shader, binding.nameOffset));
				std::transform(name.begin(), name.end(), name.begin(),
					[](unsigned char value)
					{
						return static_cast<char>(std::tolower(value));
					});
				if(name.find("animation") != std::string::npos
					|| name.find("palette") != std::string::npos
					|| name.find("morph") != std::string::npos
					|| name.find("skin") != std::string::npos)
				{
					TERRAENGINE_LOG(LogLevel::Error,
						"Custom surface shader declares an animation resource: "
							<< path);
					return false;
				}
			}
		}
		constexpr std::array<std::uint32_t, 8> widths{
			2u, 3u, 3u, 4u, 1u, 1u, 1u, 1u
		};
		constexpr std::array<IoComponentType, 8> types{
			IoComponentType::Float, IoComponentType::Float,
			IoComponentType::Float, IoComponentType::Float,
			IoComponentType::UInt, IoComponentType::Float,
			IoComponentType::UInt, IoComponentType::UInt
		};
		if(shader.inputs.size() != widths.size())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Custom surface shader does not consume TerraSurfaceInput: "
					<< path);
			return false;
		}
		for(const StageIOAttribute& input : shader.inputs)
		{
			if(input.location >= widths.size()
				|| input.componentCount != widths[input.location]
				|| input.componentType
					!= static_cast<std::uint32_t>(types[input.location]))
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"Custom surface shader input ABI mismatch: " << path);
				return false;
			}
		}
		return true;
	}

	// Validate the range described by a file offset and size and return a
	// pointer into the payload. Returning nullptr signals that the range
	// would overrun the buffer.
	const std::uint8_t* ValidateRange(const std::filesystem::path& path, const ShaderHeader& header, const std::uint8_t* payload, std::size_t payloadSize, std::size_t fileOffset, std::size_t requiredSize)
	{
		if (fileOffset < header.headerSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF offset underflows header for " << path);
			return nullptr;
		}

		const std::size_t relativeOffset = fileOffset - header.headerSize;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (relativeOffset + requiredSize > payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF offset overruns payload for " << path);
			return nullptr;
		}

		return payload + relativeOffset;
	}

	// Load the optional, trailing TSPV record without changing the fixed v1
	// header. Unknown trailing data is retained for forward compatibility, but a
	// record that identifies itself as TSPV must be internally valid.
	bool LoadShaderProvenance(
		const std::filesystem::path& path,
		const std::vector<std::uint8_t>& fileBytes,
		std::size_t knownPayloadEnd,
		std::optional<ShaderProvenanceBackendVariantExtension>& provenance)
	{
		if(knownPayloadEnd >= fileBytes.size())
		{
			return knownPayloadEnd == fileBytes.size();
		}

		const std::size_t remainingBytes = fileBytes.size() - knownPayloadEnd;
		if(remainingBytes < sizeof(ShaderProvenanceExtensionHeader))
		{
			TERRAENGINE_LOG(LogLevel::Warning,
				"Ignoring unrecognized trailing TSPF bytes in " << path);
			return true;
		}

		ShaderProvenanceExtensionHeader extensionHeader{};
		std::memcpy(&extensionHeader, fileBytes.data() + knownPayloadEnd,
			sizeof(extensionHeader));
		if(extensionHeader.magic != kShaderProvenanceExtensionMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Warning,
				"Ignoring unknown trailing TSPF extension in " << path);
			return true;
		}
		if(extensionHeader.payloadSize != sizeof(ShaderProvenanceBackendVariantExtension)
			|| remainingBytes < sizeof(extensionHeader) + extensionHeader.payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TSPV extension size is invalid in " << path);
			return false;
		}

		ShaderProvenanceBackendVariantExtension parsed{};
		std::memcpy(&parsed,
			fileBytes.data() + knownPayloadEnd + sizeof(extensionHeader),
			sizeof(parsed));
		if(!IsValidShaderProvenanceExtension(parsed))
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TSPV extension is invalid in " << path);
			return false;
		}

		provenance = parsed;
		return true;
	}

	// Parse a TSPF shader stage container into a runtime-friendly structure.
	// LoadShaderFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::unique_ptr<Shader> LoadShaderFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (fileBytes.size() < sizeof(ShaderHeader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF file is too small: " << resolvedPath);
			return nullptr;
		}

		ShaderHeader header{};
		std::memcpy(&header, fileBytes.data(), sizeof(ShaderHeader));

		std::uint32_t magic = 0;
		std::memcpy(&magic, header.magic, sizeof(magic));
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (magic != kShaderMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF magic mismatch in " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.version != kShaderCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF version mismatch in " << resolvedPath << " (expected " << kShaderCurrentVersion << ")");
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.headerSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF headerSize exceeds file length for " << resolvedPath);
			return nullptr;
		}

		const std::size_t payloadOffset = header.headerSize;
		auto shader = std::make_unique<Shader>();
		shader->header = header;

		shader->storage.assign(fileBytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset), fileBytes.end());

		const std::size_t payloadSize = shader->storage.size();
		const std::uint8_t* payload = shader->storage.data();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (payloadSize == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF payload is empty for " << resolvedPath);
			return nullptr;
		}

		const std::uint8_t* reflectionPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.reflectionOffset, sizeof(ShaderReflection));
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!reflectionPtr)
		{
			return nullptr;
		}
		std::memcpy(&shader->reflection, reflectionPtr, sizeof(ShaderReflection));

		const std::uint8_t* binaryPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.compiledBinaryOffset, sizeof(CompiledStageBinary));
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!binaryPtr)
		{
			return nullptr;
		}
		std::memcpy(&shader->binary, binaryPtr, sizeof(CompiledStageBinary));

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.descriptorSetCount > 0)
		{
			const std::size_t descriptorSize = header.descriptorSetCount * sizeof(DescriptorSetLayoutDesc);
			const std::uint8_t* descriptorPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.descriptorSetsOffset, descriptorSize);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!descriptorPtr)
			{
				return nullptr;
			}

			shader->descriptorSets = std::span<const DescriptorSetLayoutDesc>(reinterpret_cast<const DescriptorSetLayoutDesc*>(descriptorPtr), header.descriptorSetCount);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.descriptorBindingCount > 0)
		{
			const std::size_t bindingsSize = header.descriptorBindingCount * sizeof(DescriptorBinding);
			const std::uint8_t* bindingsPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.descriptorBindingsOffset, bindingsSize);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!bindingsPtr)
			{
				return nullptr;
			}

			shader->descriptorBindings = std::span<const DescriptorBinding>(reinterpret_cast<const DescriptorBinding*>(bindingsPtr), header.descriptorBindingCount);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.pushConstantCount > 0)
		{
			const std::size_t pushConstantSize = header.pushConstantCount * sizeof(PushConstantRange);
			const std::uint8_t* pushPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.pushConstantsOffset, pushConstantSize);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!pushPtr)
			{
				return nullptr;
			}

			shader->pushConstants = std::span<const PushConstantRange>(reinterpret_cast<const PushConstantRange*>(pushPtr), header.pushConstantCount);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.stageInputCount > 0)
		{
			const std::size_t inputSize = header.stageInputCount * sizeof(StageIOAttribute);
			const std::uint8_t* inputPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.stageInputsOffset, inputSize);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!inputPtr)
			{
				return nullptr;
			}

			shader->inputs = std::span<const StageIOAttribute>(reinterpret_cast<const StageIOAttribute*>(inputPtr), header.stageInputCount);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.stageOutputCount > 0)
		{
			const std::size_t outputSize = header.stageOutputCount * sizeof(StageIOAttribute);
			const std::uint8_t* outputPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.stageOutputsOffset, outputSize);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!outputPtr)
			{
				return nullptr;
			}

			shader->outputs = std::span<const StageIOAttribute>(reinterpret_cast<const StageIOAttribute*>(outputPtr), header.stageOutputCount);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.stringTableSize > 0)
		{
			const std::uint8_t* stringPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.stringTableOffset, header.stringTableSize);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!stringPtr)
			{
				return nullptr;
			}

			shader->stringTable = std::string_view(reinterpret_cast<const char*>(stringPtr), header.stringTableSize);
		}

		const std::uint8_t* bytecodePtr = ValidateRange(resolvedPath, header, payload, payloadSize, shader->binary.bytecodeOffset, shader->binary.bytecodeSize);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!bytecodePtr)
		{
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.compiledBinarySize != shader->binary.bytecodeSize)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "TSPF compiledBinarySize does not match bytecodeSize in " << resolvedPath);
		}

		shader->bytecode = std::span<const std::uint8_t>(bytecodePtr, shader->binary.bytecodeSize);
		// Every fixed v1 range has already been bounds-checked above. The maximum
		// end identifies where an optional appended extension may safely begin.
		std::size_t knownPayloadEnd = header.headerSize;
		const auto IncludeRange = [&knownPayloadEnd](std::uint32_t offset, std::size_t size)
		{
			knownPayloadEnd = std::max(knownPayloadEnd,
				static_cast<std::size_t>(offset) + size);
		};
		IncludeRange(header.reflectionOffset, sizeof(ShaderReflection));
		IncludeRange(header.compiledBinaryOffset, sizeof(CompiledStageBinary));
		IncludeRange(shader->binary.bytecodeOffset, shader->binary.bytecodeSize);
		IncludeRange(header.descriptorSetsOffset,
			static_cast<std::size_t>(header.descriptorSetCount) * sizeof(DescriptorSetLayoutDesc));
		IncludeRange(header.descriptorBindingsOffset,
			static_cast<std::size_t>(header.descriptorBindingCount) * sizeof(DescriptorBinding));
		IncludeRange(header.pushConstantsOffset,
			static_cast<std::size_t>(header.pushConstantCount) * sizeof(PushConstantRange));
		IncludeRange(header.stageInputsOffset,
			static_cast<std::size_t>(header.stageInputCount) * sizeof(StageIOAttribute));
		IncludeRange(header.stageOutputsOffset,
			static_cast<std::size_t>(header.stageOutputCount) * sizeof(StageIOAttribute));
		IncludeRange(header.stringTableOffset, header.stringTableSize);
		if(!LoadShaderProvenance(resolvedPath, fileBytes, knownPayloadEnd,
			shader->provenance))
		{
			return nullptr;
		}
		if(!ValidateRuntimeGeometryContract(*shader, resolvedPath))
		{
			return nullptr;
		}

		return shader;
	}

	// IsCompiledShaderPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsCompiledShaderPath(const std::filesystem::path& path)
	{
		std::string extension = path.extension().generic_string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		return extension == ".tspf";
	}
}

// ResourceManager::LoadShader is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::LoadShader(const std::filesystem::path& path)
{
	if (!IsCompiledShaderPath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping source shader load until compiled TSPF is available: " << path);
		return TerraEngine::EMPTY_UUID;
	}

	return LoadOrGetCached<Shader>(path, shaderCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadShaderFile(*this, resolvedPath);
	});
}

// ResourceManager::LoadShaderAsync is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::shared_future<TerraEngine::UUID> ResourceManager::LoadShaderAsync(const std::filesystem::path& path)
{
	if (!IsCompiledShaderPath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping async source shader load until compiled TSPF is available: " << path);
		return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}

	return LoadOrSchedule<Shader>(path, shaderCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadShaderFile(*this, resolvedPath);
	});
}

ResourceManager::RequestPtr ResourceManager::AdmitShaderDependency(
	const std::filesystem::path& path, const RequestPtr& consumer)
{
	if (!IsCompiledShaderPath(path)) return {};
	// A collection owns a consumer lease rather than a direct request. Losing
	// that parent's authority therefore cannot leave private stage work alive.
	LoadOrSchedule<Shader>(path, shaderCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadShaderFile(*this, resolvedPath);
	}, false, false, false, consumer);
	std::lock_guard gate(m_PublicationGate);
	const std::string mapKey = std::to_string(static_cast<std::uint32_t>(ResourceKind::Shader))
		+ ":" + NormalizeKey(path);
	const auto found = m_Requests.find(mapKey);
	return found == m_Requests.end() ? RequestPtr{} : found->second;
}

std::shared_ptr<const Shader> ResourceManager::AcquireShaderSnapshot(const TerraEngine::UUID& id) const
{
	std::shared_lock lock(shaderCache.mutex);
	const auto found = shaderCache.resources.find(id);
	return found == shaderCache.resources.end() ? nullptr : found->second;
}
