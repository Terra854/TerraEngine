// DerivedSceneCache.cpp - portable bounded derived-scene envelope encoding.

#include "Scene/DerivedSceneCache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace TerraEngine::Scene {
	namespace {
		constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

		class ByteWriter {
		public:
			void WriteU8(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
			void WriteU32(std::uint32_t value) { WriteInteger(value); }
			void WriteU64(std::uint64_t value) { WriteInteger(value); }
			void WriteString(const std::string& value) {
				WriteU64(value.size());
				for (const char character : value) WriteU8(static_cast<std::uint8_t>(character));
			}
			void WriteBytes(const std::vector<std::byte>& value) { bytes.insert(bytes.end(), value.begin(), value.end()); }
			std::vector<std::byte> bytes;
		private:
			template <typename Integer>
			void WriteInteger(Integer value) {
				for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
					WriteU8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU));
				}
			}
		};

		class ByteReader {
		public:
			explicit ByteReader(const std::vector<std::byte>& source) : bytes(source) {}
			bool ReadU8(std::uint8_t& value) {
				if (offset >= bytes.size()) return false;
				value = static_cast<std::uint8_t>(bytes[offset++]);
				return true;
			}
			bool ReadU32(std::uint32_t& value) { return ReadInteger(value); }
			bool ReadU64(std::uint64_t& value) { return ReadInteger(value); }
			bool ReadString(std::string& value) {
				std::uint64_t length = 0U;
				if (!ReadU64(length) || length > bytes.size() - offset) return false;
				value.assign(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(length));
				offset += static_cast<std::size_t>(length);
				return true;
			}
			bool ReadBytes(std::size_t length, std::vector<std::byte>& value) {
				if (length > bytes.size() - offset) return false;
				value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
				offset += length;
				return true;
			}
			std::size_t Offset() const { return offset; }
		private:
			template <typename Integer>
			bool ReadInteger(Integer& value) {
				if (sizeof(Integer) > bytes.size() - offset) return false;
				value = 0;
				for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
					value |= static_cast<Integer>(static_cast<std::uint8_t>(bytes[offset++])) << (index * 8U);
				}
				return true;
			}
			const std::vector<std::byte>& bytes;
			std::size_t offset = 0U;
		};

		bool IdentityMatches(const DerivedSceneCompatibilityIdentity& actual, const DerivedSceneCompatibilityIdentity& expected) {
			return actual.formatLineage == expected.formatLineage && actual.sourceId == expected.sourceId && actual.sourceHash == expected.sourceHash
				&& actual.dependencyHash == expected.dependencyHash && actual.spatialArtifactHash == expected.spatialArtifactHash
				&& actual.componentSchemaFingerprint == expected.componentSchemaFingerprint
				&& actual.reflectionFingerprint == expected.reflectionFingerprint
				&& actual.engineCompatibility == expected.engineCompatibility
				&& actual.platform == expected.platform && actual.byteOrder == expected.byteOrder;
		}

		bool IsKnownSectionType(DerivedSceneSectionType type) {
			switch (type) {
			case DerivedSceneSectionType::Objects:
			case DerivedSceneSectionType::Components:
			case DerivedSceneSectionType::Hierarchy:
			case DerivedSceneSectionType::Spatial:
			case DerivedSceneSectionType::Rendering:
			case DerivedSceneSectionType::Physics:
				return true;
			}
			return false;
		}

		void WriteIdentity(ByteWriter& writer, const DerivedSceneCompatibilityIdentity& identity) {
			writer.WriteString(identity.sourceId); writer.WriteString(identity.sourceHash); writer.WriteString(identity.dependencyHash);
			writer.WriteString(identity.componentSchemaFingerprint); writer.WriteString(identity.reflectionFingerprint);
			writer.WriteString(identity.engineCompatibility); writer.WriteString(identity.platform);
			writer.WriteU8(static_cast<std::uint8_t>(identity.byteOrder)); writer.WriteString(identity.formatLineage); writer.WriteString(identity.spatialArtifactHash);
		}

		bool ReadIdentity(ByteReader& reader, DerivedSceneCompatibilityIdentity& identity) {
			std::uint8_t byteOrder = 0U;
			return reader.ReadString(identity.sourceId) && reader.ReadString(identity.sourceHash)
				&& reader.ReadString(identity.dependencyHash) && reader.ReadString(identity.componentSchemaFingerprint)
				&& reader.ReadString(identity.reflectionFingerprint) && reader.ReadString(identity.engineCompatibility)
				&& reader.ReadString(identity.platform) && reader.ReadU8(byteOrder) && reader.ReadString(identity.formatLineage) && reader.ReadString(identity.spatialArtifactHash)
				&& (identity.byteOrder = static_cast<DerivedSceneByteOrder>(byteOrder), true);
		}
	}

	std::uint64_t CalculateDerivedSceneChecksum(const std::vector<std::byte>& bytes) {
		std::uint64_t checksum = kFnvOffset;
		for (const std::byte value : bytes) {
			checksum ^= static_cast<std::uint8_t>(value);
			checksum *= kFnvPrime;
		}
		return checksum;
	}

	DerivedSceneValidationResult ValidateDerivedSceneEnvelope(const DerivedSceneEnvelope& envelope, const DerivedSceneCompatibilityIdentity& expectedIdentity) {
		const DerivedSceneManifest& manifest = envelope.manifest;
		if (manifest.magic != kDerivedSceneMagic || manifest.formatVersion != kDerivedSceneFormatVersion) {
			return {false, DerivedSceneCacheState::Rejected, "unsupported derived scene magic or format version"};
		}
		if (manifest.identity.formatLineage != kDerivedSceneUnboundedLineage) {
			return {false, DerivedSceneCacheState::Rejected, "derived scene does not declare TerraSceneUnboundedV1 lineage"};
		}
		if (!IdentityMatches(manifest.identity, expectedIdentity)) {
			return {false, DerivedSceneCacheState::Stale, "derived scene identity does not match authoritative source or runtime"};
		}
		if (manifest.sections.empty() || manifest.sections.size() > kDerivedSceneMaximumSectionCount || manifest.sections.size() != envelope.sections.size()) {
			return {false, DerivedSceneCacheState::Rejected, "derived scene section table is missing or exceeds its bound"};
		}
		std::vector<DerivedSceneSectionDescriptor> descriptors = manifest.sections;
		for (std::size_t index = 0U; index < envelope.sections.size(); ++index) {
			const DerivedSceneSectionDescriptor& table = manifest.sections[index];
			const DerivedSceneSectionDescriptor& payload = envelope.sections[index].descriptor;
			if (table.type != payload.type || table.version != payload.version || table.offset != payload.offset
				|| table.storedBytes != payload.storedBytes || table.decodedBytes != payload.decodedBytes
				|| table.itemCount != payload.itemCount || table.required != payload.required || table.checksum != payload.checksum) {
				return {false, DerivedSceneCacheState::Rejected, "derived scene section table does not match its payload descriptors"};
			}
		}
		std::sort(descriptors.begin(), descriptors.end(), [](const auto& left, const auto& right) { return left.offset < right.offset; });
		std::uint64_t previousEnd = manifest.headerBytes;
		std::vector<std::byte> allPayload;
		for (const DerivedSceneSection& section : envelope.sections) {
			const DerivedSceneSectionDescriptor& descriptor = section.descriptor;
			if (!IsKnownSectionType(descriptor.type) && descriptor.required) {
				return {false, DerivedSceneCacheState::Rejected, "derived scene contains an unknown required section"};
			}
			if (descriptor.storedBytes != section.payload.size() || descriptor.decodedBytes != descriptor.storedBytes
				|| descriptor.checksum != CalculateDerivedSceneChecksum(section.payload)) {
				return {false, DerivedSceneCacheState::Rejected, "derived scene section checksum or declared size is invalid"};
			}
			allPayload.insert(allPayload.end(), section.payload.begin(), section.payload.end());
		}
		for (const DerivedSceneSectionDescriptor& descriptor : descriptors) {
			if (descriptor.offset < previousEnd || descriptor.storedBytes > kDerivedSceneMaximumArtifactBytes
				|| descriptor.offset > kDerivedSceneMaximumArtifactBytes - descriptor.storedBytes) {
				return {false, DerivedSceneCacheState::Rejected, "derived scene section offsets overlap or overflow"};
			}
			previousEnd = descriptor.offset + descriptor.storedBytes;
		}
		if (manifest.artifactBytes != 0U && manifest.artifactBytes < previousEnd) {
			return {false, DerivedSceneCacheState::Rejected, "derived scene artifact byte count is shorter than its section table"};
		}
		if (manifest.payloadChecksum != CalculateDerivedSceneChecksum(allPayload)) {
			return {false, DerivedSceneCacheState::Rejected, "derived scene payload checksum is invalid"};
		}
		return {true, DerivedSceneCacheState::Valid, {}};
	}

	std::vector<std::byte> EncodeDerivedSceneEnvelope(const DerivedSceneEnvelope& source) {
		DerivedSceneEnvelope envelope = source;
		ByteWriter writer;
		writer.WriteU32(envelope.manifest.magic); writer.WriteU32(envelope.manifest.formatVersion);
		writer.WriteU64(envelope.manifest.headerBytes); writer.WriteU64(envelope.manifest.artifactBytes);
		WriteIdentity(writer, envelope.manifest.identity);
		writer.WriteU64(envelope.manifest.objectCount); writer.WriteU64(envelope.manifest.componentCount); writer.WriteU64(envelope.manifest.hierarchyCount);
		writer.WriteU64(envelope.manifest.payloadChecksum); writer.WriteString(envelope.manifest.createdBy);
		writer.WriteU64(envelope.sections.size());
		for (const DerivedSceneSection& section : envelope.sections) {
			const auto& descriptor = section.descriptor;
			writer.WriteU32(static_cast<std::uint32_t>(descriptor.type)); writer.WriteU32(descriptor.version);
			writer.WriteU64(descriptor.offset); writer.WriteU64(descriptor.storedBytes); writer.WriteU64(descriptor.decodedBytes);
			writer.WriteU64(descriptor.itemCount); writer.WriteU8(descriptor.required ? 1U : 0U); writer.WriteU64(descriptor.checksum);
		}
		for (const DerivedSceneSection& section : envelope.sections) writer.WriteBytes(section.payload);
		return std::move(writer.bytes);
	}

	std::optional<DerivedSceneEnvelope> DecodeDerivedSceneEnvelope(const std::vector<std::byte>& bytes, std::string* outError) {
		const auto fail = [outError](std::string reason) -> std::optional<DerivedSceneEnvelope> { if (outError) *outError = std::move(reason); return std::nullopt; };
		if (bytes.empty() || bytes.size() > kDerivedSceneMaximumArtifactBytes) return fail("derived scene artifact is empty or exceeds its bound");
		ByteReader reader(bytes);
		DerivedSceneEnvelope envelope;
		std::uint64_t sectionCount = 0U;
		if (!reader.ReadU32(envelope.manifest.magic) || !reader.ReadU32(envelope.manifest.formatVersion)
			|| !reader.ReadU64(envelope.manifest.headerBytes) || !reader.ReadU64(envelope.manifest.artifactBytes)
			|| !ReadIdentity(reader, envelope.manifest.identity) || !reader.ReadU64(envelope.manifest.objectCount)
			|| !reader.ReadU64(envelope.manifest.componentCount) || !reader.ReadU64(envelope.manifest.hierarchyCount)
			|| !reader.ReadU64(envelope.manifest.payloadChecksum) || !reader.ReadString(envelope.manifest.createdBy)
			|| !reader.ReadU64(sectionCount) || sectionCount == 0U || sectionCount > kDerivedSceneMaximumSectionCount) return fail("derived scene header or section count is invalid");
		envelope.sections.resize(static_cast<std::size_t>(sectionCount)); envelope.manifest.sections.resize(static_cast<std::size_t>(sectionCount));
		for (std::size_t index = 0U; index < envelope.sections.size(); ++index) {
			auto& descriptor = envelope.sections[index].descriptor; std::uint32_t type = 0U; std::uint8_t required = 0U;
			if (!reader.ReadU32(type) || !reader.ReadU32(descriptor.version) || !reader.ReadU64(descriptor.offset)
				|| !reader.ReadU64(descriptor.storedBytes) || !reader.ReadU64(descriptor.decodedBytes)
				|| !reader.ReadU64(descriptor.itemCount) || !reader.ReadU8(required) || !reader.ReadU64(descriptor.checksum)) return fail("derived scene section directory is truncated");
			descriptor.type = static_cast<DerivedSceneSectionType>(type); descriptor.required = required != 0U; envelope.manifest.sections[index] = descriptor;
		}
		for (DerivedSceneSection& section : envelope.sections) {
			if (section.descriptor.storedBytes > std::numeric_limits<std::size_t>::max()
				|| !reader.ReadBytes(static_cast<std::size_t>(section.descriptor.storedBytes), section.payload)) return fail("derived scene section payload is truncated");
		}
		if (reader.Offset() != bytes.size()) return fail("derived scene contains trailing bytes outside the declared envelope");
		return envelope;
	}

	DerivedSceneDecodeResult PrepareDerivedSceneSections(const DerivedSceneEnvelope& envelope, const DerivedSceneCompatibilityIdentity& expectedIdentity, const std::function<bool()>& cancellationRequested) {
		const DerivedSceneValidationResult validation = ValidateDerivedSceneEnvelope(envelope, expectedIdentity);
		if (!validation.accepted) return {false, false, true, validation.state, validation.reason, {}};
		DerivedSceneDecodeResult result; result.accepted = true; result.state = DerivedSceneCacheState::Decoded;
		for (const DerivedSceneSection& section : envelope.sections) {
			if (cancellationRequested && cancellationRequested()) {
				return {false, true, false, DerivedSceneCacheState::Rejected, "derived scene preparation was cancelled", {}};
			}
			// Copying makes this a worker-owned immutable preparation record.  The
			// protected publication step receives only this value data later.
			result.sections.push_back({section.descriptor, section.payload});
		}
		return result;
	}

}
