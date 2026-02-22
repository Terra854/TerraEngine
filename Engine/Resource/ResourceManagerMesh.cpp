#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineMeshFile.h"

namespace
{
	bool TryMultiply(std::size_t lhs, std::size_t rhs, std::size_t& outValue)
	{
		if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max() / lhs))
		{
			return false;
		}

		outValue = lhs * rhs;
		return true;
	}

	bool TryAdd(std::size_t lhs, std::size_t rhs, std::size_t& outValue)
	{
		if (rhs > (std::numeric_limits<std::size_t>::max() - lhs))
		{
			return false;
		}

		outValue = lhs + rhs;
		return true;
	}

	bool ResolvePayloadRange(const MeshHeader& header, std::size_t payloadSize, std::uint32_t absoluteOffset, std::size_t byteCount, const std::filesystem::path& resolvedPath, const char* rangeName, std::size_t& outRelativeOffset)
	{
		if (absoluteOffset < header.headerSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF " << rangeName << " offset underflows header in " << resolvedPath);
			return false;
		}

		const std::size_t relativeOffset = static_cast<std::size_t>(absoluteOffset - header.headerSize);
		if (relativeOffset > payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF " << rangeName << " offset exceeds payload in " << resolvedPath);
			return false;
		}

		std::size_t rangeEnd = 0;
		if (!TryAdd(relativeOffset, byteCount, rangeEnd) || rangeEnd > payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF " << rangeName << " range overruns payload in " << resolvedPath);
			return false;
		}

		outRelativeOffset = relativeOffset;
		return true;
	}

	// Parse a TEMF mesh file from disk and populate spans into the payload.
	// This helper avoids touching global caches so it can be shared by both
	// synchronous and asynchronous entry points.
	std::unique_ptr<Mesh> LoadMeshFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		if (fileBytes.size() < sizeof(MeshHeader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF file is too small: " << resolvedPath);
			return nullptr;
		}

		MeshHeader header{};
		std::memcpy(&header, fileBytes.data(), sizeof(MeshHeader));

		std::uint32_t magic = 0;
		std::memcpy(&magic, header.magic, sizeof(magic));
		if (magic != kMeshMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF magic mismatch in " << resolvedPath);
			return nullptr;
		}

		// Meshlet-only TEMF is version-gated so we can fail legacy assets explicitly.
		if (header.version < kMeshCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF version " << header.version << " is a legacy/non-meshlet downgrade path in " << resolvedPath << " (minimum supported: " << kMeshCurrentVersion << ")");
			return nullptr;
		}

		if (header.version > kMeshCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF version " << header.version << " is newer than runtime support in " << resolvedPath << " (current runtime: " << kMeshCurrentVersion << ")");
			return nullptr;
		}

		if (header.layout.attributeCount > kMaxVertexAttributes)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex attribute count exceeds limit in " << resolvedPath);
			return nullptr;
		}

		if (header.layout.strideBytes == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex stride is zero in " << resolvedPath);
			return nullptr;
		}

		if (header.headerSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF headerSize exceeds file length for " << resolvedPath);
			return nullptr;
		}

		if (header.headerSize < sizeof(MeshHeader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF headerSize is smaller than MeshHeader in " << resolvedPath);
			return nullptr;
		}

		if (header.meshletVertexCount == 0 || header.meshletPrimitiveIndexCount == 0 || header.meshletCount == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet counts must all be non-zero in " << resolvedPath);
			return nullptr;
		}

		if (header.meshletSectionCount == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF requires at least one meshlet section in " << resolvedPath);
			return nullptr;
		}

		// Primitive indices are emitted as packed 32-bit triangle lists, so count must be triangle-aligned.
		if ((header.meshletPrimitiveIndexCount % 3u) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet primitive index count must be a multiple of 3 in " << resolvedPath);
			return nullptr;
		}

		if (header.meshletBoundsCount != 0 && header.meshletBoundsCount != header.meshletCount)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet bounds count must be zero or equal meshletCount in " << resolvedPath);
			return nullptr;
		}

		std::size_t vertexBytes = 0;
		if (!TryMultiply(static_cast<std::size_t>(header.layout.strideBytes), static_cast<std::size_t>(header.meshletVertexCount), vertexBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex payload size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t primitiveIndexBytes = 0;
		if (!TryMultiply(static_cast<std::size_t>(header.meshletPrimitiveIndexCount), static_cast<std::size_t>(kMeshletPrimitiveIndexSizeBytes), primitiveIndexBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF primitive index payload size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t meshletTableBytes = 0;
		if (!TryMultiply(static_cast<std::size_t>(header.meshletCount), sizeof(MeshletRecord), meshletTableBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet table size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t meshletSectionBytes = 0;
		if (!TryMultiply(static_cast<std::size_t>(header.meshletSectionCount), sizeof(MeshletSection), meshletSectionBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet section table size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t meshletBoundsBytes = 0;
		if (!TryMultiply(static_cast<std::size_t>(header.meshletBoundsCount), sizeof(MeshletBounds), meshletBoundsBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet bounds table size overflow in " << resolvedPath);
			return nullptr;
		}

		const std::size_t payloadOffset = header.headerSize;
		auto mesh = std::make_unique<Mesh>();
		mesh->header = header;

		mesh->storage.assign(fileBytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset), fileBytes.end());

		const std::size_t payloadSize = mesh->storage.size();
		const std::uint8_t* payload = mesh->storage.data();

		std::size_t vertexPayloadOffset = 0;
		if (!ResolvePayloadRange(header, payloadSize, header.meshletVertexDataOffset, vertexBytes, resolvedPath, "meshlet vertex data", vertexPayloadOffset))
		{
			return nullptr;
		}

		std::size_t primitivePayloadOffset = 0;
		if (!ResolvePayloadRange(header, payloadSize, header.meshletPrimitiveIndexDataOffset, primitiveIndexBytes, resolvedPath, "meshlet primitive index data", primitivePayloadOffset))
		{
			return nullptr;
		}

		std::size_t meshletTableOffset = 0;
		if (!ResolvePayloadRange(header, payloadSize, header.meshletTableOffset, meshletTableBytes, resolvedPath, "meshlet table", meshletTableOffset))
		{
			return nullptr;
		}

		std::size_t meshletSectionOffset = 0;
		if (!ResolvePayloadRange(header, payloadSize, header.meshletSectionOffset, meshletSectionBytes, resolvedPath, "meshlet section table", meshletSectionOffset))
		{
			return nullptr;
		}

		std::size_t meshletBoundsOffset = 0;
		if (header.meshletBoundsCount != 0 && !ResolvePayloadRange(header, payloadSize, header.meshletBoundsOffset, meshletBoundsBytes, resolvedPath, "meshlet bounds table", meshletBoundsOffset))
		{
			return nullptr;
		}

		mesh->meshletVertexData = std::span<const std::uint8_t>(payload + vertexPayloadOffset, vertexBytes);
		mesh->meshletPrimitiveIndexData = std::span<const std::uint8_t>(payload + primitivePayloadOffset, primitiveIndexBytes);

		mesh->meshletRecords.resize(header.meshletCount);
		std::memcpy(mesh->meshletRecords.data(), payload + meshletTableOffset, meshletTableBytes);

		mesh->meshletSections.resize(header.meshletSectionCount);
		std::memcpy(mesh->meshletSections.data(), payload + meshletSectionOffset, meshletSectionBytes);

		if (header.meshletBoundsCount != 0)
		{
			mesh->meshletBounds.resize(header.meshletBoundsCount);
			std::memcpy(mesh->meshletBounds.data(), payload + meshletBoundsOffset, meshletBoundsBytes);
		}

		std::vector<std::uint8_t> primitiveCoverage(static_cast<std::size_t>(header.meshletPrimitiveIndexCount), 0u);
		for (std::size_t meshletIndex = 0; meshletIndex < mesh->meshletRecords.size(); ++meshletIndex)
		{
			const MeshletRecord& meshlet = mesh->meshletRecords[meshletIndex];
			if (meshlet.vertexCount == 0 || meshlet.primitiveIndexCount == 0 || (meshlet.primitiveIndexCount % 3u) != 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet[" << meshletIndex << "] has invalid vertex/primitive counts in " << resolvedPath);
				return nullptr;
			}

			std::size_t meshletVertexEnd = 0;
			if (!TryAdd(static_cast<std::size_t>(meshlet.vertexOffset), static_cast<std::size_t>(meshlet.vertexCount), meshletVertexEnd) || meshletVertexEnd > header.meshletVertexCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet table overrun/underrun on vertex range for meshlet[" << meshletIndex << "] in " << resolvedPath);
				return nullptr;
			}

			std::size_t meshletPrimitiveEnd = 0;
			if (!TryAdd(static_cast<std::size_t>(meshlet.primitiveIndexOffset), static_cast<std::size_t>(meshlet.primitiveIndexCount), meshletPrimitiveEnd) || meshletPrimitiveEnd > header.meshletPrimitiveIndexCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet table overrun/underrun on primitive range for meshlet[" << meshletIndex << "] in " << resolvedPath);
				return nullptr;
			}

			for (std::size_t primitive = static_cast<std::size_t>(meshlet.primitiveIndexOffset); primitive < meshletPrimitiveEnd; ++primitive)
			{
				if (primitiveCoverage[primitive] != 0u)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet table overrun/underrun due to overlapping primitive ranges in " << resolvedPath);
					return nullptr;
				}

				primitiveCoverage[primitive] = 1u;
			}
		}

		const bool hasPrimitiveUnderrun = std::any_of(primitiveCoverage.begin(), primitiveCoverage.end(), [](std::uint8_t value) { return value == 0u; });
		if (hasPrimitiveUnderrun)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet table overrun/underrun due to uncovered primitive indices in " << resolvedPath);
			return nullptr;
		}

		for (std::size_t sectionIndex = 0; sectionIndex < mesh->meshletSections.size(); ++sectionIndex)
		{
			const MeshletSection& section = mesh->meshletSections[sectionIndex];
			if (section.meshletCount == 0)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet section[" << sectionIndex << "] has zero meshlets in " << resolvedPath);
				return nullptr;
			}

			std::size_t sectionEnd = 0;
			if (!TryAdd(static_cast<std::size_t>(section.meshletOffset), static_cast<std::size_t>(section.meshletCount), sectionEnd) || sectionEnd > header.meshletCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet section[" << sectionIndex << "] references out-of-range meshlets in " << resolvedPath);
				return nullptr;
			}
		}

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

void ResourceManager::ReloadMesh(const TerraEngine::UUID& id)
{
	std::string key;
	{
		std::shared_lock lock(meshCache.mutex);
		for (const auto& [path, uuid] : meshCache.pathToId)
		{
			if (uuid == id)
			{
				key = path;
				break;
			}
		}
	}

	if (key.empty())
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Cannot reload mesh: UUID " << ToHexString(id) << " not found in cache path map.");
		return;
	}

	const std::filesystem::path resolvedPath(key);
	TERRAENGINE_LOG(LogLevel::Debug, "Reloading mesh from " << resolvedPath);
	auto mesh = LoadMeshFile(*this, resolvedPath);
	if (mesh)
	{
		std::unique_lock lock(meshCache.mutex);
		meshCache.resources[id] = std::move(mesh);
	}
}
