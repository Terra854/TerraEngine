#include "Runtime/ArtifactValidation.h"

#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineTextureFile.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "Physics/Assets/PhysicsMaterial.h"
#include "Physics/PhysicsSettings.h"
#include "Resource/VirtualGeometryCodec.h"
#include "Scene/Serializer.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

namespace TerraEngine::Runtime {
	namespace {
		// File-owned arithmetic is checked before reads, loops, or allocations.
		// Reads copy integers rather than aliasing unaligned compiler/ABI structs.
		struct Bytes {
			std::span<const std::byte> data;
			bool Range(std::uint64_t offset, std::uint64_t size) const {
				return offset <= data.size() && size <= data.size() - offset;
			}
			std::uint64_t U(std::size_t offset, unsigned size) const {
				std::uint64_t value = 0;
				for(unsigned i = 0; i < size; ++i) value |= std::uint64_t(std::to_integer<unsigned char>(data[offset + i])) << (8 * i);
				return value;
			}
			std::optional<std::string_view> String(std::size_t offset, std::size_t limit) const {
				if(!Range(offset, limit)) return {};
				for(std::size_t size = 0; size < limit; ++size) {
					if(data[offset + size] == std::byte{0}) return std::string_view(reinterpret_cast<const char*>(data.data() + offset), size);
				}
				return {};
			}
		};
		ArtifactValidationResult Decision(ArtifactDisposition disposition, std::string code, std::string diagnostic) {
			return {disposition, std::move(code), std::move(diagnostic), {}, {}};
		}
		ArtifactValidationResult Invalid(std::string code, std::string diagnostic) {
			return Decision(ArtifactDisposition::Invalid, std::move(code), std::move(diagnostic));
		}
		ArtifactValidationResult Valid(std::string bootstrap = {}) {
			auto result = Decision(ArtifactDisposition::Valid, "artifact.valid", "Artifact structure and required contract are valid.");
			result.bootstrap = std::move(bootstrap);
			return result;
		}
		int BootstrapRank(std::string_view name) {
			if(name == "InitManagedRuntimeV4") return 3;
			if(name == "InitManagedRuntimeV3") return 2;
			if(name == "InitManagedRuntime") return 1;
			return 0;
		}
		struct Mapping { std::uint64_t address, memorySize, offset, fileSize; bool executable; };
		std::optional<std::size_t> Map(const Bytes& bytes, const std::vector<Mapping>& mappings,
			std::uint64_t address, std::uint64_t size, bool executable = false) {
			std::optional<std::size_t> result;
			for(const auto& mapping : mappings) {
				if(address < mapping.address || address - mapping.address > mapping.fileSize) continue;
				const auto relative = address - mapping.address;
				if(size > mapping.fileSize - relative || (executable && !mapping.executable)) continue;
				if(result || !bytes.Range(mapping.offset + relative, size)) return {};
				result = static_cast<std::size_t>(mapping.offset + relative);
			}
			return result;
		}
		std::size_t MappedRemaining(const std::vector<Mapping>& mappings, std::uint64_t address) {
			for(const auto& mapping : mappings) {
				if(address >= mapping.address && address - mapping.address < mapping.fileSize)
					return static_cast<std::size_t>(mapping.fileSize - (address - mapping.address));
			}
			return 0;
		}

		ArtifactValidationResult InspectPe(const Bytes& bytes) {
			if(!bytes.Range(0, 64) || bytes.U(0, 2) != 0x5a4d) return Invalid("native.pe.header", "Gameplay image lacks a bounded DOS/PE header.");
			const auto pe = bytes.U(60, 4);
			if(!bytes.Range(pe, 24) || bytes.U(pe, 4) != 0x4550) return Invalid("native.pe.header", "PE signature or COFF header is invalid.");
			if(bytes.U(pe + 4, 2) != 0x8664) return Invalid("native.architecture", "Gameplay image must target AMD64/x64.");
			if((bytes.U(pe + 22, 2) & 0x2002) != 0x2002) return Invalid("native.pe.kind", "Gameplay image must be an executable PE DLL.");
			const auto sections = bytes.U(pe + 6, 2), optionalSize = bytes.U(pe + 20, 2), optional = pe + 24;
			if(optionalSize < 120 || !bytes.Range(optional, optionalSize) || bytes.U(optional, 2) != 0x20b)
				return Invalid("native.pe.optional_header", "Gameplay DLL requires a bounded PE32+ optional header.");
			const auto imageSize = bytes.U(optional + 56, 4), headerSize = bytes.U(optional + 60, 4);
			const auto table = optional + optionalSize;
			if(!sections || !bytes.Range(table, sections * 40) || headerSize < table + sections * 40 || !bytes.Range(0, headerSize) || imageSize < headerSize)
				return Invalid("native.pe.sections", "PE section table or image/header size is invalid.");
			std::vector<Mapping> mappings;
			for(std::uint64_t i = 0; i < sections; ++i) {
				const auto entry = table + i * 40;
				Mapping map{bytes.U(entry + 12, 4), bytes.U(entry + 8, 4), bytes.U(entry + 20, 4), bytes.U(entry + 16, 4), (bytes.U(entry + 36, 4) & 0x20000000) != 0};
				const auto memorySize = std::max(map.memorySize, map.fileSize);
				if(!bytes.Range(map.offset, map.fileSize) || map.address > imageSize || memorySize > imageSize - map.address
					|| (map.fileSize && map.offset < headerSize)) return Invalid("native.pe.section_range", "A PE section extends outside the file or image.");
				for(const auto& prior : mappings) {
					if(map.fileSize && prior.fileSize && map.offset < prior.offset + prior.fileSize && prior.offset < map.offset + map.fileSize)
						return Invalid("native.pe.section_overlap", "PE raw sections overlap.");
					if(memorySize && std::max(prior.memorySize, prior.fileSize) && map.address < prior.address + std::max(prior.memorySize, prior.fileSize)
						&& prior.address < map.address + memorySize) return Invalid("native.pe.section_overlap", "PE virtual sections overlap.");
				}
				mappings.push_back(map);
			}
			if(bytes.U(optional + 108, 4) > (optionalSize - 112) / 8) return Invalid("native.pe.directory_count", "PE data directories exceed the optional header.");
			if(bytes.U(optional + 108, 4) < 1) return Invalid("native.bootstrap.missing", "Gameplay DLL has no export directory.");
			const auto exportAddress = bytes.U(optional + 112, 4), exportSize = bytes.U(optional + 116, 4);
			const auto exports = Map(bytes, mappings, exportAddress, exportSize);
			if(exportSize < 40 || !exports) return Invalid("native.pe.export_range", "PE export directory is absent or out of bounds.");
			const auto functions = bytes.U(*exports + 20, 4), names = bytes.U(*exports + 24, 4);
			const auto functionTable = Map(bytes, mappings, bytes.U(*exports + 28, 4), functions * 4);
			const auto nameTable = Map(bytes, mappings, bytes.U(*exports + 32, 4), names * 4);
			const auto ordinalTable = Map(bytes, mappings, bytes.U(*exports + 36, 4), names * 2);
			if(!functions || !names || !functionTable || !nameTable || !ordinalTable)
				return Invalid("native.pe.export_table", "PE export names, functions, or ordinals are invalid.");
			std::string selected;
			for(std::uint64_t i = 0; i < names; ++i) {
				const auto address = bytes.U(*nameTable + i * 4, 4), ordinal = bytes.U(*ordinalTable + i * 2, 2);
				const auto offset = Map(bytes, mappings, address, 1);
				if(!offset || ordinal >= functions) return Invalid("native.pe.export_name", "PE export name or ordinal is out of bounds.");
				const auto name = bytes.String(*offset, MappedRemaining(mappings, address));
				if(!name) return Invalid("native.pe.export_name", "PE export name is not terminated within its section.");
				if(!BootstrapRank(*name)) continue;
				const auto function = bytes.U(*functionTable + ordinal * 4, 4);
				if((function >= exportAddress && function - exportAddress < exportSize) || !Map(bytes, mappings, function, 1, true))
					return Invalid("native.bootstrap.mapping", "Managed bootstrap must be a defined executable export, not a forwarder.");
				if(BootstrapRank(*name) > BootstrapRank(selected)) selected = *name;
			}
			return selected.empty() ? Invalid("native.bootstrap.missing", "Gameplay DLL lacks InitManagedRuntimeV4, V3, or legacy InitManagedRuntime.") : Valid(std::move(selected));
		}

		ArtifactValidationResult InspectElf(const Bytes& bytes) {
			if(!bytes.Range(0, 64) || bytes.U(0, 4) != 0x464c457f || bytes.U(4, 1) != 2 || bytes.U(5, 1) != 1 || bytes.U(6, 1) != 1)
				return Invalid("native.elf.header", "Gameplay image requires a bounded little-endian ELF64 header.");
			if(bytes.U(18, 2) != 62) return Invalid("native.architecture", "Gameplay image must target AMD64/x64.");
			if(bytes.U(16, 2) != 3 || bytes.U(20, 4) != 1 || bytes.U(52, 2) != 64)
				return Invalid("native.elf.kind", "Gameplay image must be an ELF shared object.");
			const auto sectionTable = bytes.U(40, 8), sectionCount = bytes.U(60, 2);
			if(sectionCount) {
				if(bytes.U(58, 2) != 64 || !bytes.Range(sectionTable, sectionCount * 64)) return Invalid("native.elf.section_range", "ELF section table is invalid.");
				for(std::uint64_t i = 0; i < sectionCount; ++i) {
					const auto entry = sectionTable + i * 64;
					if(bytes.U(entry + 4, 4) != 8 && !bytes.Range(bytes.U(entry + 24, 8), bytes.U(entry + 32, 8)))
						return Invalid("native.elf.section_range", "An ELF section extends outside the file.");
				}
			} else if(sectionTable) return Invalid("native.elf.section_range", "Extended ELF section counts are unsupported by the bounded image contract.");
			const auto table = bytes.U(32, 8), count = bytes.U(56, 2);
			if(bytes.U(54, 2) != 56 || !count || !bytes.Range(table, count * 56)) return Invalid("native.elf.program_range", "ELF program table is invalid.");
			std::vector<Mapping> mappings;
			std::optional<std::pair<std::size_t, std::size_t>> dynamic;
			std::uint64_t dynamicAddress = 0;
			for(std::uint64_t i = 0; i < count; ++i) {
				const auto entry = table + i * 56, type = bytes.U(entry, 4);
				const auto offset = bytes.U(entry + 8, 8), address = bytes.U(entry + 16, 8), fileSize = bytes.U(entry + 32, 8), memorySize = bytes.U(entry + 40, 8);
				if(!bytes.Range(offset, fileSize) || memorySize > std::numeric_limits<std::uint64_t>::max() - address)
					return Invalid("native.elf.segment_range", "An ELF segment extends outside the file or address range.");
				if(type == 3) return Invalid("native.elf.kind", "An ELF interpreter identifies an executable rather than gameplay shared library.");
				if(type == 1) {
					if(fileSize > memorySize) return Invalid("native.elf.segment_range", "ELF load file size exceeds its memory size.");
					mappings.push_back({address, memorySize, offset, fileSize, (bytes.U(entry + 4, 4) & 1) != 0});
				}
				if(type == 2) {
					if(dynamic || fileSize % 16) return Invalid("native.elf.dynamic_range", "ELF dynamic segment is ambiguous or misaligned.");
					dynamic = {{static_cast<std::size_t>(offset), static_cast<std::size_t>(fileSize)}};
					dynamicAddress = address;
				}
			}
			if(!dynamic || mappings.empty()) return Invalid("native.elf.dynamic_range", "ELF shared object requires load and dynamic segments.");
			const auto mappedDynamic = Map(bytes, mappings, dynamicAddress, dynamic->second);
			if(!mappedDynamic || *mappedDynamic != dynamic->first) return Invalid("native.elf.dynamic_range", "ELF dynamic table must reside in its load mapping.");
			std::uint64_t strings = 0, stringSize = 0, symbols = 0, symbolSize = 0, hash = 0, gnuHash = 0;
			bool terminated = false;
			std::array<std::optional<std::uint64_t>, 7> critical;
			for(std::size_t offset = dynamic->first; offset < dynamic->first + dynamic->second; offset += 16) {
				const auto tag = bytes.U(offset, 8), value = bytes.U(offset + 8, 8);
				if(tag == 0) { terminated = true; break; }
				const std::array<std::uint64_t, 7> criticalTags{5, 10, 6, 11, 4, 0x6ffffef5, 0x6ffffffb};
				for(std::size_t i = 0; i < criticalTags.size(); ++i) {
					if(tag != criticalTags[i]) continue;
					if(critical[i] && *critical[i] != value) return Invalid("native.elf.dynamic_conflict", "Conflicting ELF dynamic contract tags are ambiguous.");
					critical[i] = value;
				}
				switch(tag) {
				case 5: strings = value; break; case 10: stringSize = value; break;
				case 6: symbols = value; break; case 11: symbolSize = value; break;
				case 4: hash = value; break; case 0x6ffffef5: gnuHash = value; break;
				case 0x6ffffffb: if(value & 0x08000000) return Invalid("native.elf.kind", "ELF PIE executable is not a gameplay shared library."); break;
				default: break;
				}
			}
			const auto stringTable = Map(bytes, mappings, strings, stringSize);
			if(!terminated || !stringSize || !stringTable || symbolSize != 24 || !Map(bytes, mappings, symbols, 24))
				return Invalid("native.elf.symbol_table", "ELF dynamic strings or symbol table is invalid.");
			std::uint64_t symbolCount = 0;
			if(hash) {
				const auto offset = Map(bytes, mappings, hash, 8);
				if(!offset) return Invalid("native.elf.hash_range", "ELF SysV hash header is invalid.");
				const auto buckets = bytes.U(*offset, 4); symbolCount = bytes.U(*offset + 4, 4);
				if(!Map(bytes, mappings, hash, 8 + (buckets + symbolCount) * 4)) return Invalid("native.elf.hash_range", "ELF SysV hash arrays are invalid.");
			} else if(gnuHash) {
				const auto offset = Map(bytes, mappings, gnuHash, 16);
				if(!offset) return Invalid("native.elf.hash_range", "ELF GNU hash header is invalid.");
				const auto buckets = bytes.U(*offset, 4), firstSymbol = bytes.U(*offset + 4, 4), bloom = bytes.U(*offset + 8, 4);
				const auto hashRemaining = MappedRemaining(mappings, gnuHash);
				if(hashRemaining < 16 || bloom * 8 > hashRemaining - 16 || buckets * 4 > hashRemaining - 16 - bloom * 8)
					return Invalid("native.elf.hash_range", "ELF GNU hash arrays overflow their load mapping.");
				const auto bucketAddress = gnuHash + 16 + bloom * 8;
				const auto bucketOffset = Map(bytes, mappings, bucketAddress, buckets * 4);
				if(!bloom || !buckets || !bucketOffset) return Invalid("native.elf.hash_range", "ELF GNU hash bloom or buckets are invalid.");
				std::uint64_t last = 0;
				for(std::uint64_t i = 0; i < buckets; ++i) last = std::max(last, bytes.U(*bucketOffset + i * 4, 4));
				symbolCount = firstSymbol;
				if(last) {
					if(last < firstSymbol) return Invalid("native.elf.hash_range", "ELF GNU hash bucket precedes its symbol range.");
					const auto chains = bucketAddress + buckets * 4;
					const auto remaining = MappedRemaining(mappings, chains) / 4;
					for(std::uint64_t index = last - firstSymbol; ; ++index) {
						if(index >= remaining) return Invalid("native.elf.hash_range", "ELF GNU hash chain is unterminated or out of bounds.");
						const auto chain = Map(bytes, mappings, chains + index * 4, 4);
						if(!chain) return Invalid("native.elf.hash_range", "ELF GNU hash chain is not mapped.");
						if(bytes.U(*chain, 4) & 1) { symbolCount = firstSymbol + index + 1; break; }
					}
				}
			} else return Invalid("native.elf.hash_missing", "ELF dynamic symbol count has no bounded SysV or GNU hash authority.");
			if(symbolCount > bytes.data.size() / 24) return Invalid("native.elf.symbol_table", "ELF symbol count exceeds the file.");
			const auto symbolTable = Map(bytes, mappings, symbols, symbolCount * 24);
			if(!symbolCount || !symbolTable) return Invalid("native.elf.symbol_table", "ELF dynamic symbols extend outside their mapped segment.");
			std::string selected;
			for(std::uint64_t i = 0; i < symbolCount; ++i) {
				const auto entry = *symbolTable + i * 24, nameIndex = bytes.U(entry, 4);
				if(nameIndex >= stringSize) return Invalid("native.elf.symbol_name", "ELF symbol name lies outside the dynamic string table.");
				const auto name = bytes.String(*stringTable + nameIndex, static_cast<std::size_t>(stringSize - nameIndex));
				if(!name) return Invalid("native.elf.symbol_name", "ELF symbol name is not terminated within its table.");
				if(!BootstrapRank(*name)) continue;
				const auto info = bytes.U(entry + 4, 1), visibility = bytes.U(entry + 5, 1) & 3, section = bytes.U(entry + 6, 2);
				const auto value = bytes.U(entry + 8, 8), size = bytes.U(entry + 16, 8);
				if(!section || ((info >> 4) != 1 && (info >> 4) != 2) || (info & 15) != 2 || (visibility != 0 && visibility != 3)
					|| !Map(bytes, mappings, value, std::max<std::uint64_t>(1, size), true))
					return Invalid("native.bootstrap.mapping", "Managed ELF bootstrap must be a visible, defined executable function.");
				if(BootstrapRank(*name) > BootstrapRank(selected)) selected = *name;
			}
			return selected.empty() ? Invalid("native.bootstrap.missing", "Gameplay shared object lacks InitManagedRuntimeV4, V3, or legacy InitManagedRuntime.") : Valid(std::move(selected));
		}

		ArtifactValidationResult InspectKnown(const ArtifactValidationRequest& request) {
			const Bytes bytes{request.bytes};
			if(request.kind == ArtifactKind::AuthoredData) return Valid();
			if(request.bytes.empty()) return Invalid("artifact.empty", "Required typed artifact is empty.");
			if(request.kind == ArtifactKind::NativeGameplay) {
				if(request.target == NativeImageTarget::WindowsX64) return InspectPe(bytes);
				if(request.target == NativeImageTarget::LinuxX64) return InspectElf(bytes);
				return Decision(ArtifactDisposition::Unsupported, "native.target.unsupported", "Native gameplay inspection supports Windows x64 PE and Linux x64 ELF only.");
			}
			if(request.kind == ArtifactKind::CookedCollider) {
				Physics::CookedColliderDiagnostics diagnostics;
				const auto asset = Physics::CookedColliderAsset::Deserialize(request.bytes, diagnostics);
				if(!asset) return Invalid("artifact.collider.invalid", diagnostics.entries.empty() ? "Cooked collider envelope is invalid." : diagnostics.entries.front().message);
				const auto compatible = asset->CheckCompatibility(Physics::CookedColliderAsset::HostPlatform(), Physics::CookedColliderProviderFamily::Jolt,
					Physics::kJoltCookProviderVersion, Physics::kJoltCookRevision);
				return compatible.compatible ? Valid() : Invalid("artifact.collider.incompatible", compatible.message);
			}
			if(request.kind == ArtifactKind::Mesh) {
				const auto validated = Resource::ValidateTemfProductionFile(
					{reinterpret_cast<const std::uint8_t*>(request.bytes.data()), request.bytes.size()}, {});
				return validated.IsComplete() ? Valid() : Invalid("artifact.mesh.invalid", validated.diagnostic);
			}
			if(request.kind == ArtifactKind::Material) {
				if(!bytes.Range(0, sizeof(MaterialHeader))) return Invalid("artifact.material.header", "TMAT header is truncated.");
				MaterialHeader header{}; std::memcpy(&header, request.bytes.data(), sizeof(header));
				std::uint32_t magic; std::memcpy(&magic, header.magic, sizeof(magic));
				if(magic != kMaterialMagicValue || header.version != kMaterialCurrentVersion || header.headerSize < sizeof(header) || !bytes.Range(0, header.headerSize)
					|| header.textureCount > kMaxTextureBindings || header.floatParameterCount > kMaxFloatParameters
					|| (header.textureCount && (header.textureArrayOffset < header.headerSize || !bytes.Range(header.textureArrayOffset, std::uint64_t(header.textureCount) * sizeof(TextureBinding))))
					|| (header.floatParameterCount && (header.floatParameterOffset < header.headerSize || !bytes.Range(header.floatParameterOffset, std::uint64_t(header.floatParameterCount) * sizeof(FloatParameter)))))
					return Invalid("artifact.material.range", "TMAT version, header, or parameter tables are invalid.");
				return Valid();
			}
			if(request.kind == ArtifactKind::Texture) {
				// Mirror the TETF wire envelope before any descriptor/chunk access.
				if(!bytes.Range(0, sizeof(TETFHeader))) return Invalid("artifact.texture.header", "TETF header is truncated.");
				TETFHeader header{}; std::memcpy(&header, request.bytes.data(), sizeof(header));
				if(header.magic != kMagic || header.version != kVersion || header.headerSize < sizeof(header) || header.totalSize != request.bytes.size()
					|| header.tocOffset < header.headerSize || !bytes.Range(header.tocOffset, std::uint64_t(header.chunkCount) * sizeof(TETFChunkEntry)))
					return Invalid("artifact.texture.range", "TETF header or chunk table is invalid.");
				std::optional<TETFChunkEntry> descriptor, payload;
				const auto payloadBase = header.tocOffset + std::uint64_t(header.chunkCount) * sizeof(TETFChunkEntry);
				for(std::uint64_t i = 0; i < header.chunkCount; ++i) {
					TETFChunkEntry entry{}; std::memcpy(&entry, request.bytes.data() + header.tocOffset + i * sizeof(entry), sizeof(entry));
					// Runtime maps chunks only from storage following the entire table.
					if(entry.offset < payloadBase || !bytes.Range(entry.offset, entry.size)) return Invalid("artifact.texture.range", "TETF chunk extends outside payload storage.");
					if(entry.id == kChunkDESC && !descriptor) descriptor = entry;
					if(entry.id == kChunkUAST && !payload) payload = entry;
				}
				if(!descriptor || descriptor->size < sizeof(TETFDesc) || !payload || !payload->size)
					return Invalid("artifact.texture.chunks", "TETF descriptor or UAST payload is missing.");
				TETFDesc desc{}; std::memcpy(&desc, request.bytes.data() + descriptor->offset, sizeof(desc));
				const auto tableSize = std::uint64_t(desc.mipCount) * sizeof(TETFLevelEntry);
				if(tableSize > descriptor->size - sizeof(desc)) return Invalid("artifact.texture.mip_table", "TETF descriptor contains a truncated mip table.");
				std::uint64_t requiredBlocks = 0;
				for(unsigned mip = 0; mip < desc.mipCount; ++mip) {
					TETFLevelEntry level{};
					std::memcpy(&level, request.bytes.data() + descriptor->offset + sizeof(desc) + mip * sizeof(level), sizeof(level));
					if(level.offset > payload->size || level.size > payload->size - level.offset)
						return Invalid("artifact.texture.mip_range", "TETF mip extends outside its UAST payload.");
					// Match runtime's sequential 4x4 UAST block consumption without
					// invoking the transcoder. Saturate dimensions before shifting.
					const auto width = mip < 16 ? std::max(1U, unsigned(desc.width) >> mip) : 1U;
					const auto height = mip < 16 ? std::max(1U, unsigned(desc.height) >> mip) : 1U;
					requiredBlocks += std::uint64_t((width + 3U) / 4U) * ((height + 3U) / 4U);
					if(requiredBlocks > payload->size / 16U) return Invalid("artifact.texture.payload", "TETF UAST payload cannot supply its declared mip dimensions.");
				}
				return Valid();
			}
			// JSON remains authored data: arbitrary strings are legal; only declared
			// structure/version contracts can reject it. Existing richer decoders
			// and manifest identity checks still run at their production gates.
			const auto* jsonBytes = reinterpret_cast<const unsigned char*>(request.bytes.data());
			const auto document = nlohmann::json::parse(jsonBytes, jsonBytes + request.bytes.size());
			if(request.kind == ArtifactKind::JsonDocument) return Valid();
			if(!document.is_object()) return Invalid("artifact.json.root", "Typed JSON artifact must have an object root.");
			if(request.kind == ArtifactKind::Scene) {
				// Reuse the pure format gate used by production scene loading;
				// validation does not prepare objects or access live scene state.
				std::string diagnostic;
				if(!Serializer::ValidateUnboundedSceneDocument(document, &diagnostic))
					return Invalid("artifact.scene.envelope", std::move(diagnostic));
			}
			if(request.kind == ArtifactKind::PhysicsMaterial) {
				Physics::PhysicsMaterial material; Physics::PhysicsMaterialDiagnostics diagnostics;
				if(!Physics::PhysicsMaterial::LoadJson(document, material, diagnostics)) return Invalid("artifact.physics_material.invalid", "Physics material fails its schema/value contract.");
			}
			if(request.kind == ArtifactKind::PhysicsSettings) {
				if(document.value("schemaVersion", Physics::kPhysicsSettingsSchemaVersion) != Physics::kPhysicsSettingsSchemaVersion
					|| !document.contains("layers") || !document["layers"].is_array() || document["layers"].size() != Physics::kPhysicsCollisionLayerCount
					|| !document.contains("gravity") || !document["gravity"].is_array() || document["gravity"].size() != 3)
					return Invalid("artifact.physics_settings.invalid", "Physics settings schema, gravity, or layer count is invalid.");
				Physics::PhysicsSettings settings = Physics::PhysicsSettings::Defaults();
				settings.gravity = {document["gravity"][0].get<float>(), document["gravity"][1].get<float>(), document["gravity"][2].get<float>()};
				settings.fixedTimeStep = document.value("fixedTimeStep", settings.fixedTimeStep);
				settings.maxSubsteps = document.value("maxSubsteps", settings.maxSubsteps);
				settings.maxAccumulatedTime = document.value("maxAccumulatedTime", settings.maxAccumulatedTime);
				settings.velocityIterations = document.value("velocityIterations", settings.velocityIterations);
				settings.positionIterations = document.value("positionIterations", settings.positionIterations);
				settings.maxCastDistance = document.value("maxCastDistance", settings.maxCastDistance);
				settings.maxShapeExtent = document.value("maxShapeExtent", settings.maxShapeExtent);
				settings.maxHitsPerQuery = document.value("maxHitsPerQuery", settings.maxHitsPerQuery);
				settings.maxRequestsPerBatch = document.value("maxRequestsPerBatch", settings.maxRequestsPerBatch);
				settings.retainedResultTicks = document.value("retainedResultTicks", settings.retainedResultTicks);
				settings.debugVisualizationEnabled = document.value("debugVisualizationEnabled", settings.debugVisualizationEnabled);
				settings.retainedDebugTicks = document.value("retainedDebugTicks", settings.retainedDebugTicks);
				settings.maxDebugPrimitives = document.value("maxDebugPrimitives", settings.maxDebugPrimitives);
				for(std::size_t i = 0; i < settings.layers.size(); ++i) {
					const auto& source = document["layers"][i];
					if(!source.is_object()) return Invalid("artifact.physics_settings.layer", "Physics layer must be an object.");
					auto& layer = settings.layers[i];
					layer.id = source.value("id", layer.id); layer.name = source.value("name", layer.name);
					layer.collisionMask = source.value("collisionMask", layer.collisionMask);
					layer.broadPhaseGroup = source.value("broadPhaseGroup", layer.broadPhaseGroup);
					layer.enabled = source.value("enabled", layer.enabled);
				}
				const auto validation = settings.Validate();
				if(!validation.accepted) return Invalid("artifact.physics_settings.value", validation.message);
			}
			return Valid();
		}
	}

	NativeImageTarget HostNativeImageTarget() noexcept {
#ifdef _WIN32
		return NativeImageTarget::WindowsX64;
#elif defined(__linux__)
		return NativeImageTarget::LinuxX64;
#else
		return NativeImageTarget::Unsupported;
#endif
	}
	ArtifactKind InferArtifactKind(std::string_view logicalPath) {
		std::string path(logicalPath);
		std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if(path.ends_with("assembly-csharp.dll") || path.ends_with("assembly-csharp.so")) return ArtifactKind::NativeGameplay;
		if(path.ends_with("physicssettings.json")) return ArtifactKind::PhysicsSettings;
		if(path.ends_with(".tephysmat")) return ArtifactKind::PhysicsMaterial;
		if(path.ends_with(".tepcoll")) return ArtifactKind::CookedCollider;
		if(path.ends_with(".temf")) return ArtifactKind::Mesh;
		if(path.ends_with(".tetf")) return ArtifactKind::Texture;
		if(path.ends_with(".tmat")) return ArtifactKind::Material;
		if(path.ends_with(".tescene")) return ArtifactKind::Scene;
		if(path.ends_with(".json")) return ArtifactKind::JsonDocument;
		return ArtifactKind::AuthoredData;
	}
	ArtifactValidationResult ValidateArtifact(const ArtifactValidationRequest& request) {
		ArtifactValidationResult result;
		try { result = InspectKnown(request); }
		catch(const std::exception& exception) { result = Invalid("artifact.decode_failed", exception.what()); }
		result.affectedPath = request.logicalPath;
		return result;
	}
}
