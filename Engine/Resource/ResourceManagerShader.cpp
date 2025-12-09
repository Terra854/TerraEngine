#include "Resource/ResourceManager.h"

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
		if (relativeOffset + requiredSize > payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF offset overruns payload for " << path);
			return nullptr;
		}

		return payload + relativeOffset;
	}

	// Parse a TSPF shader stage container into a runtime-friendly structure.
	std::unique_ptr<Shader> LoadShaderFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		if (fileBytes.size() < sizeof(ShaderHeader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF file is too small: " << resolvedPath);
			return nullptr;
		}

		ShaderHeader header{};
		std::memcpy(&header, fileBytes.data(), sizeof(ShaderHeader));

		std::uint32_t magic = 0;
		std::memcpy(&magic, header.magic, sizeof(magic));
		if (magic != kShaderMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF magic mismatch in " << resolvedPath);
			return nullptr;
		}

		if (header.version != kShaderCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF version mismatch in " << resolvedPath << " (expected " << kShaderCurrentVersion << ")");
			return nullptr;
		}

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

		if (payloadSize == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TSPF payload is empty for " << resolvedPath);
			return nullptr;
		}

		const std::uint8_t* reflectionPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.reflectionOffset, sizeof(ShaderReflection));
		if (!reflectionPtr)
		{
			return nullptr;
		}
		std::memcpy(&shader->reflection, reflectionPtr, sizeof(ShaderReflection));

		const std::uint8_t* binaryPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.compiledBinaryOffset, sizeof(CompiledStageBinary));
		if (!binaryPtr)
		{
			return nullptr;
		}
		std::memcpy(&shader->binary, binaryPtr, sizeof(CompiledStageBinary));

		if (header.descriptorSetCount > 0)
		{
			const std::size_t descriptorSize = header.descriptorSetCount * sizeof(DescriptorSetLayoutDesc);
			const std::uint8_t* descriptorPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.descriptorSetsOffset, descriptorSize);
			if (!descriptorPtr)
			{
				return nullptr;
			}

			shader->descriptorSets = std::span<const DescriptorSetLayoutDesc>(reinterpret_cast<const DescriptorSetLayoutDesc*>(descriptorPtr), header.descriptorSetCount);
		}

		if (header.descriptorBindingCount > 0)
		{
			const std::size_t bindingsSize = header.descriptorBindingCount * sizeof(DescriptorBinding);
			const std::uint8_t* bindingsPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.descriptorBindingsOffset, bindingsSize);
			if (!bindingsPtr)
			{
				return nullptr;
			}

			shader->descriptorBindings = std::span<const DescriptorBinding>(reinterpret_cast<const DescriptorBinding*>(bindingsPtr), header.descriptorBindingCount);
		}

		if (header.pushConstantCount > 0)
		{
			const std::size_t pushConstantSize = header.pushConstantCount * sizeof(PushConstantRange);
			const std::uint8_t* pushPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.pushConstantsOffset, pushConstantSize);
			if (!pushPtr)
			{
				return nullptr;
			}

			shader->pushConstants = std::span<const PushConstantRange>(reinterpret_cast<const PushConstantRange*>(pushPtr), header.pushConstantCount);
		}

		if (header.stageInputCount > 0)
		{
			const std::size_t inputSize = header.stageInputCount * sizeof(StageIOAttribute);
			const std::uint8_t* inputPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.stageInputsOffset, inputSize);
			if (!inputPtr)
			{
				return nullptr;
			}

			shader->inputs = std::span<const StageIOAttribute>(reinterpret_cast<const StageIOAttribute*>(inputPtr), header.stageInputCount);
		}

		if (header.stageOutputCount > 0)
		{
			const std::size_t outputSize = header.stageOutputCount * sizeof(StageIOAttribute);
			const std::uint8_t* outputPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.stageOutputsOffset, outputSize);
			if (!outputPtr)
			{
				return nullptr;
			}

			shader->outputs = std::span<const StageIOAttribute>(reinterpret_cast<const StageIOAttribute*>(outputPtr), header.stageOutputCount);
		}

		if (header.stringTableSize > 0)
		{
			const std::uint8_t* stringPtr = ValidateRange(resolvedPath, header, payload, payloadSize, header.stringTableOffset, header.stringTableSize);
			if (!stringPtr)
			{
				return nullptr;
			}

			shader->stringTable = std::string_view(reinterpret_cast<const char*>(stringPtr), header.stringTableSize);
		}

		const std::uint8_t* bytecodePtr = ValidateRange(resolvedPath, header, payload, payloadSize, shader->binary.bytecodeOffset, shader->binary.bytecodeSize);
		if (!bytecodePtr)
		{
			return nullptr;
		}

		if (header.compiledBinarySize != shader->binary.bytecodeSize)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "TSPF compiledBinarySize does not match bytecodeSize in " << resolvedPath);
		}

		shader->bytecode = std::span<const std::uint8_t>(bytecodePtr, shader->binary.bytecodeSize);

		return shader;
	}
}

TerraEngine::UUID ResourceManager::LoadShader(const std::filesystem::path& path)
{
	return LoadOrGetCached<Shader>(path, shaderCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadShaderFile(*this, resolvedPath);
	});
}

std::shared_future<TerraEngine::UUID> ResourceManager::LoadShaderAsync(const std::filesystem::path& path)
{
	return LoadOrSchedule<Shader>(path, shaderCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadShaderFile(*this, resolvedPath);
	});
}
