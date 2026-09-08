// NavigationGeometryFormat.cpp -- Explicit little-endian .tenavgeom codec.

#include "Navigation/Geometry/NavigationGeometryFormat.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <ostream>

namespace TerraEngine::Navigation
{
	std::ostream& operator<<(std::ostream& stream, NavigationGeometryPayloadKind kind)
	{
		return stream << static_cast<std::uint32_t>(kind);
	}

	namespace
	{
		constexpr std::size_t kMaximumVertices = 4U * 1024U * 1024U;
		constexpr std::size_t kMaximumTriangles = 8U * 1024U * 1024U;
		constexpr std::size_t kMaximumPrimitives = 4U * 1024U * 1024U;

		[[nodiscard]] bool IsFinite(const NavigationGeometryVector3& value) noexcept
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		[[nodiscard]] bool IsValidPrimitive(const NavigationGeometryPrimitive& primitive) noexcept
		{
			if(!IsFinite(primitive.center) || !IsFinite(primitive.halfExtents)
				|| !std::isfinite(primitive.radius) || !std::isfinite(primitive.height)) return false;
			switch(primitive.kind) {
			case NavigationGeometryPrimitiveKind::Box:
				return primitive.halfExtents.x > 0.0F && primitive.halfExtents.y > 0.0F && primitive.halfExtents.z > 0.0F;
			case NavigationGeometryPrimitiveKind::Sphere:
				return primitive.radius > 0.0F;
			case NavigationGeometryPrimitiveKind::Capsule:
				return primitive.radius > 0.0F && primitive.height >= primitive.radius * 2.0F;
			}
			return false;
		}

		void AppendU32(std::vector<std::byte>& outBytes, std::uint32_t value)
		{
			for(std::size_t index = 0U; index < sizeof(value); ++index)
				outBytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
		}

		void AppendU64(std::vector<std::byte>& outBytes, std::uint64_t value)
		{
			for(std::size_t index = 0U; index < sizeof(value); ++index)
				outBytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
		}

		void AppendFloat(std::vector<std::byte>& outBytes, float value)
		{
			AppendU32(outBytes, std::bit_cast<std::uint32_t>(value));
		}

		[[nodiscard]] bool ReadU32(const std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t& outValue) noexcept
		{
			if(offset > bytes.size() || bytes.size() - offset < sizeof(outValue)) return false;
			outValue = 0U;
			for(std::size_t index = 0U; index < sizeof(outValue); ++index)
				outValue |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
			return true;
		}

		[[nodiscard]] bool ReadU64(const std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t& outValue) noexcept
		{
			if(offset > bytes.size() || bytes.size() - offset < sizeof(outValue)) return false;
			outValue = 0U;
			for(std::size_t index = 0U; index < sizeof(outValue); ++index)
				outValue |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
			return true;
		}

		[[nodiscard]] bool ReadFloat(const std::vector<std::byte>& bytes, std::size_t offset, float& outValue) noexcept
		{
			std::uint32_t encoded = 0U;
			if(!ReadU32(bytes, offset, encoded)) return false;
			outValue = std::bit_cast<float>(encoded);
			return true;
		}

		void WriteU64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value)
		{
			for(std::size_t index = 0U; index < sizeof(value); ++index)
				bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
		}

		[[nodiscard]] bool CheckedByteCount(std::uint64_t count, std::size_t recordBytes, std::size_t maximumCount,
			std::size_t& outBytes) noexcept
		{
			if(count > maximumCount || count > std::numeric_limits<std::size_t>::max() / recordBytes) return false;
			outBytes = static_cast<std::size_t>(count) * recordBytes;
			return true;
		}

		[[nodiscard]] bool IsRange(const std::vector<std::byte>& bytes, std::uint64_t offset, std::size_t count) noexcept
		{
			return offset <= bytes.size() && count <= bytes.size() - static_cast<std::size_t>(offset);
		}

		[[nodiscard]] std::uint64_t HashBytes(const std::vector<std::byte>& bytes) noexcept
		{
			std::uint64_t hash = 14695981039346656037ULL;
			for(const std::byte value : bytes) {
				hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(value));
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		void ExpandBounds(NavigationGeometryBounds& bounds, bool& hasValue, const NavigationGeometryVector3& point) noexcept
		{
			if(!hasValue) {
				bounds.minimum = point;
				bounds.maximum = point;
				hasValue = true;
				return;
			}
			bounds.minimum.x = std::min(bounds.minimum.x, point.x);
			bounds.minimum.y = std::min(bounds.minimum.y, point.y);
			bounds.minimum.z = std::min(bounds.minimum.z, point.z);
			bounds.maximum.x = std::max(bounds.maximum.x, point.x);
			bounds.maximum.y = std::max(bounds.maximum.y, point.y);
			bounds.maximum.z = std::max(bounds.maximum.z, point.z);
		}

		void ExpandPrimitiveBounds(NavigationGeometryBounds& bounds, bool& hasValue,
			const NavigationGeometryPrimitive& primitive) noexcept
		{
			NavigationGeometryVector3 extent = primitive.halfExtents;
			if(primitive.kind == NavigationGeometryPrimitiveKind::Sphere) extent = {primitive.radius, primitive.radius, primitive.radius};
			if(primitive.kind == NavigationGeometryPrimitiveKind::Capsule) extent = {primitive.radius, primitive.height * 0.5F, primitive.radius};
			ExpandBounds(bounds, hasValue, {primitive.center.x - extent.x, primitive.center.y - extent.y, primitive.center.z - extent.z});
			ExpandBounds(bounds, hasValue, {primitive.center.x + extent.x, primitive.center.y + extent.y, primitive.center.z + extent.z});
		}
	}

	bool ValidateNavigationGeometryDocument(const NavigationGeometryDocument& document,
		NavigationGeometryBounds& outBounds, std::string& outReason)
	{
		outBounds = {};
		outReason.clear();
		if(document.vertices.size() > kMaximumVertices || document.triangles.size() > kMaximumTriangles
			|| document.primitives.size() > kMaximumPrimitives) {
			outReason = "GeometryCountExceedsBound";
			return false;
		}
		if(document.vertices.empty() && document.primitives.empty()) {
			outReason = "GeometryHasNoPrimitives";
			return false;
		}
		bool hasBounds = false;
		for(const NavigationGeometryVector3& vertex : document.vertices) {
			if(!IsFinite(vertex)) { outReason = "GeometryContainsNonFiniteVertex"; return false; }
			ExpandBounds(outBounds, hasBounds, vertex);
		}
		for(const NavigationGeometryTriangle& triangle : document.triangles) {
			if(triangle.firstVertex >= document.vertices.size() || triangle.secondVertex >= document.vertices.size()
				|| triangle.thirdVertex >= document.vertices.size()) { outReason = "GeometryTriangleIndexOutOfRange"; return false; }
			if(triangle.firstVertex == triangle.secondVertex || triangle.firstVertex == triangle.thirdVertex
				|| triangle.secondVertex == triangle.thirdVertex) { outReason = "GeometryDegenerateTriangle"; return false; }
		}
		for(const NavigationGeometryPrimitive& primitive : document.primitives) {
			if(!IsValidPrimitive(primitive)) { outReason = "GeometryInvalidPrimitive"; return false; }
			ExpandPrimitiveBounds(outBounds, hasBounds, primitive);
		}
		if(!hasBounds) { outReason = "GeometryHasNoBounds"; return false; }
		return true;
	}

	NavigationGeometryEncodeResult EncodeNavigationGeometry(const NavigationGeometryDocument& document)
	{
		NavigationGeometryEncodeResult result{};
		NavigationGeometryBounds bounds{};
		if(!ValidateNavigationGeometryDocument(document, bounds, result.reason)) return result;

		std::vector<std::byte> payload;
		payload.reserve(document.vertices.size() * NavigationGeometryWireLayout::kVertexRecordSize
			+ document.triangles.size() * NavigationGeometryWireLayout::kTriangleRecordSize
			+ document.primitives.size() * NavigationGeometryWireLayout::kPrimitiveRecordSize);
		for(const NavigationGeometryVector3& vertex : document.vertices) {
			AppendFloat(payload, vertex.x); AppendFloat(payload, vertex.y); AppendFloat(payload, vertex.z);
		}
		for(const NavigationGeometryTriangle& triangle : document.triangles) {
			AppendU32(payload, triangle.firstVertex); AppendU32(payload, triangle.secondVertex); AppendU32(payload, triangle.thirdVertex);
		}
		for(const NavigationGeometryPrimitive& primitive : document.primitives) {
			AppendU32(payload, static_cast<std::uint32_t>(primitive.kind));
			AppendFloat(payload, primitive.center.x); AppendFloat(payload, primitive.center.y); AppendFloat(payload, primitive.center.z);
			AppendFloat(payload, primitive.halfExtents.x); AppendFloat(payload, primitive.halfExtents.y); AppendFloat(payload, primitive.halfExtents.z);
			AppendFloat(payload, primitive.radius); AppendFloat(payload, primitive.height);
		}

		result.bytes.assign(NavigationGeometryWireLayout::kHeaderSize, std::byte{0});
		std::copy(kNavigationGeometryMagic.begin(), kNavigationGeometryMagic.end(), result.bytes.begin());
		std::vector<std::byte> headerValues;
		AppendU32(headerValues, kNavigationGeometrySchemaVersion);
		AppendU32(headerValues, static_cast<std::uint32_t>(document.kind));
		AppendU64(headerValues, document.vertices.size()); AppendU64(headerValues, document.triangles.size()); AppendU64(headerValues, document.primitives.size());
		AppendU64(headerValues, NavigationGeometryWireLayout::kHeaderSize);
		AppendU64(headerValues, NavigationGeometryWireLayout::kHeaderSize + document.vertices.size() * NavigationGeometryWireLayout::kVertexRecordSize);
		AppendU64(headerValues, NavigationGeometryWireLayout::kHeaderSize + document.vertices.size() * NavigationGeometryWireLayout::kVertexRecordSize + document.triangles.size() * NavigationGeometryWireLayout::kTriangleRecordSize);
		for(const float coordinate : {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z, bounds.maximum.x, bounds.maximum.y, bounds.maximum.z}) AppendFloat(headerValues, coordinate);
		std::copy(headerValues.begin(), headerValues.end(), result.bytes.begin() + NavigationGeometryWireLayout::kSchemaVersion);
		result.fingerprint = HashBytes(payload);
		WriteU64(result.bytes, NavigationGeometryWireLayout::kFingerprint, result.fingerprint);
		WriteU64(result.bytes, NavigationGeometryWireLayout::kPayloadLength, payload.size());
		result.bytes.insert(result.bytes.end(), payload.begin(), payload.end());
		result.checksum = HashBytes(result.bytes);
		WriteU64(result.bytes, NavigationGeometryWireLayout::kChecksum, result.checksum);
		result.accepted = true;
		return result;
	}

	NavigationGeometryDecodeResult DecodeNavigationGeometry(const std::vector<std::byte>& bytes)
	{
		NavigationGeometryDecodeResult result{};
		if(bytes.size() < NavigationGeometryWireLayout::kHeaderSize) { result.reason = "GeometryHeaderTruncated"; return result; }
		if(!std::equal(kNavigationGeometryMagic.begin(), kNavigationGeometryMagic.end(), bytes.begin())) { result.reason = "GeometryMagicMismatch"; return result; }
		std::uint32_t schema = 0U, encodedKind = 0U;
		std::uint64_t vertexCount = 0U, triangleCount = 0U, primitiveCount = 0U, vertexOffset = 0U, triangleOffset = 0U, primitiveOffset = 0U, fingerprint = 0U, checksum = 0U, payloadLength = 0U;
		if(!ReadU32(bytes, NavigationGeometryWireLayout::kSchemaVersion, schema) || !ReadU32(bytes, NavigationGeometryWireLayout::kPayloadKind, encodedKind)
			|| !ReadU64(bytes, NavigationGeometryWireLayout::kVertexCount, vertexCount) || !ReadU64(bytes, NavigationGeometryWireLayout::kTriangleCount, triangleCount)
			|| !ReadU64(bytes, NavigationGeometryWireLayout::kPrimitiveCount, primitiveCount) || !ReadU64(bytes, NavigationGeometryWireLayout::kVertexPayloadOffset, vertexOffset)
			|| !ReadU64(bytes, NavigationGeometryWireLayout::kTrianglePayloadOffset, triangleOffset) || !ReadU64(bytes, NavigationGeometryWireLayout::kPrimitivePayloadOffset, primitiveOffset)
			|| !ReadU64(bytes, NavigationGeometryWireLayout::kFingerprint, fingerprint) || !ReadU64(bytes, NavigationGeometryWireLayout::kChecksum, checksum)
			|| !ReadU64(bytes, NavigationGeometryWireLayout::kPayloadLength, payloadLength)) { result.reason = "GeometryHeaderInvalid"; return result; }
		if(schema != kNavigationGeometrySchemaVersion || encodedKind < static_cast<std::uint32_t>(NavigationGeometryPayloadKind::TriangleMesh)
			|| encodedKind > static_cast<std::uint32_t>(NavigationGeometryPayloadKind::Mixed)) { result.reason = "GeometrySchemaUnsupported"; return result; }
		NavigationGeometryBounds declaredBounds{};
		if(!ReadFloat(bytes, NavigationGeometryWireLayout::kBounds, declaredBounds.minimum.x)
			|| !ReadFloat(bytes, NavigationGeometryWireLayout::kBounds + 4U, declaredBounds.minimum.y)
			|| !ReadFloat(bytes, NavigationGeometryWireLayout::kBounds + 8U, declaredBounds.minimum.z)
			|| !ReadFloat(bytes, NavigationGeometryWireLayout::kBounds + 12U, declaredBounds.maximum.x)
			|| !ReadFloat(bytes, NavigationGeometryWireLayout::kBounds + 16U, declaredBounds.maximum.y)
			|| !ReadFloat(bytes, NavigationGeometryWireLayout::kBounds + 20U, declaredBounds.maximum.z)
			|| !IsFinite(declaredBounds.minimum) || !IsFinite(declaredBounds.maximum)
			|| declaredBounds.minimum.x > declaredBounds.maximum.x || declaredBounds.minimum.y > declaredBounds.maximum.y
			|| declaredBounds.minimum.z > declaredBounds.maximum.z) { result.reason = "GeometryBoundsInvalid"; return result; }
		std::size_t vertexBytes = 0U, triangleBytes = 0U, primitiveBytes = 0U;
		if(!CheckedByteCount(vertexCount, NavigationGeometryWireLayout::kVertexRecordSize, kMaximumVertices, vertexBytes)
			|| !CheckedByteCount(triangleCount, NavigationGeometryWireLayout::kTriangleRecordSize, kMaximumTriangles, triangleBytes)
			|| !CheckedByteCount(primitiveCount, NavigationGeometryWireLayout::kPrimitiveRecordSize, kMaximumPrimitives, primitiveBytes)
			|| payloadLength != vertexBytes + triangleBytes + primitiveBytes || vertexOffset != NavigationGeometryWireLayout::kHeaderSize
			|| triangleOffset != vertexOffset + vertexBytes || primitiveOffset != triangleOffset + triangleBytes
			|| !IsRange(bytes, vertexOffset, vertexBytes) || !IsRange(bytes, triangleOffset, triangleBytes) || !IsRange(bytes, primitiveOffset, primitiveBytes)
			|| primitiveOffset + primitiveBytes != bytes.size()) { result.reason = "GeometryPayloadRangeInvalid"; return result; }
		std::vector<std::byte> checksumBytes = bytes;
		WriteU64(checksumBytes, NavigationGeometryWireLayout::kChecksum, 0U);
		if(HashBytes(checksumBytes) != checksum) { result.reason = "GeometryChecksumMismatch"; return result; }
		std::vector<std::byte> payload(bytes.begin() + NavigationGeometryWireLayout::kHeaderSize, bytes.end());
		if(HashBytes(payload) != fingerprint) { result.reason = "GeometryFingerprintMismatch"; return result; }

		result.document.kind = static_cast<NavigationGeometryPayloadKind>(encodedKind);
		result.document.vertices.resize(static_cast<std::size_t>(vertexCount));
		result.document.triangles.resize(static_cast<std::size_t>(triangleCount));
		result.document.primitives.resize(static_cast<std::size_t>(primitiveCount));
		for(std::size_t index = 0U; index < result.document.vertices.size(); ++index) {
			const std::size_t offset = static_cast<std::size_t>(vertexOffset) + index * NavigationGeometryWireLayout::kVertexRecordSize;
			(void)ReadFloat(bytes, offset, result.document.vertices[index].x); (void)ReadFloat(bytes, offset + 4U, result.document.vertices[index].y); (void)ReadFloat(bytes, offset + 8U, result.document.vertices[index].z);
		}
		for(std::size_t index = 0U; index < result.document.triangles.size(); ++index) {
			const std::size_t offset = static_cast<std::size_t>(triangleOffset) + index * NavigationGeometryWireLayout::kTriangleRecordSize;
			(void)ReadU32(bytes, offset, result.document.triangles[index].firstVertex); (void)ReadU32(bytes, offset + 4U, result.document.triangles[index].secondVertex); (void)ReadU32(bytes, offset + 8U, result.document.triangles[index].thirdVertex);
		}
		for(std::size_t index = 0U; index < result.document.primitives.size(); ++index) {
			const std::size_t offset = static_cast<std::size_t>(primitiveOffset) + index * NavigationGeometryWireLayout::kPrimitiveRecordSize;
			std::uint32_t kind = 0U; (void)ReadU32(bytes, offset, kind); result.document.primitives[index].kind = static_cast<NavigationGeometryPrimitiveKind>(kind);
			(void)ReadFloat(bytes, offset + 4U, result.document.primitives[index].center.x); (void)ReadFloat(bytes, offset + 8U, result.document.primitives[index].center.y); (void)ReadFloat(bytes, offset + 12U, result.document.primitives[index].center.z);
			(void)ReadFloat(bytes, offset + 16U, result.document.primitives[index].halfExtents.x); (void)ReadFloat(bytes, offset + 20U, result.document.primitives[index].halfExtents.y); (void)ReadFloat(bytes, offset + 24U, result.document.primitives[index].halfExtents.z);
			(void)ReadFloat(bytes, offset + 28U, result.document.primitives[index].radius); (void)ReadFloat(bytes, offset + 32U, result.document.primitives[index].height);
		}
		std::string validationReason;
		if(!ValidateNavigationGeometryDocument(result.document, result.bounds, validationReason)) { result.reason = validationReason; return result; }
		if(result.bounds != declaredBounds) { result.reason = "GeometryBoundsMismatch"; return result; }
		result.fingerprint = fingerprint;
		result.checksum = checksum;
		result.accepted = true;
		return result;
	}
}
