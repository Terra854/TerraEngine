#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cmath>
#include <cctype>
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

	bool DecodeRemapStream(
		const std::uint8_t* encodedData,
		std::size_t remapCount,
		MeshletRemapEncoding encoding,
		std::vector<std::uint32_t>& outDecoded)
	{
		outDecoded.resize(remapCount);
		const std::size_t elementSize = static_cast<std::size_t>(GetRemapElementSizeBytes(encoding));
		for (std::size_t remapIndex = 0; remapIndex < remapCount; ++remapIndex)
		{
			std::size_t byteOffset = 0;
			if (!TryMultiply(remapIndex, elementSize, byteOffset))
			{
				return false;
			}

			if (encoding == MeshletRemapEncoding::Uint16)
			{
				std::uint16_t packed = 0;
				std::memcpy(&packed, encodedData + byteOffset, sizeof(packed));
				outDecoded[remapIndex] = static_cast<std::uint32_t>(packed);
			}
			else
			{
				std::uint32_t packed = 0;
				std::memcpy(&packed, encodedData + byteOffset, sizeof(packed));
				outDecoded[remapIndex] = packed;
			}
		}

		return true;
	}

	bool IsFiniteFloat(float value)
	{
		return std::isfinite(value) != 0;
	}

	void DisableConeData(MeshletBounds& bounds)
	{
		bounds.coneApex[0] = bounds.center[0];
		bounds.coneApex[1] = bounds.center[1];
		bounds.coneApex[2] = bounds.center[2];
		bounds.coneAxis[0] = 0.0f;
		bounds.coneAxis[1] = 0.0f;
		bounds.coneAxis[2] = 0.0f;
		// Values outside [-1, 1] are treated by the task shader as "disabled".
		bounds.coneCutoff = 2.0f;
		bounds.coneApexOffset = 0.0f;
	}

	bool IsCompiledMeshPath(const std::filesystem::path& path)
	{
		std::string extension = path.extension().generic_string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		return extension == ".temf";
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

		// Direct replacement intentionally rejects every non-current layout.
		if (header.version != kMeshCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF version mismatch in " << resolvedPath
				<< ". Expected version " << kMeshCurrentVersion
				<< " with no backward compatibility path.");
			return nullptr;
		}

		if (header.layout.attributeCount > kMaxVertexAttributes)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex attribute count exceeds limit in " << resolvedPath);
			return nullptr;
		}

		if (header.layout.strideBytes != kMeshletPackedVertexStrideBytes)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF stride mismatch in " << resolvedPath
				<< ". Expected fixed " << kMeshletPackedVertexStrideBytes
				<< "-byte packed vertex layout.");
			return nullptr;
		}

		if (header.layout.attributeCount != 4u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF packed vertex layout must expose exactly 4 attributes in " << resolvedPath);
			return nullptr;
		}

		const VertexAttribute expectedLayout[4] = {
			{
				static_cast<std::uint32_t>(AttributeSemantic::Position),
				static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm),
				0u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::Normal),
				static_cast<std::uint32_t>(AttributeFormat::Oct32),
				4u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::Tangent),
				static_cast<std::uint32_t>(AttributeFormat::Oct32),
				8u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::TexCoord0),
				static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm),
				12u}};

		for (std::size_t layoutIndex = 0; layoutIndex < 4u; ++layoutIndex)
		{
			const VertexAttribute& current = header.layout.attributes[layoutIndex];
			const VertexAttribute& expected = expectedLayout[layoutIndex];
			if (current.semantic != expected.semantic
				|| current.format != expected.format
				|| current.offsetBytes != expected.offsetBytes)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"TEMF packed attribute mismatch at index " << layoutIndex << " in " << resolvedPath);
				return nullptr;
			}
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

		if (header.meshletVertexCount == 0
			|| header.meshletVertexRemapCount == 0
			|| header.meshletPrimitiveIndexCount == 0
			|| header.meshletCount == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet counts (vertex/remap/primitive/table) must all be non-zero in " << resolvedPath);
			return nullptr;
		}

		if (header.meshletSectionCount == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF requires at least one meshlet section in " << resolvedPath);
			return nullptr;
		}

		if ((header.meshletVertexDataOffset % kMeshletVertexDataAlignmentBytes) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex stream offset must be 16-byte aligned in " << resolvedPath);
		}

		if (header.meshletSkinningDataCount != 0)
		{
			if ((header.meshletSkinningDataOffset % kMeshletIndexDataAlignmentBytes) != 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF skinning stream offset must be aligned in " << resolvedPath);
				return nullptr;
			}
			
			if (header.meshletSkinningDataCount != header.meshletVertexCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF skinning count must match vertex count in " << resolvedPath);
				return nullptr;
			}
		}

		if ((header.meshletVertexRemapDataOffset % kMeshletIndexDataAlignmentBytes) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex remap stream offset must be 4-byte aligned in " << resolvedPath);
			return nullptr;
		}

		if ((header.meshletPrimitiveIndexDataOffset % kMeshletIndexDataAlignmentBytes) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF primitive stream offset must be 4-byte aligned in " << resolvedPath);
			return nullptr;
		}

		// Primitive entries are meshlet-local uint8 elements and must remain triangle-aligned.
		if ((header.meshletPrimitiveIndexCount % 3u) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet primitive index count must be a multiple of 3 in " << resolvedPath);
			return nullptr;
		}

		if ((header.meshletFlags & ~kMeshletHeaderFlagHasBounds) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshletFlags has unsupported bits in " << resolvedPath);
			return nullptr;
		}

		const MeshletRemapEncoding remapEncoding = static_cast<MeshletRemapEncoding>(header.meshletVertexRemapEncoding);
		if (remapEncoding != MeshletRemapEncoding::Uint16 && remapEncoding != MeshletRemapEncoding::Uint32)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF remap encoding is invalid in " << resolvedPath);
			return nullptr;
		}

		if (remapEncoding == MeshletRemapEncoding::Uint16
			&& header.meshletVertexCount > static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF remap encoding is u16 but vertex count exceeds 65535 in " << resolvedPath);
			return nullptr;
		}

		const bool boundsPresent = (header.meshletFlags & kMeshletHeaderFlagHasBounds) != 0u;
		if (boundsPresent)
		{
			if (header.meshletBoundsCount != header.meshletCount || header.meshletBoundsOffset == 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF bounds flag is set but bounds count/offset is inconsistent in " << resolvedPath);
				return nullptr;
			}

			if ((header.meshletBoundsOffset % kMeshletIndexDataAlignmentBytes) != 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF bounds payload offset must be 4-byte aligned in " << resolvedPath);
				return nullptr;
			}
		}
		else if (header.meshletBoundsCount != 0u || header.meshletBoundsOffset != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF bounds payload must be fully absent when bounds flag is not set in " << resolvedPath);
			return nullptr;
		}

		std::size_t vertexBytes = 0;
		if (!TryMultiply(static_cast<std::size_t>(header.layout.strideBytes), static_cast<std::size_t>(header.meshletVertexCount), vertexBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex payload size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t skinningBytes = 0;
		if (!TryMultiply(static_cast<std::size_t>(header.meshletSkinningDataCount), 8ULL, skinningBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF skinning payload size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t vertexRemapBytes = 0;
		if (!TryMultiply(
			static_cast<std::size_t>(header.meshletVertexRemapCount),
			static_cast<std::size_t>(GetRemapElementSizeBytes(remapEncoding)),
			vertexRemapBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex remap payload size overflow in " << resolvedPath);
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

		std::size_t vertexRemapPayloadOffset = 0;
		if (!ResolvePayloadRange(header, payloadSize, header.meshletVertexRemapDataOffset, vertexRemapBytes, resolvedPath, "meshlet vertex remap data", vertexRemapPayloadOffset))
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

		std::size_t skinningPayloadOffset = 0;
		if (header.meshletSkinningDataCount != 0 && !ResolvePayloadRange(header, payloadSize, header.meshletSkinningDataOffset, skinningBytes, resolvedPath, "meshlet skinning data", skinningPayloadOffset))
		{
			return nullptr;
		}

		mesh->meshletVertexData = std::span<const std::uint8_t>(payload + vertexPayloadOffset, vertexBytes);
		mesh->meshletVertexRemapData = std::span<const std::uint8_t>(payload + vertexRemapPayloadOffset, vertexRemapBytes);
		mesh->meshletSkinningData = (header.meshletSkinningDataCount != 0) 
			? std::span<const std::uint8_t>(payload + skinningPayloadOffset, skinningBytes)
			: std::span<const std::uint8_t>{};
		mesh->meshletPrimitiveIndexData = std::span<const std::uint8_t>(payload + primitivePayloadOffset, primitiveIndexBytes);

		mesh->meshletRecords.resize(header.meshletCount);
		std::memcpy(mesh->meshletRecords.data(), payload + meshletTableOffset, meshletTableBytes);

		mesh->meshletSections.resize(header.meshletSectionCount);
		std::memcpy(mesh->meshletSections.data(), payload + meshletSectionOffset, meshletSectionBytes);

		if (header.meshletBoundsCount != 0)
		{
			mesh->meshletBounds.resize(header.meshletBoundsCount);
			std::memcpy(mesh->meshletBounds.data(), payload + meshletBoundsOffset, meshletBoundsBytes);

			for (std::size_t boundsIndex = 0; boundsIndex < mesh->meshletBounds.size(); ++boundsIndex)
			{
				MeshletBounds& bounds = mesh->meshletBounds[boundsIndex];
				if (!IsFiniteFloat(bounds.center[0])
					|| !IsFiniteFloat(bounds.center[1])
					|| !IsFiniteFloat(bounds.center[2])
					|| !IsFiniteFloat(bounds.radius)
					|| bounds.radius < 0.0f)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet bounds[" << boundsIndex << "] has invalid sphere data in " << resolvedPath);
					return nullptr;
				}

				const glm::vec3 center(bounds.center[0], bounds.center[1], bounds.center[2]);
				const glm::vec3 extent(bounds.radius);
				const auto meshletBounds = TerraEngine::Scene::MakeAABB(center - extent, center + extent, TerraEngine::Scene::SpatialBoundsSource::Mesh);
				if (mesh->hasLocalBounds)
				{
					mesh->localBoundsMin[0] = std::min(mesh->localBoundsMin[0], meshletBounds.min.x);
					mesh->localBoundsMin[1] = std::min(mesh->localBoundsMin[1], meshletBounds.min.y);
					mesh->localBoundsMin[2] = std::min(mesh->localBoundsMin[2], meshletBounds.min.z);
					mesh->localBoundsMax[0] = std::max(mesh->localBoundsMax[0], meshletBounds.max.x);
					mesh->localBoundsMax[1] = std::max(mesh->localBoundsMax[1], meshletBounds.max.y);
					mesh->localBoundsMax[2] = std::max(mesh->localBoundsMax[2], meshletBounds.max.z);
				}
				else
				{
					mesh->hasLocalBounds = true;
					mesh->localBoundsMin[0] = meshletBounds.min.x;
					mesh->localBoundsMin[1] = meshletBounds.min.y;
					mesh->localBoundsMin[2] = meshletBounds.min.z;
					mesh->localBoundsMax[0] = meshletBounds.max.x;
					mesh->localBoundsMax[1] = meshletBounds.max.y;
					mesh->localBoundsMax[2] = meshletBounds.max.z;
				}

				// Cone validation is fail-safe: invalid payload disables backface culling
				// for that meshlet instead of rejecting the whole asset.
				const bool coneInvalid = !IsFiniteFloat(bounds.coneApex[0])
					|| !IsFiniteFloat(bounds.coneApex[1])
					|| !IsFiniteFloat(bounds.coneApex[2])
					|| !IsFiniteFloat(bounds.coneAxis[0])
					|| !IsFiniteFloat(bounds.coneAxis[1])
					|| !IsFiniteFloat(bounds.coneAxis[2])
					|| !IsFiniteFloat(bounds.coneCutoff)
					|| !IsFiniteFloat(bounds.coneApexOffset);
				if (coneInvalid)
				{
					TERRAENGINE_LOG(LogLevel::Warning, "TEMF meshlet bounds[" << boundsIndex << "] has invalid cone data; disabling cone backface culling for this meshlet.");
					DisableConeData(bounds);
					continue;
				}

				const float axisLengthSq = bounds.coneAxis[0] * bounds.coneAxis[0]
					+ bounds.coneAxis[1] * bounds.coneAxis[1]
					+ bounds.coneAxis[2] * bounds.coneAxis[2];
				if (axisLengthSq < 1e-8f)
				{
					DisableConeData(bounds);
					continue;
				}

				const float invAxisLength = 1.0f / std::sqrt(axisLengthSq);
				bounds.coneAxis[0] *= invAxisLength;
				bounds.coneAxis[1] *= invAxisLength;
				bounds.coneAxis[2] *= invAxisLength;
			}
		}
		else
		{
			// Missing meshlet bounds should not make object-level culling unsafe.
			// The renderer will include fallback records so visibility remains
			// correct until the asset pipeline produces precise bounds.
			mesh->hasLocalBounds = false;
		}

		const std::uint8_t* primitivePayload = mesh->meshletPrimitiveIndexData.data();

		std::vector<std::uint8_t> primitiveCoverage(static_cast<std::size_t>(header.meshletPrimitiveIndexCount), 0u);
		for (std::size_t meshletIndex = 0; meshletIndex < mesh->meshletRecords.size(); ++meshletIndex)
		{
			const MeshletRecord& meshlet = mesh->meshletRecords[meshletIndex];
			if (meshlet.vertexCount == 0
				|| meshlet.primitiveIndexCount == 0
				|| meshlet.vertexCount > kMeshletMaxVertices
				|| (meshlet.primitiveIndexCount % 3u) != 0u)
			{
				// Count validation is kept strict so malformed files fail before GPU upload.
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet[" << meshletIndex << "] has invalid vertex/primitive counts in " << resolvedPath);
				return nullptr;
			}

			std::size_t meshletVertexEnd = 0;
			if (!TryAdd(static_cast<std::size_t>(meshlet.vertexOffset), static_cast<std::size_t>(meshlet.vertexCount), meshletVertexEnd)
				|| meshletVertexEnd > header.meshletVertexRemapCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet[" << meshletIndex << "] remap range overruns in " << resolvedPath);
				return nullptr;
			}

			std::size_t meshletPrimitiveEnd = 0;
			if (!TryAdd(static_cast<std::size_t>(meshlet.primitiveIndexOffset), static_cast<std::size_t>(meshlet.primitiveIndexCount), meshletPrimitiveEnd)
				|| meshletPrimitiveEnd > header.meshletPrimitiveIndexCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet[" << meshletIndex << "] primitive range overruns in " << resolvedPath);
				return nullptr;
			}

			for (std::size_t primitive = static_cast<std::size_t>(meshlet.primitiveIndexOffset); primitive < meshletPrimitiveEnd; ++primitive)
			{
				if (primitiveCoverage[primitive] != 0u)
				{
					// Overlap means one primitive element would belong to two meshlets, which is invalid.
					TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet table has overlapping primitive ranges in " << resolvedPath);
					return nullptr;
				}

				const std::uint8_t localVertex = primitivePayload[primitive];
				if (localVertex >= meshlet.vertexCount)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMF primitive stream references out-of-range local vertex in meshlet[" << meshletIndex << "] for " << resolvedPath);
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
	if (!IsCompiledMeshPath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping source mesh load until compiled TEMF is available: " << path);
		return TerraEngine::EMPTY_UUID;
	}

	return LoadOrGetCached<Mesh>(path, meshCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMeshFile(*this, resolvedPath);
	});
}

std::shared_future<TerraEngine::UUID> ResourceManager::LoadMeshAsync(const std::filesystem::path& path)
{
	if (!IsCompiledMeshPath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping async source mesh load until compiled TEMF is available: " << path);
		return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}

	return LoadOrSchedule<Mesh>(path, meshCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMeshFile(*this, resolvedPath);
	});
}

void ResourceManager::ReloadMesh(const TerraEngine::UUID& id)
{
	std::string key;
	{
		std::shared_lock lock(meshCache.mutex);
		if (const auto pathIt = meshCache.idToPath.find(id); pathIt != meshCache.idToPath.end())
		{
			key = pathIt->second;
		}
	}

	if (key.empty())
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Cannot reload mesh yet: UUID " << ToHexString(id) << " has no compiled TEMF path registered.");
		return;
	}

	const std::filesystem::path resolvedPath(key);
	if (!IsCompiledMeshPath(resolvedPath))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping mesh reload from non-TEMF path: " << resolvedPath);
		return;
	}

	TERRAENGINE_LOG(LogLevel::Debug, "Reloading mesh from " << resolvedPath);
	auto mesh = LoadMeshFile(*this, resolvedPath);
	if (mesh)
	{
		std::unique_lock lock(meshCache.mutex);
		meshCache.resources[id] = std::move(mesh);
	}
}

TerraEngine::Scene::SpatialAABB ResourceManager::GetMeshLocalBounds(const TerraEngine::UUID& id)
{
	if (Mesh* mesh = GetMesh(id))
	{
		if (mesh->hasLocalBounds)
		{
			return TerraEngine::Scene::MakeAABB(
				glm::vec3(mesh->localBoundsMin[0], mesh->localBoundsMin[1], mesh->localBoundsMin[2]),
				glm::vec3(mesh->localBoundsMax[0], mesh->localBoundsMax[1], mesh->localBoundsMax[2]),
				TerraEngine::Scene::SpatialBoundsSource::Mesh);
		}
		return TerraEngine::Scene::MakeFallbackBounds(glm::vec3(0.0f), 1.0f);
	}

	return TerraEngine::Scene::MakeInvalidBounds(TerraEngine::Scene::SpatialBoundsSource::Mesh);
}
