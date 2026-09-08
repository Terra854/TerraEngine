// NavigationGeometryFormat.h -- Checked provider-neutral .tenavgeom geometry.

#pragma once

#include "Core/Export.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace TerraEngine::Navigation
{
	// The file stores all scalar fields explicitly in little-endian order. These
	// offsets are a wire contract, not a C++ struct layout or a provider format.
	struct NavigationGeometryWireLayout final
	{
		static constexpr std::size_t kHeaderSize = 112U;
		static constexpr std::size_t kSchemaVersion = 8U;
		static constexpr std::size_t kPayloadKind = 12U;
		static constexpr std::size_t kVertexCount = 16U;
		static constexpr std::size_t kTriangleCount = 24U;
		static constexpr std::size_t kPrimitiveCount = 32U;
		static constexpr std::size_t kVertexPayloadOffset = 40U;
		static constexpr std::size_t kTrianglePayloadOffset = 48U;
		static constexpr std::size_t kPrimitivePayloadOffset = 56U;
		static constexpr std::size_t kBounds = 64U;
		static constexpr std::size_t kFingerprint = 88U;
		static constexpr std::size_t kChecksum = 96U;
		static constexpr std::size_t kPayloadLength = 104U;
		static constexpr std::size_t kVertexRecordSize = 12U;
		static constexpr std::size_t kTriangleRecordSize = 12U;
		static constexpr std::size_t kPrimitiveRecordSize = 40U;
	};

	inline constexpr std::uint32_t kNavigationGeometrySchemaVersion = 1U;
	inline constexpr std::array<std::byte, 8U> kNavigationGeometryMagic{
		std::byte{'T'}, std::byte{'E'}, std::byte{'N'}, std::byte{'A'},
		std::byte{'V'}, std::byte{'G'}, std::byte{'E'}, std::byte{'O'} };

	enum class NavigationGeometryPayloadKind : std::uint32_t
	{
		TriangleMesh = 1U,
		AnalyticPrimitives = 2U,
		Mixed = 3U
	};
	TERRAENGINE_API std::ostream& operator<<(std::ostream& stream, NavigationGeometryPayloadKind kind);

	enum class NavigationGeometryPrimitiveKind : std::uint32_t
	{
		Box = 1U,
		Sphere = 2U,
		Capsule = 3U
	};

	struct NavigationGeometryVector3
	{
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;
		[[nodiscard]] constexpr bool operator==(const NavigationGeometryVector3&) const noexcept = default;
	};

	struct NavigationGeometryBounds
	{
		NavigationGeometryVector3 minimum{};
		NavigationGeometryVector3 maximum{};
		[[nodiscard]] constexpr bool operator==(const NavigationGeometryBounds&) const noexcept = default;
	};

	struct NavigationGeometryTriangle
	{
		std::uint32_t firstVertex = 0U;
		std::uint32_t secondVertex = 0U;
		std::uint32_t thirdVertex = 0U;
		[[nodiscard]] constexpr bool operator==(const NavigationGeometryTriangle&) const noexcept = default;
	};

	// Primitive values deliberately retain their analytic dimensions. Recast
	// tessellation can be conservative, while final checks still use this exact
	// provider-neutral description rather than an opaque cooked collision blob.
	struct NavigationGeometryPrimitive
	{
		NavigationGeometryPrimitiveKind kind = NavigationGeometryPrimitiveKind::Box;
		NavigationGeometryVector3 center{};
		NavigationGeometryVector3 halfExtents{};
		float radius = 0.0F;
		float height = 0.0F;
		[[nodiscard]] constexpr bool operator==(const NavigationGeometryPrimitive&) const noexcept = default;
	};

	struct NavigationGeometryDocument
	{
		NavigationGeometryPayloadKind kind = NavigationGeometryPayloadKind::TriangleMesh;
		std::vector<NavigationGeometryVector3> vertices;
		std::vector<NavigationGeometryTriangle> triangles;
		std::vector<NavigationGeometryPrimitive> primitives;
	};

	struct NavigationGeometryEncodeResult
	{
		bool accepted = false;
		std::string reason;
		std::vector<std::byte> bytes;
		std::uint64_t fingerprint = 0U;
		std::uint64_t checksum = 0U;
	};

	struct NavigationGeometryDecodeResult
	{
		bool accepted = false;
		std::string reason;
		NavigationGeometryDocument document;
		NavigationGeometryBounds bounds{};
		std::uint64_t fingerprint = 0U;
		std::uint64_t checksum = 0U;
	};

	// Validates all finite/range invariants without allocating a file buffer.
	// Snapshot and compiler paths share this function so they cannot disagree on
	// what is safe provider-neutral source geometry.
	TERRAENGINE_API bool ValidateNavigationGeometryDocument(const NavigationGeometryDocument& document,
		NavigationGeometryBounds& outBounds, std::string& outReason);
	TERRAENGINE_API NavigationGeometryEncodeResult EncodeNavigationGeometry(const NavigationGeometryDocument& document);
	TERRAENGINE_API NavigationGeometryDecodeResult DecodeNavigationGeometry(const std::vector<std::byte>& bytes);
}
