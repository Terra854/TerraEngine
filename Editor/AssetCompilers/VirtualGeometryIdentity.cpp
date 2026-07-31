#include "VirtualGeometryIdentity.h"

namespace TerraEngine::Editor
{
	namespace
	{
		void HashByte(std::uint64_t &hash, std::uint8_t byte) noexcept { hash = (hash ^ byte) * 1099511628211ull; }

		template <typename Value> void HashInteger(std::uint64_t &hash, Value value) noexcept
		{
			// Hash little-endian bytes explicitly so identities do not depend on
			// the host architecture used by an offline cook worker.
			for (std::size_t byteIndex = 0u; byteIndex < sizeof(Value); ++byteIndex)
			{
				HashByte(hash, static_cast<std::uint8_t>((value >> (byteIndex * 8u)) & 0xffu));
			}
		}
	} // namespace

	std::uint64_t BuildVirtualGeometryStableId(std::string_view namespaceName, std::uint64_t contentHash, std::uint32_t ordinal) noexcept
	{
		std::uint64_t hash = 1469598103934665603ull;
		for (const unsigned char byte : namespaceName)
		{
			HashByte(hash, byte);
		}
		HashByte(hash, 0xffu);
		HashInteger(hash, contentHash);
		HashInteger(hash, ordinal);
		return hash == 0u ? 1u : hash;
	}

	VirtualGeometryPrimitiveMapping AssignVirtualGeometryPrimitive(std::uint64_t assetHash, std::uint32_t primitiveOrdinal, std::uint32_t pageCount) noexcept
	{
		const std::uint64_t primitiveId = BuildVirtualGeometryStableId("primitive", assetHash, primitiveOrdinal);
		const std::uint64_t clusterId = BuildVirtualGeometryStableId("cluster", primitiveId, 0u);
		const std::uint64_t pageKey = BuildVirtualGeometryStableId("page", clusterId, 0u);
		return { primitiveId, clusterId, pageCount == 0u ? 0u : static_cast<std::uint32_t>(pageKey % pageCount) };
	}

	VirtualGeometryBoundaryGroup AssignVirtualGeometryBoundaryGroup(
		std::uint64_t assetHash, std::uint32_t primitiveOrdinal, std::uint32_t sourceVertexOrdinal) noexcept
	{
		const std::uint64_t sourcePrimitiveId = BuildVirtualGeometryStableId("source-primitive", assetHash, primitiveOrdinal);
		const std::uint64_t groupId = BuildVirtualGeometryStableId("quantization-boundary", sourcePrimitiveId, sourceVertexOrdinal);
		return { groupId, sourcePrimitiveId, sourceVertexOrdinal };
	}
} // namespace TerraEngine::Editor
