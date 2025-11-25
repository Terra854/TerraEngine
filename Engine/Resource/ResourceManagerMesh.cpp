#include "Resource/ResourceManager.h"

#include <cstring>
#include <limits>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineMeshFile.h"

namespace
{
	// Parse a TEMF mesh file from disk and populate spans into the payload.
	// This helper avoids touching global caches so it can be shared by both
	// synchronous and asynchronous entry points.
	std::unique_ptr<Mesh> LoadMeshFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		if (fileBytes.size() < sizeof(Header))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF file is too small: " << resolvedPath);
			return nullptr;
		}

		Header header{};
		std::memcpy(&header, fileBytes.data(), sizeof(Header));

		std::uint32_t magic = 0;
		std::memcpy(&magic, header.magic, sizeof(magic));
		if (magic != kMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF magic mismatch in " << resolvedPath);
			return nullptr;
		}

		if (header.version != kCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF version mismatch in " << resolvedPath << " (expected " << kCurrentVersion << ")");
			return nullptr;
		}

		if (header.layout.attributeCount > kMaxVertexAttributes)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex attribute count exceeds limit in " << resolvedPath);
			return nullptr;
		}

		if (header.headerSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF headerSize exceeds file length for " << resolvedPath);
			return nullptr;
		}

		const std::size_t payloadOffset = header.headerSize;
		auto mesh = std::make_unique<Mesh>();
		mesh->header = header;

		mesh->storage.assign(fileBytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset), fileBytes.end());

		const std::size_t payloadSize = mesh->storage.size();
		const std::uint8_t* payload = mesh->storage.data();

		if (header.vertexDataOffset < header.headerSize || header.indexDataOffset < header.headerSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF data offsets underflow header for " << resolvedPath);
			return nullptr;
		}

		const std::size_t vertexOffset = header.vertexDataOffset - header.headerSize;
		const std::size_t indexOffset = header.indexDataOffset - header.headerSize;

		const std::size_t vertexStride = header.layout.strideBytes;
		const std::size_t vertexBytes = vertexStride * static_cast<std::size_t>(header.vertexCount);

		if (vertexOffset + vertexBytes > payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex buffer overruns payload in " << resolvedPath);
			return nullptr;
		}

		std::size_t indexElementSize = 0;
		switch (static_cast<IndexFormat>(header.indexFormat))
		{
			case IndexFormat::UInt16:
				indexElementSize = sizeof(std::uint16_t);
				break;
			case IndexFormat::UInt32:
				indexElementSize = sizeof(std::uint32_t);
				break;
			default:
				TERRAENGINE_LOG(LogLevel::Error, "TEMF uses unknown index format in " << resolvedPath);
				return nullptr;
		}

		const std::size_t indexBytes = indexElementSize * static_cast<std::size_t>(header.indexCount);
		if (indexOffset + indexBytes > payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF index buffer overruns payload in " << resolvedPath);
			return nullptr;
		}

		mesh->vertexData = std::span<const std::uint8_t>(payload + vertexOffset, vertexBytes);
		mesh->indexData = std::span<const std::uint8_t>(payload + indexOffset, indexBytes);

		return mesh;
	}
}

TerraEngine::UUID ResourceManager::LoadMesh(const std::filesystem::path& path)
{
	return LoadOrGetCached<Mesh>(path, meshCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMeshFile(*this, resolvedPath);
	});
}

std::shared_future<TerraEngine::UUID> ResourceManager::LoadMeshAsync(const std::filesystem::path& path)
{
	return LoadOrSchedule<Mesh>(path, meshCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMeshFile(*this, resolvedPath);
	});
}
