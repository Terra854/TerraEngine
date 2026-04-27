// ResourceManagerShader.cpp — Shader Resource Management Implementation
//
// Implements the logic for loading and managing individual shader stage resources.
// It handles the retrieval of shader bytecode and reflection metadata from the engine's packaged shader formats.
// This module serves as the foundational layer for building more complex shader collections and graphics pipelines.

#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineShaderFile.h"

namespace
{
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
