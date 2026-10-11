// Full native readbacks become portable evidence only after their real queue
// receipt completes. Files and JSON are written on detached CPU export jobs.
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyEvidenceExport.h"
#include <windows.h>
#include <bcrypt.h>
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include "Renderer/RenderCaptureEncoding.h"
#include "Renderer/RHI/Vulkan/VulkanReadbackConversion.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using Json = nlohmann::json;
		using namespace Renderer;
		constexpr std::uint64_t kMaximumExactJsonInteger = 9007199254740991ULL;
		constexpr std::size_t kMaximumArtifactBytes = 512ULL * 1024 * 1024;
		void Require(bool condition, std::string_view message) { if(!condition) throw std::runtime_error(std::string(message)); }
		Json Valid(const Json& value) { return {{"status", "Valid"}, {"value", value}, {"reason", ""}}; }
		Json Unavailable(std::string reason) { return {{"status", "Unavailable"}, {"value", nullptr}, {"reason", std::move(reason)}}; }
		Json Number(std::uint64_t value) { Require(value <= kMaximumExactJsonInteger, "Evidence integer exceeds exact JSON range."); return Valid(value); }
		Json Counter(const RendererEfficiencyCounter& value)
		{
			if(value.status == RendererEfficiencyEvidenceStatus::Valid && value.value) return Number(*value.value);
			return Unavailable(value.reason.empty() ? "Native counter was not published." : value.reason);
		}
		Json Text(const std::string& value) { return value.empty() ? Unavailable("Native text was not published.") : Valid(value); }
		Json Timing(const RendererEfficiencyStageTiming& value)
		{
			if(value.status != RendererEfficiencyEvidenceStatus::Valid || !value.durationMilliseconds)
				return {{"status", value.status == RendererEfficiencyEvidenceStatus::Pending ? "Pending" : "Unavailable"}, {"value", nullptr},
					{"reason", value.unavailableReason.empty() ? "No completed native timing sample." : value.unavailableReason}};
			const double nanoseconds = *value.durationMilliseconds * 1000000.0;
			Require(std::isfinite(nanoseconds) && nanoseconds >= 0, "Native timing sample is invalid."); return Valid(nanoseconds);
		}
		Json Options(const RendererEfficiencyOptions& value)
		{
			return {{"shadowReuse", value.shadowReuse == RendererEfficiencyShadowReuse::Enabled ? "Enabled" : "AlwaysRefresh"},
				{"lightingUploads", value.lightingUploads == RendererEfficiencyLightingUploads::Dirty ? "Dirty" : "Full"},
				{"lightingDepth", value.lightingDepth == RendererEfficiencyLightingDepth::Cooperative ? "Cooperative" : "CompleteScan"},
				{"shadowSubmission", value.shadowSubmission == RendererEfficiencyShadowSubmission::Grouped ? "Grouped" : "Singleton"},
				{"staticShadowCacheBudgetBytes", value.staticShadowCacheBudgetBytes},
				{"validationPreparation", value.validationPreparation == RendererEfficiencyPreparation::Concurrent ? "Concurrent" : "Serial"}};
		}
		std::wstring PathKey(const std::filesystem::path& path)
		{
			auto value = path.lexically_normal().generic_wstring();
			std::transform(value.begin(), value.end(), value.begin(), [](auto character) { return std::towlower(character); }); return value;
		}
		bool Within(const std::filesystem::path& path, const std::filesystem::path& root)
		{
			auto part = path.begin();
			for(const auto& expected : root) { if(part == path.end() || PathKey(*part) != PathKey(expected)) return false; ++part; }
			return true;
		}
		std::wstring NativePath(const std::filesystem::path& path)
		{
			// Content-addressed names can exceed MAX_PATH in disposable evidence
			// roots. The extended spelling is IO-only; receipts retain portable paths.
			auto value = path.native();
			if(!path.is_absolute() || value.starts_with(L"\\\\?\\")) return value;
			if(value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
			return L"\\\\?\\" + value;
		}
		void NoRedirects(const std::filesystem::path& path)
		{
			std::filesystem::path current;
			for(const auto& part : path)
			{
				current /= part;
				const auto attributes = GetFileAttributesW(NativePath(current).c_str());
				if(attributes == INVALID_FILE_ATTRIBUTES) { Require(!std::filesystem::exists(current), "Evidence path attributes unavailable."); continue; }
				Require((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0, "Evidence path redirects through a symlink/junction.");
			}
		}
		struct Algorithm
		{
			BCRYPT_ALG_HANDLE value = nullptr;
			~Algorithm() { if(value) BCryptCloseAlgorithmProvider(value, 0); }
		};
		struct Hash
		{
			BCRYPT_HASH_HANDLE value = nullptr;
			~Hash() { if(value) BCryptDestroyHash(value); }
		};
		std::string Sha256(std::span<const std::uint8_t> bytes)
		{
			Require(bytes.size() <= kMaximumArtifactBytes, "Evidence artifact exceeds bounded CPU export size.");
			Algorithm algorithm; std::vector<std::uint8_t> object; Hash hash; ULONG size = 0, returned = 0;
			Require(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA-256 provider unavailable.");
			Require(BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &returned, 0) >= 0, "SHA-256 object size unavailable.");
			// Hash destruction must precede releasing the caller-owned CNG object
			// buffer; declaration order above preserves that borrowed-storage lifetime.
			object.resize(size); std::array<std::uint8_t, 32> digest{};
			Require(BCryptCreateHash(algorithm.value, &hash.value, object.data(), size, nullptr, 0, 0) >= 0, "SHA-256 initialization failed.");
			Require(BCryptHashData(hash.value, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0
				&& BCryptFinishHash(hash.value, digest.data(), digest.size(), 0) >= 0, "SHA-256 hashing failed.");
			constexpr std::string_view alphabet = "0123456789abcdef"; std::string result;
			for(auto byte : digest) { result += alphabet[byte >> 4]; result += alphabet[byte & 15]; } return result;
		}
		std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
		{
			NoRedirects(path); const std::filesystem::path ioPath = NativePath(path); const auto size = std::filesystem::file_size(ioPath);
			Require(size <= kMaximumArtifactBytes, "Existing evidence file is oversized.");
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size)); std::ifstream input(ioPath, std::ios::binary);
			input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); Require(input.good(), "Existing evidence file cannot be read fully."); return bytes;
		}
		Json WriteArtifact(const std::filesystem::path& project, const std::filesystem::path& root,
			std::string kind, std::string extension, std::span<const std::uint8_t> bytes)
		{
			const auto digest = Sha256(bytes);
			const auto destination = root / (digest + extension);
			NoRedirects(root); NoRedirects(destination);
			if(std::filesystem::exists(std::filesystem::path(NativePath(destination)))) Require(Sha256(ReadBytes(destination)) == digest, "Content-addressed artifact was altered.");
			else
			{
				static std::atomic_uint64_t sequence{0};
				const auto temporary = root / (digest + ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++sequence));
				HANDLE file = CreateFileW(NativePath(temporary).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
				Require(file != INVALID_HANDLE_VALUE, "Cannot exclusively create owned evidence staging file.");
				bool okay = true; std::size_t offset = 0;
				while(offset < bytes.size())
				{
					DWORD written = 0; const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
					if(!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written != count) { okay = false; break; } offset += written;
				}
				okay = okay && FlushFileBuffers(file); CloseHandle(file);
				if(!okay) { DeleteFileW(NativePath(temporary).c_str()); throw std::runtime_error("Owned evidence staging write failed."); }
				NoRedirects(root); NoRedirects(destination);
				if(!MoveFileExW(NativePath(temporary).c_str(), NativePath(destination).c_str(), MOVEFILE_WRITE_THROUGH))
				{
					const bool existingMatches = std::filesystem::exists(std::filesystem::path(NativePath(destination))) && Sha256(ReadBytes(destination)) == digest;
					DeleteFileW(NativePath(temporary).c_str()); Require(existingMatches, "Atomic evidence publication failed.");
				}
			}
			return {{"kind", kind}, {"status", "Valid"}, {"path", std::filesystem::relative(destination, project).generic_string()}, {"sha256", digest}, {"reason", ""}};
		}
		Json Origin(const ShadowCanonicalOrigin& value)
		{
			return {{"cell", {std::to_string(value.cell[0]), std::to_string(value.cell[1]), std::to_string(value.cell[2])}}, {"offsetMetres", value.offsetMetres}};
		}
		Json Stamp(const ShadowCasterStamp& value)
		{
			Json texturePublications = Json::array();
			for (const auto& texture : value.texturePublications) texturePublications.push_back({
				{"textureId", ToHexString(texture.textureId)}, {"publicationGeneration", std::to_string(texture.publicationGeneration)}, {"bindingIndex", texture.bindingIndex}});
			// Decimal strings preserve every native 64-bit semantic generation/range
			// outside C4's deliberately bounded numeric metrics.
			return {{"objectId", ToHexString(value.objectId)}, {"lifetimeGeneration", std::to_string(value.lifetimeGeneration)}, {"segmentId", std::to_string(value.segmentId)},
				{"meshId", ToHexString(value.meshId)}, {"assetId", ToHexString(value.assetId)}, {"materialId", ToHexString(value.materialId)}, {"shaderId", ToHexString(value.shaderId)}, {"textureId", ToHexString(value.textureId)},
				{"meshRevision", std::to_string(value.meshRevision)}, {"assetRevision", std::to_string(value.assetRevision)}, {"materialRevision", std::to_string(value.materialRevision)}, {"shaderRevision", std::to_string(value.shaderRevision)}, {"textureRevision", std::to_string(value.textureRevision)},
				{"worldPosition", Origin(value.worldPosition)}, {"referenceOrigin", Origin(value.referenceOrigin)}, {"rotation", value.rotation}, {"scale", value.scale}, {"shadowBehavior", std::to_string(value.shadowBehavior)}, {"materialShadowParameters", value.materialShadowParameters}, {"texturePublications", texturePublications},
				{"animationInstanceId", std::to_string(value.animationInstanceId)}, {"poseGeneration", std::to_string(value.poseGeneration)}, {"morphGeneration", std::to_string(value.morphGeneration)}, {"paletteGeneration", std::to_string(value.paletteGeneration)},
				{"deformationGeneration", std::to_string(value.deformationGeneration)}, {"deformationProgramGeneration", std::to_string(value.deformationProgramGeneration)}, {"deformationContentIdLow", std::to_string(value.deformationContentIdLow)}, {"deformationContentIdHigh", std::to_string(value.deformationContentIdHigh)},
				{"deformationPalette", value.deformationPalette}, {"morphWeights", value.morphWeights}, {"geometryRoute", value.geometryRoute}, {"selectedLod", value.selectedLod}, {"selectionFlags", value.selectionFlags},
				{"firstMeshlet", std::to_string(value.firstMeshlet)}, {"meshletCount", std::to_string(value.meshletCount)}, {"firstCluster", std::to_string(value.firstCluster)}, {"clusterCount", std::to_string(value.clusterCount)},
				{"pageId", std::to_string(value.pageId)}, {"fallbackPageId", std::to_string(value.fallbackPageId)}, {"pageContentRevision", std::to_string(value.pageContentRevision)}, {"residencyGeneration", std::to_string(value.residencyGeneration)},
				{"payloadContentRevision", std::to_string(value.payloadContentRevision)}, {"payloadByteOffset", std::to_string(value.payloadByteOffset)}, {"payloadByteSize", std::to_string(value.payloadByteSize)}, {"payloadSource", value.payloadSource}, {"emittedSource", value.emittedSource},
				{"staticEligible", value.staticEligible}, {"staticClassificationReason", value.staticClassificationReason},
				{"staticSupport", {{"authoredStatic", value.staticSupport.authoredStatic}, {"knownDefaultShadowAbi", value.staticSupport.knownDefaultShadowAbi},
					{"ownerPublicationsComplete", value.staticSupport.ownerPublicationsComplete}, {"materialShadowSupported", value.staticSupport.materialShadowSupported},
					{"geometrySupported", value.staticSupport.geometrySupported}, {"deformationAbsent", value.staticSupport.deformationAbsent}}}, {"evidenceMask", std::to_string(value.evidenceMask)}};
		}
		Json Key(const ShadowViewKey& key)
		{
			return {{"rendererSessionGeneration", std::to_string(key.rendererSessionGeneration)}, {"viewType", key.viewType == RenderViewType::Scene ? "Scene" : "Game"}, {"viewId", key.viewId},
				{"referenceOrigin", Origin(key.referenceOrigin)}, {"lightId", ToHexString(key.lightId)}, {"resourceKind", static_cast<std::uint32_t>(key.resourceKind)}, {"faceIndex", key.faceIndex},
				{"allocationGeneration", std::to_string(key.allocationGeneration)}, {"regionId", std::to_string(key.regionId)}, {"extent", key.extent}, {"depthFormat", key.depthFormat},
				{"lightProjectionStamp", {{"viewProjection", key.lightProjectionStamp.viewProjection}, {"lightParameters", key.lightProjectionStamp.lightParameters}, {"shadowFilterParameters", key.lightProjectionStamp.shadowFilterParameters},
					{"semanticRevision", std::to_string(key.lightProjectionStamp.semanticRevision)}, {"cubeViewProjections", key.lightProjectionStamp.cubeViewProjections}, {"cubeProjectionEvidenceComplete", key.lightProjectionStamp.cubeProjectionEvidenceComplete}}}};
		}
		void ValidateLightingUpload(const RendererEfficiencyLightingUploadEvidence& upload, bool analysis)
		{
			constexpr std::uint64_t blockBytes = 176ULL * 1024;
			Require(upload.available && upload.succeeded && upload.analysisPurpose == analysis
				&& upload.allocationId && upload.allocationGeneration && upload.frameSlot < 3 && upload.snapshotSlot < 8
				&& upload.activeRecordCount <= 1024, "Lighting lacks successful exact physical upload ownership.");
			Require(upload.regionSizeBytes == blockBytes && upload.regionOffsetBytes == upload.snapshotSlot * blockBytes
				&& upload.allocationSizeBytes >= upload.regionOffsetBytes && blockBytes <= upload.allocationSizeBytes - upload.regionOffsetBytes
				&& upload.allocationSizeBytes <= kMaximumExactJsonInteger && upload.nonCoherentAtomSize
				&& (upload.nonCoherentAtomSize & (upload.nonCoherentAtomSize - 1)) == 0,
				"Lighting lacks bounded native allocation/atom ownership.");
			Require(upload.payloadLightIds.size() == upload.activeRecordCount, "Lighting lacks exact selected native record identities.");
			std::set<std::string> lightIds;
			for(const auto& id : upload.payloadLightIds) Require(id != EMPTY_UUID && lightIds.insert(ToHexString(id)).second,
				"Lighting selected native identities are empty or duplicated.");
			const auto regionStart = upload.regionOffsetBytes;
			std::uint64_t bytes = 0, previousEnd = regionStart;
			for (const auto& range : upload.logicalWriteRanges)
			{
				Require(range[1] && range[0] >= previousEnd && range[0] >= regionStart && range[0] <= regionStart + blockBytes
					&& range[1] <= regionStart + blockBytes - range[0] && (range[0] - regionStart) % 176 == 0 && range[1] % 176 == 0,
					"Lighting logical write range escapes its complete physical record slice.");
				previousEnd = range[0] + range[1]; bytes += range[1];
			}
			Require(bytes == upload.logicalWriteBytes && bytes / 176 == upload.logicalWriteRecords,
				"Lighting logical counters differ from actual typed write ranges.");
			bytes = 0; previousEnd = 0;
			for (const auto& range : upload.physicalFlushRanges)
			{
				Require(range[1] && range[0] >= previousEnd && range[0] <= kMaximumExactJsonInteger && range[1] <= kMaximumArtifactBytes
					&& range[1] <= kMaximumExactJsonInteger - range[0] && bytes <= kMaximumExactJsonInteger - range[1]
					&& range[0] <= upload.allocationSizeBytes && range[1] <= upload.allocationSizeBytes - range[0]
					&& range[0] % upload.nonCoherentAtomSize == 0
					&& (range[1] % upload.nonCoherentAtomSize == 0 || range[0] + range[1] == upload.allocationSizeBytes),
					"Lighting physical flush range is invalid or overflows.");
				previousEnd = range[0] + range[1]; bytes += range[1];
			}
			Require(bytes == upload.physicalFlushBytes && (!upload.coherent || upload.physicalFlushRanges.empty()),
				"Lighting flush counters differ from actual ranges or invent coherent flushes.");
			if (!upload.coherent)
				for (const auto& write : upload.logicalWriteRanges)
					Require(std::any_of(upload.physicalFlushRanges.begin(), upload.physicalFlushRanges.end(), [&](const auto& flush) {
						return flush[0] <= write[0] && flush[0] + flush[1] >= write[0] + write[1];
					}), "Noncoherent lighting write lacks its successful covering flush range.");
		}
		Json LightingRanges(const std::vector<std::array<std::uint64_t, 2>>& ranges)
		{
			Json result = Json::array();
			for(const auto& range : ranges) result.push_back({{"offsetBytes",range[0]},{"lengthBytes",range[1]}});
			return result;
		}
		Json LightingRows(const RendererEfficiencyEvidence& row, bool raw)
		{
			Json result = Json::array();
			for (unsigned index = 0; index < 2; ++index)
			{
				const auto& upload = row.lightingUploads[index];
				Json payloadLightIds = Json::array(); for(const auto& id : upload.payloadLightIds) payloadLightIds.push_back(ToHexString(id));
				const auto unavailable = Unavailable(upload.reason.empty() ? "This purpose has no successful native lighting upload receipt." : upload.reason);
				if (upload.available) ValidateLightingUpload(upload, index != 0);
				if (raw)
				{
					result.push_back({{"purpose",index ? "Analysis" : "FinalLights"},{"available",upload.available},{"succeeded",upload.succeeded},
						{"coherent",upload.coherent},{"analysisPurpose",upload.analysisPurpose},{"allocationId",std::to_string(upload.allocationId)},
						{"allocationGeneration",std::to_string(upload.allocationGeneration)},{"allocationSizeBytes",upload.allocationSizeBytes},{"regionOffsetBytes",upload.regionOffsetBytes},{"regionSizeBytes",upload.regionSizeBytes},{"nonCoherentAtomSize",upload.nonCoherentAtomSize},{"frameSlot",upload.frameSlot},{"snapshotSlot",upload.snapshotSlot},
						{"payloadLightIds",payloadLightIds},{"activeRecordCount",upload.activeRecordCount},{"logicalWriteBytes",upload.logicalWriteBytes},{"logicalWriteRecords",upload.logicalWriteRecords},
						{"logicalWriteRanges",upload.logicalWriteRanges},{"physicalFlushBytes",upload.physicalFlushBytes},{"physicalFlushRanges",upload.physicalFlushRanges},
						{"mirrorCpuDurationNanoseconds",upload.mirrorCpuDurationNanoseconds},{"reason",upload.reason}});
					continue;
				}
				const bool available = upload.available && upload.succeeded;
				result.push_back({{"purpose",index ? "Analysis" : "FinalLights"},{"allocationId",available ? Number(upload.allocationId) : unavailable},
					{"allocationGeneration",available ? Number(upload.allocationGeneration) : unavailable},{"allocationSizeBytes",available ? Number(upload.allocationSizeBytes) : unavailable},{"regionOffsetBytes",available ? Number(upload.regionOffsetBytes) : unavailable},{"regionSizeBytes",available ? Number(upload.regionSizeBytes) : unavailable},{"nonCoherentAtomSize",available ? Number(upload.nonCoherentAtomSize) : unavailable},{"coherent",upload.coherent},{"frameSlot",available ? Number(upload.frameSlot) : unavailable},
					{"snapshotSlot",available ? Number(upload.snapshotSlot) : unavailable},{"payloadLightIds",available ? Valid(payloadLightIds) : unavailable},{"activeRecordCount",available ? Number(upload.activeRecordCount) : unavailable},
					{"logicalWriteBytes",available ? Number(upload.logicalWriteBytes) : unavailable},{"logicalWriteRecords",available ? Number(upload.logicalWriteRecords) : unavailable},
					{"logicalWriteRanges",available ? Valid(LightingRanges(upload.logicalWriteRanges)) : unavailable},{"physicalFlushBytes",available ? Number(upload.physicalFlushBytes) : unavailable},
					{"physicalFlushRanges",available ? Valid(LightingRanges(upload.physicalFlushRanges)) : unavailable},{"mirrorCpuDurationNanoseconds",available ? Number(upload.mirrorCpuDurationNanoseconds) : unavailable},
					{"hostWriteStatus",available ? "Succeeded" : "Unavailable"},{"reason",available ? "" : upload.reason}});
			}
			return result;
		}
		namespace DepthTiming = VulkanLightingDepthTiming;
		const char* DepthStatus(DepthTiming::Status value)
		{
			switch(value)
			{
			case DepthTiming::Status::Pending: return "Pending";
			case DepthTiming::Status::Unavailable: return "Unavailable";
			case DepthTiming::Status::Valid: return "Valid";
			}
			throw std::runtime_error("Unknown depth query status.");
		}
		const char* DepthReason(DepthTiming::Reason value)
		{
			switch(value)
			{
			case DepthTiming::Reason::None: return "None";
			case DepthTiming::Reason::MissingMarkers: return "MissingMarkers";
			case DepthTiming::Reason::ReadFailed: return "ReadFailed";
			case DepthTiming::Reason::MissingAvailability: return "MissingAvailability";
			case DepthTiming::Reason::ZeroNonemptyDuration: return "ZeroNonemptyDuration";
			case DepthTiming::Reason::InvalidDuration: return "InvalidDuration";
			case DepthTiming::Reason::Canceled: return "Canceled";
			case DepthTiming::Reason::Abandoned: return "Abandoned";
			}
			throw std::runtime_error("Unknown depth query reason.");
		}
		std::uint32_t DepthWord(std::span<const std::uint8_t> bytes,std::size_t offset)
		{
			return std::uint32_t(bytes[offset])|(std::uint32_t(bytes[offset+1])<<8)|(std::uint32_t(bytes[offset+2])<<16)|(std::uint32_t(bytes[offset+3])<<24);
		}
		bool ValidateDepthBounds(std::span<const std::uint8_t> bytes)
		{
			Require(bytes.size() == 144 * 8, "Depth bounds must contain all 144 native float2 records.");
			bool nonempty = false;
			for(std::size_t i = 0; i < 144; ++i)
			{
				const float minimum = std::bit_cast<float>(DepthWord(bytes, i * 8));
				const float maximum = std::bit_cast<float>(DepthWord(bytes, i * 8 + 4));
				Require(std::isfinite(minimum) && std::isfinite(maximum) && minimum >= 0 && minimum <= maximum && maximum <= 1
					&& ((minimum == 1 && maximum == 1) || maximum < 1), "Depth bounds contain malformed or nonfinite native extrema.");
				nonempty |= minimum < 1;
			}
			return nonempty;
		}
		void ValidateDepthQuery(const RendererEfficiencyEvidence& row,const RendererEfficiencyDepthQueryEvidence& query,DepthTiming::Purpose purpose,bool nonempty)
		{
			const auto& r = query.result;
			const auto& o = r.owner;
			DepthStatus(r.status);
			DepthReason(r.reason);
			// Even unavailable results must belong to the actual completed command.
			Require(o.rendererSessionGeneration == row.effectiveOptions.rendererSessionGeneration && o.backendResourceGeneration && o.queryPoolGeneration
				&& o.frameSerial == row.frameSerial && o.commandSerial == row.commandSerial && o.frameSlot < 3 && o.snapshotSlot < 8 && o.outputId == row.viewId
				&& o.view == (row.viewType == RenderViewType::Scene ? DepthTiming::View::Scene : DepthTiming::View::Game)
				&& o.purpose == purpose && o.scope == DepthTiming::Scope::IsolatedDepth
				&& r.submitSerial == row.submitSerial && r.completionSerial == row.completionSerial,
				"Depth query differs from its completed physical view/command/session owner.");
			const auto first = ((o.frameSlot * 8 + o.snapshotSlot) * 2 + static_cast<unsigned>(purpose)) * 2;
			Require(query.range.firstQuery == first && query.range.count == 2, "Depth query range differs from its exact frame/snapshot/purpose pair.");
			const bool requestedCooperative = row.effectiveOptions.options.lightingDepth == RendererEfficiencyLightingDepth::Cooperative;
			// A real unsupported cooperative pipeline may execute the reference.
			// Keep requested options and actual mode separate, with its native reason.
			Require(query.cooperative == requestedCooperative || (requestedCooperative && !query.cooperative && !row.fallbackReason.empty()),
				"Depth query mode differs from captured options without an actual fallback reason.");
			// Configuration metadata is real even when availability/read fails. Zero
			// flags/stages mean no call was made; other values must name these calls.
			Require(r.timestampValidBits && r.timestampValidBits <= 64 && std::isfinite(r.timestampPeriodNanoseconds)
				&& r.timestampPeriodNanoseconds > 0 && std::isfinite(r.durationNanoseconds) && r.durationNanoseconds >= 0,
				"Depth query lacks valid configured timestamp metadata.");
			Require((query.queryReadFlags == 0 || query.queryReadFlags == 5)
				&& (query.beginStageMask == 0 || query.beginStageMask == 2048) && (query.endStageMask == 0 || query.endStageMask == 2048)
				&& query.noWaitRead == (query.queryReadFlags == 5)
				&& query.computeStageMarkers == (query.beginStageMask == 2048 && query.endStageMask == 2048),
				"Depth query flags/stages contradict the captured isolated no-WAIT calls.");
			if(r.status == DepthTiming::Status::Valid)
			{
				Require(r.reason == DepthTiming::Reason::None && r.beginAvailable && r.endAvailable
					&& query.computeStageMarkers && query.noWaitRead && query.queryReadFlags == 5 && query.beginStageMask == 2048 && query.endStageMask == 2048,
					"Valid depth timing lacks actual available no-WAIT isolated compute markers.");
				const auto mask = r.timestampValidBits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << r.timestampValidBits) - 1;
				const auto ticks = (r.endTicks - r.beginTicks) & mask;
				Require(r.rawTicks == ticks && r.durationNanoseconds == static_cast<double>(ticks) * r.timestampPeriodNanoseconds
					&& r.nonemptyWork == nonempty && (!nonempty || ticks), "Depth raw masked rollover/duration/nonempty proof is inconsistent.");
			}
			else Require(r.reason != DepthTiming::Reason::None, "Unavailable or pending depth timing needs its actual reason.");
		}
		Json DepthQuery(const RendererEfficiencyDepthQueryEvidence& query)
		{
			const auto& r = query.result;
			const auto& o = r.owner;
			// Decimal strings preserve all native uint64 values; the duration keeps
			// its original double precision instead of passing through milliseconds.
			const Json owner = {{"rendererSessionGeneration",std::to_string(o.rendererSessionGeneration)},{"backendResourceGeneration",std::to_string(o.backendResourceGeneration)},{"queryPoolGeneration",std::to_string(o.queryPoolGeneration)},
				{"frameSerial",std::to_string(o.frameSerial)},{"commandSerial",std::to_string(o.commandSerial)},{"frameSlot",o.frameSlot},{"snapshotSlot",o.snapshotSlot},{"outputId",o.outputId},
				{"purpose",o.purpose==DepthTiming::Purpose::Production?"Production":"Oracle"},{"view",o.view==DepthTiming::View::Scene?"Scene":"Game"},{"scope",o.scope==DepthTiming::Scope::IsolatedDepth?"IsolatedDepth":"WholeView"}};
			return {{"owner",owner},
				{"range",{{"firstQuery",query.range.firstQuery},{"count",query.range.count}}},{"status",DepthStatus(r.status)},{"reason",DepthReason(r.reason)},
				{"beginTicks",std::to_string(r.beginTicks)},{"endTicks",std::to_string(r.endTicks)},{"rawTicks",std::to_string(r.rawTicks)},{"submitSerial",std::to_string(r.submitSerial)},{"completionSerial",std::to_string(r.completionSerial)},
				{"timestampValidBits",r.timestampValidBits},{"timestampPeriodNanoseconds",r.timestampPeriodNanoseconds},{"durationNanoseconds",r.durationNanoseconds},
				{"beginAvailable",r.beginAvailable},{"endAvailable",r.endAvailable},{"nonemptyWork",r.nonemptyWork},{"cooperative",query.cooperative},{"computeStageMarkers",query.computeStageMarkers},{"noWaitRead",query.noWaitRead},
				{"queryReadFlags",query.queryReadFlags},{"beginStageMask",query.beginStageMask},{"endStageMask",query.endStageMask}};
		}
		Json DepthRecipe(const LightingDepthReduction::DepthRecipe& recipe)
		{
			// Bit patterns support NaN/Inf requests without invalid JSON numbers.
			Json overrides = Json::array();
			for(const auto& pixel : recipe.overrides)
			{
				overrides.push_back({{"x",pixel.x},{"y",pixel.y},{"bits",pixel.bits}});
			}
			return {{"width",recipe.extent.width},{"height",recipe.extent.height},{"fillBits",recipe.fillBits},{"overrides",overrides}};
		}
		void ValidateLightingDepth(const RendererEfficiencyEvidence& row)
		{
			Require(row.artifacts->lightingDepth && row.fixtureControlGeneration && row.effectiveOptions.optionsSessionGeneration
				&& (row.viewType == RenderViewType::Scene || row.viewType == RenderViewType::Game), "Depth lacks current owned payload/control/options identity.");
			const auto& d = *row.artifacts->lightingDepth;
			const auto& color = *row.artifacts->frameColor;
			const auto bpp = VulkanReadback::BytesPerPixel(static_cast<VkFormat>(color.sourceFormat));
			// A copied payload cannot silently drift from the actual recorded view.
			Require(d.productionWidth && d.productionHeight && row.resolution.x == d.productionWidth && row.resolution.y == d.productionHeight
				&& color.width == d.productionWidth && color.height == d.productionHeight && bpp
				&& std::uint64_t(color.width) * color.height <= kMaximumArtifactBytes / bpp
				&& color.bytes.size() == std::uint64_t(color.width) * color.height * bpp, "Depth lacks full same-command color at its actual native extent.");
			ValidateDepthQuery(row, d.productionQuery, DepthTiming::Purpose::Production, ValidateDepthBounds(d.productionBounds));
			LightingRows(row, true);
			// No receipt is invented when lighting was not captured. If it was,
			// both native purposes must be present at this exact physical slot.
			Require(row.lightingUploads[0].available == row.lightingUploads[1].available, "Depth carries only one current lighting upload purpose.");
			for(const auto& upload : row.lightingUploads)
			{
				if(upload.available) Require(upload.frameSlot == d.productionQuery.result.owner.frameSlot
					&& upload.snapshotSlot == d.productionQuery.result.owner.snapshotSlot, "Lighting receipts differ from the actual depth physical frame/snapshot.");
			}
			if(!d.oracleRecipe)
			{
				Require(!d.oracleQuery && d.oracleInput.empty() && d.oracleBounds.empty() && d.oracleCoverage.empty(), "Unrequested oracle bytes or query cannot be exported.");
				return;
			}
			Require(d.oracleQuery.has_value(), "Requested oracle lacks explicit query status/owner.");
			const auto& recipe = *d.oracleRecipe;
			const std::uint64_t pixels = std::uint64_t(recipe.extent.width) * recipe.extent.height;
			Require(recipe.extent.width == d.productionWidth && recipe.extent.height == d.productionHeight
				&& recipe.extent.width <= 1920 && recipe.extent.height <= 1080 && recipe.overrides.size() <= 1024
				&& recipe.overrides.size() <= pixels, "Oracle recipe differs from its bounded admitted native extent.");
			std::map<std::uint64_t, std::uint32_t> overrides;
			for(const auto& pixel : recipe.overrides)
			{
				Require(pixel.x < recipe.extent.width && pixel.y < recipe.extent.height
					&& overrides.emplace(std::uint64_t(pixel.y) * recipe.extent.width + pixel.x, pixel.bits).second, "Oracle recipe has duplicate/outside pixels.");
			}
			const bool payload = !d.oracleInput.empty() || !d.oracleBounds.empty() || !d.oracleCoverage.empty();
			Require(payload || d.oracleQuery->result.status != DepthTiming::Status::Valid, "Valid oracle query lacks completed native payloads.");
			bool nonempty = false;
			if(payload)
			{
				Require(d.oracleInput.size() == pixels * 4 && d.oracleCoverage.size() == 144 * 8, "Oracle lacks full GPU input or actual-load coverage.");
				nonempty = ValidateDepthBounds(d.oracleBounds);
				for(std::uint64_t i = 0; i < pixels; ++i)
				{
					const auto found = overrides.find(i);
					Require(DepthWord(d.oracleInput, i * 4) == (found == overrides.end() ? recipe.fillBits : found->second), "Actual oracle GPU input differs from the exact owned recipe.");
				}
				// Validate observed counts; never replace them with rectangle metadata.
				const float dx = float(recipe.extent.width) / 16, dy = float(recipe.extent.height) / 9;
				for(unsigned y = 0; y < 9; ++y)
				{
					for(unsigned x = 0; x < 16; ++x)
					{
						const auto index = x + y * 16;
						// Parentheses protect these calls from Windows' function-like min macro.
						const auto width = (std::min)(recipe.extent.width, std::uint32_t(float(x + 1) * dx)) - std::uint32_t(float(x) * dx);
						const auto height = (std::min)(recipe.extent.height, std::uint32_t(float(y + 1) * dy)) - std::uint32_t(float(y) * dy);
						const auto visited = DepthWord(d.oracleCoverage, index * 8), eligible = DepthWord(d.oracleCoverage, index * 8 + 4);
						Require(visited == width * height && eligible <= visited, "Oracle actual texture-load coverage is incomplete or invalid.");
						const bool empty = DepthWord(d.oracleBounds, index * 8) == 0x3f800000U && DepthWord(d.oracleBounds, index * 8 + 4) == 0x3f800000U;
						Require(empty == (eligible == 0), "Oracle empty extrema differ from actual eligible texture-load count.");
					}
				}
			}
			ValidateDepthQuery(row, *d.oracleQuery, DepthTiming::Purpose::Oracle, nonempty);
			const auto& p = d.productionQuery.result.owner;
			const auto& o = d.oracleQuery->result.owner;
			Require(p.backendResourceGeneration == o.backendResourceGeneration && p.queryPoolGeneration == o.queryPoolGeneration
				&& p.frameSlot == o.frameSlot && p.snapshotSlot == o.snapshotSlot && d.productionQuery.cooperative == d.oracleQuery->cooperative,
				"Oracle differs from its production physical query pool/slots/actual mode.");
		}
		Json ShadowBatchMember(const RendererEfficiencyShadowBatchMember& member)
		{
			return {{"objectId", ToHexString(member.objectId)}, {"lifetimeGeneration", std::to_string(member.lifetimeGeneration)},
				{"segmentId", std::to_string(member.segmentId)}};
		}
		Json StaticShadowExecution(const RendererEfficiencyStaticShadowExecution& execution)
		{
			Json statics = Json::array(), dynamics = Json::array(), allocations = Json::array(), completed = Json::array(), discarded = Json::array(), accepted = Json::array();
			for (const auto& row : execution.staticMembership) statics.push_back(Stamp(row));
			for (const auto& row : execution.dynamicMembership) dynamics.push_back(Stamp(row));
			for (const auto& owner : execution.allocations) {
				Json uses = Json::array();
				for (const auto& use : owner.uses) uses.push_back({{"commandSerial", std::to_string(use.commandSerial)},
					{"submitSerial", std::to_string(use.submitSerial)}, {"frameSlot", use.frameSlot}});
				allocations.push_back({{"sessionGeneration", std::to_string(owner.sessionGeneration)}, {"backendGeneration", std::to_string(owner.backendGeneration)},
					{"allocationIdentity", std::to_string(owner.allocationIdentity)}, {"allocationGeneration", std::to_string(owner.allocationGeneration)},
					{"actualBytes", std::to_string(owner.actualBytes)}, {"depthFormat", owner.depthFormat}, {"layers", owner.layers},
					{"extent", owner.extent}, {"retiring", owner.retiring}, {"uses", std::move(uses)}});
			}
			for (const auto value : execution.completedSubmissions) completed.push_back(std::to_string(value));
			for (const auto value : execution.discardedCommands) discarded.push_back(std::to_string(value));
			for (const auto& use : execution.acceptedSubmissions) accepted.push_back({{"commandSerial", std::to_string(use.commandSerial)},
				{"submitSerial", std::to_string(use.submitSerial)}, {"frameSlot", use.frameSlot}});
			// The source allocation differs from the final composed shadow image.
			// Preserve its actual command/submit receipt even when content is reused.
			return {{"available", execution.available}, {"partitionComplete", execution.partitionComplete}, {"copyOverlay", execution.copyOverlay},
				{"staticReused", execution.staticReused}, {"recordedComplete", execution.recordedComplete},
				{"requiredFaceMask", execution.requiredFaceMask}, {"staticRecordedFaceMask", execution.staticRecordedFaceMask}, {"copiedFaceMask", execution.copiedFaceMask},
				{"staticView", Key(execution.staticView)}, {"staticContentToken", std::to_string(execution.staticContentToken)},
				{"sourceCommandSerial", std::to_string(execution.sourceCommandSerial)}, {"sourceSubmitSerial", std::to_string(execution.sourceSubmitSerial)},
				{"allocationIdentity", std::to_string(execution.allocationIdentity)}, {"allocationGeneration", std::to_string(execution.allocationGeneration)},
				{"backendGeneration", std::to_string(execution.backendGeneration)}, {"actualBytes", std::to_string(execution.actualBytes)},
				{"staticDraws", std::to_string(execution.staticDraws)}, {"dynamicDraws", std::to_string(execution.dynamicDraws)},
				{"staticMembership", std::move(statics)}, {"dynamicMembership", std::move(dynamics)}, {"fallbackReason", execution.fallbackReason},
				{"allocations", std::move(allocations)}, {"completedSubmissions", std::move(completed)}, {"discardedCommands", std::move(discarded)},
				{"acceptedSubmissions", std::move(accepted)}};
		}
		void ValidateStaticShadowExecution(const RendererEfficiencyEvidence& row)
		{
			const auto& execution = row.staticShadowExecution;
			if (!execution.available) return;
			Require(execution.recordedComplete && execution.backendGeneration, "Static execution lacks complete final native recording or backend ownership.");
			for (const auto* bytes : {&row.staticReservedBytes, &row.staticActiveBytes, &row.staticRetiringBytes})
				Require(bytes->status == RendererEfficiencyEvidenceStatus::Valid && bytes->value && *bytes->value <= kMaximumExactJsonInteger,
					"Static execution lacks measured native memory charges.");
			Require(row.staticCacheBudgetRequestedBytes <= kMaximumExactJsonInteger && row.staticCacheBudgetEffectiveBytes <= kMaximumExactJsonInteger,
				"Static memory budget exceeds exact JSON metric capacity.");
			// Inventory is a copied native ownership observation, not a fabricated
			// completion proof. Recorded references may legitimately have submit 0.
			std::uint64_t active = 0, retiring = 0;
			std::set<std::pair<std::uint64_t, std::uint64_t>> ownerKeys;
			const RendererEfficiencyStaticShadowAllocation* copyOwner = nullptr;
			for (const auto& owner : execution.allocations) {
				Require(owner.sessionGeneration == row.effectiveOptions.rendererSessionGeneration && owner.backendGeneration
					&& owner.backendGeneration == execution.backendGeneration
					&& owner.allocationIdentity && owner.allocationGeneration && owner.actualBytes && owner.depthFormat == 126
					&& owner.extent[0] && owner.extent[1] && (owner.layers == 1 || owner.layers == 6)
					&& ownerKeys.emplace(owner.allocationIdentity, owner.allocationGeneration).second,
					"Static inventory lacks unique current native allocation ownership.");
				auto& sum = owner.retiring ? retiring : active;
				Require(owner.actualBytes <= kMaximumExactJsonInteger - sum, "Static inventory byte sum overflows exact metrics."); sum += owner.actualBytes;
				std::set<std::uint64_t> commands;
				for (const auto& use : owner.uses) Require(use.commandSerial && use.frameSlot < 3 && commands.insert(use.commandSerial).second,
					"Static allocation contains an invalid or duplicate command reference.");
				if (owner.allocationIdentity == execution.allocationIdentity && owner.allocationGeneration == execution.allocationGeneration) copyOwner = &owner;
			}
			Require(*row.staticReservedBytes.value == 0 && active == *row.staticActiveBytes.value && retiring == *row.staticRetiringBytes.value,
				"Static physical inventory does not match complete active/retiring charges.");
			const auto validateHistory = [](const auto& values) {
				std::set<std::uint64_t> unique;
				for (const auto value : values) Require(value && unique.insert(value).second, "Static receipt history contains zero or duplicate serials.");
			};
			validateHistory(execution.completedSubmissions); validateHistory(execution.discardedCommands);
			std::map<std::uint64_t, const RendererEfficiencyStaticShadowUse*> accepted;
			for (const auto& use : execution.acceptedSubmissions)
				Require(use.commandSerial && use.submitSerial && use.frameSlot < 3 && accepted.emplace(use.commandSerial, &use).second,
					"Static accepted-submission history lacks unique positive command mappings.");
			if (const auto current = accepted.find(row.commandSerial); current != accepted.end())
				Require(current->second->submitSerial == row.submitSerial, "Static command mapping disagrees with this actual completed row.");
			if (execution.copyOverlay) if (const auto source = accepted.find(execution.sourceCommandSerial); source != accepted.end())
				Require(source->second->submitSerial == execution.sourceSubmitSerial, "Static source receipt disagrees with its actual accepted mapping.");
			for (const auto& owner : execution.allocations) for (const auto& use : owner.uses) {
				const auto found = accepted.find(use.commandSerial);
				if (found == accepted.end()) continue; // Bounded history can omit an older accepted command.
				// Command and submit serials are independent clocks. A captured
				// recorded use stays submit 0 while its later actual mapping is kept.
				Require(use.frameSlot == found->second->frameSlot && (!use.submitSerial || use.submitSerial == found->second->submitSerial),
					"Static accepted submission disagrees with the captured physical command use.");
			}
			// A combined fallback may have no valid partition. It still carries
			// actual memory diagnostics, and does not assert static optimization.
			if (!execution.copyOverlay && !execution.partitionComplete) return;
			using Identity = std::tuple<UUID, std::uint64_t, std::uint64_t>;
			const auto identity = [](const ShadowCasterStamp& value) { return Identity{value.objectId, value.lifetimeGeneration, value.segmentId}; };
			std::map<Identity, const ShadowCasterStamp*> eligible, partition, expectedSubmitted;
			for (const auto& value : row.artifacts->eligibleMembership.rows)
				Require(eligible.emplace(identity(value), &value).second, "Static partition has duplicate eligible contributions.");
			const auto append = [&](const auto& rows, bool isStatic) {
				for (const auto& value : rows) {
					const auto found = eligible.find(identity(value));
					Require(found != eligible.end() && value == *found->second && partition.emplace(identity(value), &value).second,
						"Static/dynamic partition is duplicate, forged or outside eligible membership.");
					if (isStatic) Require(ClassifyStaticShadowCaster(value).eligible, "Static row lacks complete captured support and semantic proof.");
					if (!isStatic || !execution.staticReused) expectedSubmitted.emplace(identity(value), &value);
				}
			};
			append(execution.staticMembership, true); append(execution.dynamicMembership, false);
			Require(partition.size() == eligible.size(), "Static/dynamic partition omits eligible contributions.");
			if (!execution.copyOverlay) return; // Fallback groups can cross categories.
			const auto& source = execution.staticView; const auto& final = row.artifacts->shadowKey;
			const bool cube = static_cast<std::uint32_t>(final.resourceKind) == 1;
			const auto required = cube ? 0x3fU : 1U;
			Require(execution.partitionComplete && execution.allocationIdentity && execution.allocationGeneration && execution.backendGeneration
				&& execution.actualBytes && execution.actualBytes <= *row.staticActiveBytes.value && execution.staticContentToken
				&& execution.sourceCommandSerial && execution.sourceSubmitSerial && source.allocationGeneration == execution.allocationGeneration
				&& source.regionId == execution.allocationIdentity,
				"Static copy lacks a committed physical source owner.");
			Require(copyOwner && !copyOwner->retiring && copyOwner->actualBytes == execution.actualBytes
				&& copyOwner->depthFormat == source.depthFormat && copyOwner->extent == source.extent && copyOwner->layers == (cube ? 6U : 1U),
				"Static copy source is absent, retiring or inconsistent with physical inventory.");
			// Cube sources own all six layers; a face readback still names one face.
			auto qualified = source; qualified.allocationGeneration = final.allocationGeneration; qualified.regionId = final.regionId;
			if (cube) {
				Require(final.faceIndex < 6 && source.lightProjectionStamp.cubeProjectionEvidenceComplete
					&& final.lightProjectionStamp.cubeProjectionEvidenceComplete
					&& source.lightProjectionStamp.viewProjection == source.lightProjectionStamp.cubeViewProjections[0]
					&& final.lightProjectionStamp.viewProjection == final.lightProjectionStamp.cubeViewProjections[final.faceIndex],
					"Static cube lacks exact source and selected-face projection evidence.");
				qualified.faceIndex = final.faceIndex;
				qualified.lightProjectionStamp.viewProjection = final.lightProjectionStamp.viewProjection;
			}
			Require(qualified == final && (!cube || source.faceIndex == 0), "Static source belongs to another qualified view or light.");
			Require(execution.requiredFaceMask == required && execution.copiedFaceMask == required
				&& execution.staticRecordedFaceMask == (execution.staticReused ? 0U : required), "Static source/copy lacks the complete native face cohort.");
			Require(execution.staticReused || (execution.sourceCommandSerial == row.commandSerial && execution.sourceSubmitSerial == row.submitSerial),
				"Fresh static source lacks this actual command and submit receipt.");
			Require(execution.staticDraws == (execution.staticReused ? 0U : execution.staticMembership.size())
				&& execution.dynamicDraws == execution.dynamicMembership.size()
				&& row.actualDrawSubmissions.value && *row.actualDrawSubmissions.value == execution.staticDraws + execution.dynamicDraws,
				"Static copy draw counts do not match actual per-face singleton commands.");
			std::set<Identity> submitted;
			for (const auto& value : row.artifacts->submittedSegments) {
				const auto found = expectedSubmitted.find(identity(value));
				Require(found != expectedSubmitted.end() && value == *found->second && submitted.insert(identity(value)).second,
					"Static copy submits forged, duplicate or cached contributions.");
			}
			Require(submitted.size() == expectedSubmitted.size(), "Static copy omits newly recorded contributions.");
		}
		Json ShadowBatchExecution(const RendererEfficiencyShadowBatchExecution& execution)
		{
			Json sources = Json::array(), groups = Json::array(), singletons = Json::array();
			for(const auto& source : execution.sources)
				sources.push_back({{"objectId", ToHexString(source.objectId)}, {"lifetimeGeneration", std::to_string(source.lifetimeGeneration)},
					{"sourceOrdinal", source.sourceOrdinal}, {"ownerFirstMeshlet", source.ownerFirstMeshlet}, {"ownerMeshletCount", source.ownerMeshletCount},
					{"resolved", source.resolved}, {"excludedByMaterial", source.excludedByMaterial}, {"cameraVisible", source.cameraVisible}});
			for(const auto& group : execution.groups)
			{
				Json members = Json::array();
				for(const auto& member : group.members) members.push_back(ShadowBatchMember(member));
				groups.push_back({{"firstInstance", group.firstInstance}, {"instanceCount", group.instanceCount}, {"taskGroupsX", group.taskGroupsX},
					{"pipelineIdentity", std::to_string(group.pipelineIdentity)}, {"layoutIdentity", std::to_string(group.layoutIdentity)}, {"members", std::move(members)}});
			}
			for(const auto& member : execution.singletons) singletons.push_back(ShadowBatchMember(member));
			// Serialize observations verbatim, including unavailable or failed upload
			// flags. Neither exported counts nor CPU validation invent a native call.
			return {{"available", execution.available}, {"recordedComplete", execution.recordedComplete}, {"frameSlotSafe", execution.frameSlotSafe},
				{"uploadAttempted", execution.uploadAttempted}, {"uploadSucceeded", execution.uploadSucceeded},
				{"allocationIdentity", std::to_string(execution.allocationIdentity)}, {"allocationGeneration", std::to_string(execution.allocationGeneration)},
				{"backendGeneration", std::to_string(execution.backendGeneration)}, {"instanceBytes", std::to_string(execution.instanceBytes)},
				{"frameSlot", execution.frameSlot}, {"snapshotSlot", execution.snapshotSlot}, {"sources", std::move(sources)},
				{"groups", std::move(groups)}, {"singletons", std::move(singletons)}};
		}
		void ValidateShadowBatchExecution(const RendererEfficiencyEvidence& row)
		{
			const auto& execution = row.shadowBatchExecution;
			if(!execution.available) return; // Earlier native contracts have no P4 receipt.
			Require(execution.recordedComplete && execution.frameSlotSafe && execution.backendGeneration
				&& execution.frameSlot < 3 && execution.snapshotSlot < 8 && execution.instanceBytes % 176 == 0,
				"Available shadow execution lacks complete actual recording or physical owner metadata.");
			Require(!execution.uploadSucceeded || execution.uploadAttempted, "Shadow upload success has no actual attempt.");
			if(!execution.groups.empty()) Require(execution.uploadAttempted && execution.uploadSucceeded
				&& execution.allocationIdentity && execution.allocationGeneration,
				"Recorded shadow groups lack an accepted actual instance allocation/upload.");
			using Identity = std::tuple<UUID, std::uint64_t, std::uint64_t>;
			using Owner = std::pair<UUID, std::uint64_t>;
			const auto identity = [](const auto& value) { return Identity{value.objectId, value.lifetimeGeneration, value.segmentId}; };
			std::map<Identity, const ShadowCasterStamp*> eligible, submitted;
			std::map<Owner, std::vector<std::pair<std::uint64_t, std::uint64_t>>> ownerRanges;
			for(const auto& segment : row.artifacts->eligibleMembership.rows)
			{
				Require(eligible.emplace(identity(segment), &segment).second, "Shadow batching has duplicate eligible semantic contributions.");
				Require(segment.meshletCount && segment.firstMeshlet <= (std::numeric_limits<std::uint64_t>::max)() - segment.meshletCount,
					"Eligible shadow segment has an invalid meshlet extent.");
				ownerRanges[{segment.objectId, segment.lifetimeGeneration}].emplace_back(segment.firstMeshlet, segment.firstMeshlet + segment.meshletCount);
			}
			for(const auto& segment : row.artifacts->submittedSegments)
				Require(eligible.contains(identity(segment)) && submitted.emplace(identity(segment), &segment).second,
					"Shadow batching submits a duplicate or ineligible semantic contribution.");
			std::set<Identity> recorded;
			std::vector<std::pair<std::uint64_t, std::uint64_t>> instanceRanges;
			for(const auto& group : execution.groups)
			{
				const std::uint64_t end = std::uint64_t(group.firstInstance) + group.instanceCount;
				Require(group.instanceCount >= 2 && group.members.size() == group.instanceCount && group.taskGroupsX
					&& group.pipelineIdentity && group.layoutIdentity && end <= execution.instanceBytes / 176,
					"Shadow group has incomplete actual members, pipeline or uploaded instance range.");
				instanceRanges.emplace_back(group.firstInstance, end);
				for(const auto& member : group.members)
					Require(submitted.contains(identity(member)) && recorded.insert(identity(member)).second,
						"Shadow group member is duplicate or absent from actual submitted semantics.");
			}
			std::sort(instanceRanges.begin(), instanceRanges.end());
			for(std::size_t index = 1; index < instanceRanges.size(); ++index)
				Require(instanceRanges[index - 1].second <= instanceRanges[index].first, "Actual shadow group instance ranges overlap.");
			for(const auto& member : execution.singletons)
				Require(submitted.contains(identity(member)) && recorded.insert(identity(member)).second,
					"Shadow singleton is duplicate or absent from actual submitted semantics.");
			Require(recorded.size() == submitted.size(), "Actual shadow group/singleton commands do not cover all submitted contributions.");
			const auto countMatches = [](const RendererEfficiencyCounter& counter, std::uint64_t value) {
				return counter.status == RendererEfficiencyEvidenceStatus::Valid && counter.value == value;
			};
			Require(countMatches(row.groups, execution.groups.size()) && countMatches(row.singletons, execution.singletons.size())
				&& countMatches(row.actualDrawSubmissions, execution.groups.size() + execution.singletons.size()),
				"Shadow command counters disagree with actual group/singleton recording receipts.");
			std::set<std::uint32_t> ordinals;
			std::set<Owner> sourceOwners;
			for(const auto& source : execution.sources)
			{
				const Owner owner{source.objectId, source.lifetimeGeneration};
				Require(source.objectId != EMPTY_UUID && source.lifetimeGeneration && source.resolved
					&& ordinals.insert(source.sourceOrdinal).second && sourceOwners.insert(owner).second,
					"Shadow source is unresolved, duplicated or lacks exact original lifetime identity.");
				// Index ranges once per immutable row rather than scanning every
				// caster again for each original source in a thousand-caster fixture.
				const auto found = ownerRanges.find(owner);
				auto ranges = found == ownerRanges.end() ? std::vector<std::pair<std::uint64_t, std::uint64_t>>{} : found->second;
				if(source.excludedByMaterial)
				{
					Require(ranges.empty(), "Material-excluded source has an eligible shadow contribution.");
					continue;
				}
				Require(source.ownerMeshletCount && !ranges.empty(), "Resolved caster has no complete eligible owner range.");
				std::sort(ranges.begin(), ranges.end());
				std::uint64_t next = source.ownerFirstMeshlet;
				for(const auto& range : ranges)
				{
					Require(range.first == next, "Eligible shadow owner ranges overlap or omit selected meshlets.");
					next = range.second;
				}
				Require(next == std::uint64_t(source.ownerFirstMeshlet) + source.ownerMeshletCount,
					"Eligible shadow segments do not cover the actual light-selected owner span.");
			}
			for(const auto& [id, segment] : eligible)
				Require(sourceOwners.contains({segment->objectId, segment->lifetimeGeneration}), "Eligible shadow segment has no resolved original source.");
		}
		void ValidateRow(const RendererEfficiencyEvidenceExportContext& context, const RendererEfficiencyEvidence& row)
		{
			Require(row.status == RendererEfficiencyEvidenceStatus::Valid && row.artifacts && row.artifacts->frameColor, "Evidence lacks complete immutable CPU readbacks.");
			const auto& correlation = row.fixtureCorrelation; const auto& key = row.artifacts->shadowKey;
			Require(correlation.operationId == context.operationId && correlation.fixtureStepId == context.fixtureStepId
				&& correlation.settingsGeneration == context.settingsGeneration && correlation.mutationGeneration == context.mutationGeneration
				&& correlation.rendererSessionGeneration == context.rendererSessionGeneration, "Evidence correlation differs from captured export operation/step/generations.");
			Require(row.frameSerial && row.commandSerial && row.submitSerial && row.completionSerial >= row.submitSerial, "Evidence lacks actual nonzero command/submit/completion receipts.");
			// Transition evidence keeps the original command correlation. A protected
			// control publication must be actual and nonzero; serialization cannot
			// substitute the later measurement token or invent either native epoch.
			if(context.measurementPhase == "Transition") {
				Require(row.fixtureControlGeneration && row.effectiveOptions.optionsSessionGeneration,
					"Transition lacks actual native control/options publication proof.");
				// Residency tokens follow the frozen sample window, while Control
				// observes zero. These independent purposes never relabel native rows.
				const bool matchingFrame = context.transitionPurpose == RendererEfficiencyTransitionPurpose::Control ? correlation.stepFrame == 0
					: context.transitionPurpose == RendererEfficiencyTransitionPurpose::ResidencyBaseline ? correlation.stepFrame == context.residencyBaselineStepFrame
					: correlation.stepFrame > context.residencyBaselineStepFrame;
				Require(matchingFrame,
					"Transition native step frame contradicts its captured typed purpose.");
			}
			Require(row.effectiveOptions.settingsGeneration == context.settingsGeneration && row.effectiveOptions.rendererSessionGeneration == context.rendererSessionGeneration,
				"Effective settings were not observed in the captured renderer session.");
			Require(row.kind == RendererEfficiencyEvidenceKind::Shadow || row.kind == RendererEfficiencyEvidenceKind::Lighting || row.kind == RendererEfficiencyEvidenceKind::LightingDepth, "Unknown native evidence kind.");
			if(row.kind==RendererEfficiencyEvidenceKind::LightingDepth){ValidateLightingDepth(row);return;}
			// Lighting uses the same actual completion/color proof, but does not
			// fabricate depth faces, caster memberships or a nonempty light UUID.
			if (row.kind == RendererEfficiencyEvidenceKind::Lighting)
			{
				Require(row.viewType == RenderViewType::Scene || row.viewType == RenderViewType::Game, "Lighting view is unsupported.");
				Require(row.fixtureControlGeneration && row.effectiveOptions.optionsSessionGeneration, "Lighting lacks current control/options proof.");
				ValidateLightingUpload(row.lightingUploads[row.analysisPurpose ? 1 : 0], row.analysisPurpose);
				LightingRows(row, true);
				Require(row.artifacts->fullSourceCandidateCount.has_value()
					&& *row.artifacts->fullSourceCandidateCount <= 4096
					&& *row.artifacts->fullSourceCandidateCount >= row.lightingUploads[row.analysisPurpose ? 1 : 0].activeRecordCount,
					"Lighting lacks the actual bounded uncapped source candidate count.");
				Require(row.artifacts->lightingPayload.size() == 176ULL * 1024, "Lighting readback lacks the full native 1024-record block.");
				const auto tail = row.lightingUploads[row.analysisPurpose ? 1 : 0].activeRecordCount * 176ULL;
				Require(std::all_of(row.artifacts->lightingPayload.begin() + tail, row.artifacts->lightingPayload.end(), [](auto byte) { return byte == 0; }),
					"Native lighting inactive tail was not cleared.");
				const auto& color = *row.artifacts->frameColor; const auto bpp = VulkanReadback::BytesPerPixel(static_cast<VkFormat>(color.sourceFormat));
				Require(color.width && color.height && bpp && std::uint64_t(color.width) * color.height <= kMaximumArtifactBytes / bpp
					&& color.bytes.size() == std::uint64_t(color.width) * color.height * bpp, "Lighting lacks its full native same-command color.");
				return;
			}
			LightingRows(row, true); // Available P2 receipts on shadow rows are equally qualified.
			Require(key.rendererSessionGeneration == context.rendererSessionGeneration && key.viewType == row.viewType && key.viewId == row.viewId && key.lightId == row.lightId
				&& key.faceIndex == row.faceIndex && key.extent[0] == row.resolution.x && key.extent[1] == row.resolution.y && key.allocationGeneration,
				"Qualified shadow view/allocation/native extent differs from completed row.");
			Require(row.lightId != EMPTY_UUID && (row.viewType == RenderViewType::Scene || row.viewType == RenderViewType::Game), "Completed row lacks a supported view/light identity.");
			Require(static_cast<std::uint32_t>(key.resourceKind) < static_cast<std::uint32_t>(ShadowResourceKind::Unsupported), "Completed row names an unsupported shadow resource route.");
			const auto& artifacts = *row.artifacts;
			Require(artifacts.contentToken && artifacts.depthSourceFormat == 126 && key.depthFormat == 126 && artifacts.depthEncoding == "float32-le" && artifacts.coverageEncoding == "occupied-u8",
				"Evidence depth is not full native D32_SFLOAT float32-le with occupied-u8 coverage.");
			const std::uint64_t pixels = std::uint64_t(row.resolution.x) * row.resolution.y;
			Require(pixels && pixels <= kMaximumArtifactBytes / 4 && artifacts.depthF32LE.size() == pixels * 4 && artifacts.coverageU8.size() == pixels,
				"Native depth/coverage byte counts do not match the full shadow extent.");
			for(std::size_t index = 0; index < artifacts.coverageU8.size(); ++index)
			{
				std::uint32_t bits = 0; for(unsigned byte = 0; byte < 4; ++byte) bits |= std::uint32_t(artifacts.depthF32LE[index * 4 + byte]) << (byte * 8);
				const float depth = std::bit_cast<float>(bits);
				Require(std::isfinite(depth) && depth >= 0 && depth <= 1 && artifacts.coverageU8[index] == (depth < 1.F ? 1 : 0), "Native depth/coverage contains invalid pixels or mismatched occupancy.");
			}
			for(const auto& segment : artifacts.eligibleMembership.rows) Require(segment.objectId != EMPTY_UUID && segment.lifetimeGeneration, "Eligible caster row lacks exact object/lifetime identity.");
			for(const auto& segment : artifacts.submittedSegments) Require(segment.objectId != EMPTY_UUID && segment.lifetimeGeneration, "Submitted caster row lacks exact object/lifetime identity.");
			Require(row.eligibleCasterSegments.status == RendererEfficiencyEvidenceStatus::Valid && row.eligibleCasterSegments.value == artifacts.eligibleMembership.rows.size()
				&& row.submittedCasterSegments.status == RendererEfficiencyEvidenceStatus::Valid && row.submittedCasterSegments.value == artifacts.submittedSegments.size(),
				"Native caster counters do not match exact emitted identity rows.");
			// A reused shadow (or an empty light) legitimately records zero new draws.
			// The optional must contain an observed counter, not a positive value.
			Require(row.actualDrawSubmissions.status == RendererEfficiencyEvidenceStatus::Valid && row.actualDrawSubmissions.value.has_value() && !row.contentOwner.empty(),
				"Completed shadow observation lacks its native draw counter or source content owner.");
			ValidateShadowBatchExecution(row);
			ValidateStaticShadowExecution(row);
			const auto& color = *artifacts.frameColor;
			const auto colorPixelBytes = VulkanReadback::BytesPerPixel(static_cast<VkFormat>(color.sourceFormat));
			Require(color.width && color.height && colorPixelBytes && std::uint64_t(color.width) * color.height <= kMaximumArtifactBytes / colorPixelBytes
				&& color.bytes.size() == std::uint64_t(color.width) * color.height * colorPixelBytes, "Native frame color format or full extent byte count is unsupported.");
		}
	}

	bool ExportRendererEfficiencyEvidence(const RendererEfficiencyEvidenceExportContext& context,
		std::span<const RendererEfficiencyEvidence> evidence, RendererEfficiencyEvidenceExportResult& output, std::string& error)
	{
		try
		{
			Require(!evidence.empty() && evidence.size() <= 128, "Export bundle is empty or exceeds the bounded face/view count.");
			Require(ParseUUID(context.operationId).has_value() && !context.fixtureId.empty() && context.fixtureHash.size() == 64 && !context.fixtureStepId.empty()
				&& !context.runId.empty() && context.seed && context.settingsGeneration && context.mutationGeneration && context.rendererSessionGeneration,
				"Captured export identity is incomplete.");
			Require(context.seed <= kMaximumExactJsonInteger && context.runIndex <= kMaximumExactJsonInteger, "Export run identity exceeds exact JSON range.");
			Require((context.mode == "Reference" || context.mode == "Candidate") && (context.measurementPhase == "Measured" || context.measurementPhase == "Warmup" || context.measurementPhase == "Transition" || context.measurementPhase == "LightingBaseline"), "Export mode/phase is unsupported.");
			Require(context.transitionPurpose == RendererEfficiencyTransitionPurpose::Control
				|| context.transitionPurpose == RendererEfficiencyTransitionPurpose::ResidencyBaseline
				|| context.transitionPurpose == RendererEfficiencyTransitionPurpose::ResidencyReupload, "Export transition purpose is unsupported.");
			Require(context.measurementPhase == "Transition" || context.transitionPurpose == RendererEfficiencyTransitionPurpose::Control,
				"Normal measurement phases cannot carry a residency transition purpose.");
			Require(context.transitionPurpose == RendererEfficiencyTransitionPurpose::Control ? context.residencyBaselineStepFrame == 0
				: context.residencyBaselineStepFrame > 0 && context.residencyBaselineStepFrame <= kMaximumExactJsonInteger,
				"Export purpose lacks its exact native residency baseline token or carries an unexpected token.");
			const auto project = std::filesystem::absolute(context.projectRoot).lexically_normal(); NoRedirects(project);
			const auto root = std::filesystem::absolute(context.outputDirectory).lexically_normal();
			Require(Within(root, project / "renderer-efficiency-evidence" / context.operationId), "Evidence output must stay in the owned operation directory."); NoRedirects(root);
			const auto markerPath = project / "renderer-efficiency-project.json"; NoRedirects(markerPath);
			Require(std::filesystem::file_size(markerPath) <= 4 * 1024 * 1024, "Ownership marker exceeds bounded export input.");
			Json marker; std::ifstream(markerPath) >> marker;
			Require(marker.at("schemaVersion") == 1 && marker.at("kind") == "RendererEfficiencyValidationProject" && marker.at("fixtureId") == context.fixtureId
				&& marker.at("fixtureHash") == context.fixtureHash && PathKey(marker.at("projectRoot").get<std::string>()) == PathKey(project), "Export ownership marker differs from captured fixture/project.");
			std::map<std::pair<RenderViewType, std::uint32_t>, unsigned> lightingPurposes;
			std::map<std::string, unsigned> cubeFaces; std::set<std::string> observations;
			std::map<std::pair<RenderViewType, std::uint32_t>, std::uint64_t> viewCommands;
			for(const auto& row : evidence)
			{
				ValidateRow(context, row);
				if(context.measurementPhase == "LightingBaseline") Require(row.kind == RendererEfficiencyEvidenceKind::Lighting && row.fixtureCorrelation.stepFrame < 60, "Lighting baseline must retain its actual first-sixty native lighting token.");
				// One view renders all its shadow faces into one primary. Separate
				// Scene/Game primaries may share a submit, so qualify command identity
				// by view instead of demanding one command for the whole bundle.
				const auto [command, inserted] = viewCommands.emplace(std::pair{row.viewType, row.viewId}, row.commandSerial);
				Require(inserted || command->second == row.commandSerial, "Export view combines contradictory native command identities.");
				Require(row.fixtureCorrelation.stepFrame <= kMaximumExactJsonInteger, "Step frame exceeds exact JSON range.");
				Require(row.frameSerial == evidence.front().frameSerial && row.submitSerial == evidence.front().submitSerial && row.completionSerial == evidence.front().completionSerial
					&& row.fixtureCorrelation.stepFrame == evidence.front().fixtureCorrelation.stepFrame && row.effectiveOptions.options == evidence.front().effectiveOptions.options,
					"Export bundle combines different frames, submissions, steps, or effective options.");
				if(context.measurementPhase == "Transition" || row.kind == RendererEfficiencyEvidenceKind::Lighting || row.kind==RendererEfficiencyEvidenceKind::LightingDepth)
					Require(row.fixtureControlGeneration == evidence.front().fixtureControlGeneration
						&& row.effectiveOptions.optionsSessionGeneration == evidence.front().effectiveOptions.optionsSessionGeneration,
						"Transition bundle combines different protected control/options publications.");
				if(row.kind==RendererEfficiencyEvidenceKind::LightingDepth)
				{
					const auto observation="LightingDepth:"+std::to_string(static_cast<unsigned>(row.viewType))+":"+std::to_string(row.viewId);
					Require(observations.insert(observation).second,"Export bundle duplicates a lighting-depth view.");
					continue;
				}
				if (row.kind == RendererEfficiencyEvidenceKind::Lighting)
				{
					const auto observation = "Lighting:" + std::to_string(static_cast<unsigned>(row.viewType)) + ":" + std::to_string(row.viewId) + ":" + std::to_string(row.analysisPurpose);
					Require(observations.insert(observation).second, "Export bundle duplicates a lighting purpose.");
					lightingPurposes[{row.viewType,row.viewId}] |= row.analysisPurpose ? 2U : 1U;
					continue;
				}
				const std::string observation = std::to_string(static_cast<unsigned>(row.viewType)) + ":" + std::to_string(row.viewId) + ":" + ToHexString(row.lightId);
				Require(observations.insert(observation + ":" + std::to_string(row.faceIndex)).second, "Export bundle duplicates a shadow face.");
				if(row.artifacts->shadowKey.resourceKind == ShadowResourceKind::PointCube) { Require(row.faceIndex < 6, "Cube face index exceeds native six-face range."); cubeFaces[observation] |= 1U << row.faceIndex; }
				else Require(row.faceIndex == 0, "Noncube evidence has a cube face index.");
			}
			for(const auto& [view, purposes] : lightingPurposes) Require(purposes == 3, "Partial native lighting purpose bundle cannot publish evidence.");
			for(const auto& [key, faces] : cubeFaces) Require(faces == 0x3f, "Partial point cube cannot publish an evidence bundle.");
			std::filesystem::create_directories(root); NoRedirects(root);
			RendererEfficiencyEvidenceExportResult result;
			// Every row has already passed the complete native bundle gates above.
			// Keep only this job's last immutable color owner and encoded bytes. A
			// hit requires exact current raw bytes and format/extent equality, never
			// a fingerprint or a prior frame's capture. Other per-row artifacts and
			// provenance still take their normal validation/publication paths.
			std::shared_ptr<const RendererEfficiencyFrameColor> encodedColorOwner;
			std::vector<std::uint8_t> png;
			for(const auto& row : evidence)
			{
				const auto& native = *row.artifacts; const auto& color = *native.frameColor;
				Require(color.width && color.height && color.bytes.size() <= kMaximumArtifactBytes, "Native frame color has no bounded full extent.");
				if(!encodedColorOwner || encodedColorOwner->sourceFormat != color.sourceFormat
					|| encodedColorOwner->width != color.width || encodedColorOwner->height != color.height
					|| encodedColorOwner->bytes != color.bytes)
				{
					const auto rgba = VulkanReadback::ConvertToRgba8(color.bytes, color.width, color.height, static_cast<VkFormat>(color.sourceFormat));
					png = EncodeRgba8ToPng(rgba, color.width, color.height);
					encodedColorOwner = native.frameColor;
					++result.colorEncodingCount;
				}
				if(row.kind==RendererEfficiencyEvidenceKind::LightingDepth)
				{
					const auto& depth=*native.lightingDepth;
					Json artifacts=Json::array({WriteArtifact(project,root,"ProductionDepthBounds",".production-depth.f32",depth.productionBounds),WriteArtifact(project,root,"Image",".png",png)});
					if(!depth.oracleInput.empty()){
						artifacts.push_back(WriteArtifact(project,root,"DepthInput",".depth-input.bin",depth.oracleInput));
						artifacts.push_back(WriteArtifact(project,root,"DepthBounds",".depth-bounds.f32",depth.oracleBounds));
						artifacts.push_back(WriteArtifact(project,root,"DepthCoverage",".depth-coverage.bin",depth.oracleCoverage));
					}
					Json counters={{"schemaVersion",1},{"evidenceKind","LightingDepth"},{"payloadPurpose","Production"},
						{"fixtureId",context.fixtureId},{"fixtureHash",context.fixtureHash},{"operationId",context.operationId},{"fixtureStepId",context.fixtureStepId},
						{"runId",context.runId},{"runIndex",context.runIndex},{"seed",context.seed},{"mode",context.mode},{"stage",context.stage},
						{"measurementPhase",context.measurementPhase},{"stepFrame",std::to_string(row.fixtureCorrelation.stepFrame)},
						{"viewType",row.viewType==RenderViewType::Scene?"Scene":"Game"},{"viewId",row.viewId},
						{"productionNativeExtent",{depth.productionWidth,depth.productionHeight}},{"tileCount",144},{"boundsEncoding","float32-ieee-le"},{"coverageEncoding","uint32-pair-le"},
						{"productionQuery",DepthQuery(depth.productionQuery)},{"oracleQuery",depth.oracleQuery?DepthQuery(*depth.oracleQuery):Json(nullptr)},
						{"depthFallbackReason",row.fallbackReason},
						{"oracleRecipe",depth.oracleRecipe?DepthRecipe(*depth.oracleRecipe):Json(nullptr)},
						{"lightingUploads",LightingRows(row,true)},{"colorSourceFormat",color.sourceFormat},{"colorNativeExtent",{color.width,color.height}},{"imageEncoding","png-rgba8"},
						{"effectiveOptions",Options(row.effectiveOptions.options)},
						{"readbackProvenance",{{"kind","FullNativeSameCommandCompletedReadback"},{"frameSerial",std::to_string(row.frameSerial)},{"commandSerial",std::to_string(row.commandSerial)},
							{"submitSerial",std::to_string(row.submitSerial)},{"completionSerial",std::to_string(row.completionSerial)},{"rendererSessionGeneration",std::to_string(context.rendererSessionGeneration)},
							{"settingsGeneration",std::to_string(context.settingsGeneration)},{"mutationGeneration",std::to_string(context.mutationGeneration)},{"fixtureControlGeneration",std::to_string(row.fixtureControlGeneration)},
							{"optionsSessionGeneration",std::to_string(row.effectiveOptions.optionsSessionGeneration)}}},
						{"nativePreparation",{{"workerTasks",row.preparationWorkerTasks},{"inlineTasks",row.preparationInlineTasks},{"settled",row.preparationSettled}}}};
					if(native.fullSourceCandidateCount)counters["fullSourceCandidateCount"]=*native.fullSourceCandidateCount;
					const auto text=counters.dump(1,'\t');artifacts.push_back(WriteArtifact(project,root,"Counters",".counters.json",std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size())));
					const auto unavailable=Unavailable("Lighting-depth observation has no shadow region.");
					Json shadow;for(const auto* field:{"eligibleCasterSegments","submittedCasterSegments","actualDrawSubmissions","groups","singletons","staticRefreshCasterSegments","dynamicRefreshCasterSegments","reuseRegionCount","staticReuseRegionCount","contentOwner","refreshReason","fallbackReason"})shadow[field]=unavailable;
					Json memory;for(const auto* field:{"staticCacheBudgetRequestedBytes","staticCacheBudgetEffectiveBytes","reservedBytes","activeBytes","retiringBytes","auxiliaryBytes","admissionReason","fallbackReason"})memory[field]=unavailable;memory["draining"]="Unavailable";
					const auto& query=depth.productionQuery.result;
					const auto timing=query.status==DepthTiming::Status::Valid?Valid(query.durationNanoseconds):Json{{"status",DepthStatus(query.status)},{"value",nullptr},{"reason",DepthReason(query.reason)}};
					Json frame={{"payloadPurpose","Production"},{"evidenceKind","LightingDepth"},{"fixtureStepId",context.fixtureStepId},{"stepFrame",row.fixtureCorrelation.stepFrame},{"measurementPhase",context.measurementPhase},
						{"viewType",row.viewType==RenderViewType::Scene?"Scene":"Game"},{"viewId",row.viewId},{"lightId",unavailable},{"faceIndex",unavailable},
						{"frameSerial",Number(row.frameSerial)},{"commandSerial",Number(row.commandSerial)},{"submitSerial",Number(row.submitSerial)},{"completionSerial",Number(row.completionSerial)},
						{"settingsGeneration",Number(context.settingsGeneration)},{"mutationGeneration",Number(context.mutationGeneration)},{"rendererSessionGeneration",Number(context.rendererSessionGeneration)},
						{"publicationState","Complete"},{"effectiveOptions",Options(row.effectiveOptions.options)},{"lighting",LightingRows(row,false)},{"shadow",shadow},{"memory",memory},
						{"depthStageTimingNanoseconds",timing},{"depthFallbackReason",Text(row.fallbackReason)},{"totalFrameTimingNanoseconds",Unavailable("No completion-qualified total-frame GPU timing producer.")},{"artifacts",artifacts}};
					if(native.fullSourceCandidateCount)frame["fullSourceCandidateCount"]=Number(*native.fullSourceCandidateCount);
					result.frames.push_back(std::move(frame));for(const auto& artifact:artifacts)result.artifacts.push_back(artifact);
					continue;
				}
				if (row.kind == RendererEfficiencyEvidenceKind::Lighting)
				{
					Json artifacts = Json::array({WriteArtifact(project, root, "LightingPayload", ".lighting.bin", native.lightingPayload), WriteArtifact(project, root, "Image", ".png", png)});
					artifacts[0]["payloadPurpose"] = row.analysisPurpose ? "Analysis" : "FinalLights";
					Json counters = {{"schemaVersion",1},{"evidenceKind","Lighting"},{"analysisPurpose",row.analysisPurpose},
						{"fullSourceCandidateCount",*native.fullSourceCandidateCount},
						{"fixtureId",context.fixtureId},{"fixtureHash",context.fixtureHash},{"operationId",context.operationId},{"fixtureStepId",context.fixtureStepId},
						{"runId",context.runId},{"runIndex",context.runIndex},{"seed",context.seed},{"mode",context.mode},{"stage",context.stage},
						{"measurementPhase",context.measurementPhase},{"stepFrame",std::to_string(row.fixtureCorrelation.stepFrame)},
						{"payloadEncoding","float32-ieee-le"},{"recordBytes",176},{"recordCapacity",1024},{"payloadPurpose",row.analysisPurpose ? "Analysis" : "FinalLights"},
						{"lightingUploads",LightingRows(row,true)},{"colorSourceFormat",color.sourceFormat},{"colorNativeExtent",{color.width,color.height}},
						{"imageEncoding","png-rgba8"},{"effectiveOptions",Options(row.effectiveOptions.options)},
						{"readbackProvenance",{{"kind","FullNativeSameCommandCompletedReadback"},{"frameSerial",std::to_string(row.frameSerial)},
							{"commandSerial",std::to_string(row.commandSerial)},{"submitSerial",std::to_string(row.submitSerial)},{"completionSerial",std::to_string(row.completionSerial)},
							{"rendererSessionGeneration",std::to_string(context.rendererSessionGeneration)},{"settingsGeneration",std::to_string(context.settingsGeneration)},
							{"mutationGeneration",std::to_string(context.mutationGeneration)},{"fixtureControlGeneration",std::to_string(row.fixtureControlGeneration)},
							{"optionsSessionGeneration",std::to_string(row.effectiveOptions.optionsSessionGeneration)}}},
						{"nativePreparation",{{"workerTasks",row.preparationWorkerTasks},{"inlineTasks",row.preparationInlineTasks},{"settled",row.preparationSettled}}}};
					const auto text = counters.dump(1, '\t'); artifacts.push_back(WriteArtifact(project,root,"Counters",".counters.json",std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size())));
					const auto unavailable = Unavailable("Lighting observation has no shadow region.");
					Json shadow; for (const auto* field : {"eligibleCasterSegments","submittedCasterSegments","actualDrawSubmissions","groups","singletons","staticRefreshCasterSegments","dynamicRefreshCasterSegments","reuseRegionCount","staticReuseRegionCount","contentOwner","refreshReason","fallbackReason"}) shadow[field] = unavailable;
					Json memory; for (const auto* field : {"staticCacheBudgetRequestedBytes","staticCacheBudgetEffectiveBytes","reservedBytes","activeBytes","retiringBytes","auxiliaryBytes","admissionReason","fallbackReason"}) memory[field] = unavailable;
					memory["draining"] = "Unavailable";
					Json frame = {{"payloadPurpose",row.analysisPurpose ? "Analysis" : "FinalLights"},{"evidenceKind","Lighting"},{"fixtureStepId",context.fixtureStepId},{"stepFrame",row.fixtureCorrelation.stepFrame},{"measurementPhase",context.measurementPhase},
						{"fullSourceCandidateCount",Number(*native.fullSourceCandidateCount)},
						{"viewType",row.viewType == RenderViewType::Scene ? "Scene" : "Game"},{"viewId",row.viewId},{"lightId",unavailable},{"faceIndex",unavailable},
						{"frameSerial",Number(row.frameSerial)},{"commandSerial",Number(row.commandSerial)},{"submitSerial",Number(row.submitSerial)},{"completionSerial",Number(row.completionSerial)},
						{"settingsGeneration",Number(context.settingsGeneration)},{"mutationGeneration",Number(context.mutationGeneration)},{"rendererSessionGeneration",Number(context.rendererSessionGeneration)},
						{"publicationState","Complete"},{"effectiveOptions",Options(row.effectiveOptions.options)},{"lighting",LightingRows(row,false)},
						{"shadow",shadow},{"memory",memory},{"depthStageTimingNanoseconds",Timing(row.lightingDepthTiming)},
						{"totalFrameTimingNanoseconds",Unavailable("No completion-qualified total-frame GPU timing producer.")},{"artifacts",artifacts}};
					result.frames.push_back(std::move(frame)); for (const auto& artifact : artifacts) result.artifacts.push_back(artifact);
					continue;
				}
				Json artifacts = Json::array({WriteArtifact(project, root, "Depth", ".f32", native.depthF32LE), WriteArtifact(project, root, "Coverage", ".coverage", native.coverageU8), WriteArtifact(project, root, "Image", ".png", png)});
				Json eligible = Json::array(), submitted = Json::array(); for(const auto& segment : native.eligibleMembership.rows) eligible.push_back(Stamp(segment)); for(const auto& segment : native.submittedSegments) submitted.push_back(Stamp(segment));
				Json counters = {{"schemaVersion", 1}, {"fixtureId", context.fixtureId}, {"fixtureHash", context.fixtureHash}, {"operationId", context.operationId}, {"fixtureStepId", context.fixtureStepId},
					{"runId", context.runId}, {"runIndex", context.runIndex}, {"seed", context.seed}, {"mode", context.mode}, {"stage", context.stage}, {"measurementPhase", context.measurementPhase}, {"stepFrame", std::to_string(row.fixtureCorrelation.stepFrame)},
					{"qualifiedShadowViewKey", Key(native.shadowKey)}, {"sourceContentOwner", {{"nativeOwner", row.contentOwner}, {"qualifiedShadowViewKey", Key(native.shadowKey)}, {"contentToken", std::to_string(native.contentToken)}}}, {"contentToken", std::to_string(native.contentToken)},
					{"depthSourceFormat", native.depthSourceFormat}, {"depthNativeExtent", {row.resolution.x, row.resolution.y}}, {"depthEncoding", native.depthEncoding}, {"coverageEncoding", native.coverageEncoding},
					{"colorSourceFormat", color.sourceFormat}, {"colorNativeExtent", {color.width, color.height}}, {"imageEncoding", "png-rgba8"},
					{"readbackProvenance", {{"kind", "FullNativeSameCommandCompletedReadback"}, {"frameSerial", std::to_string(row.frameSerial)}, {"commandSerial", std::to_string(row.commandSerial)},
						{"submitSerial", std::to_string(row.submitSerial)}, {"completionSerial", std::to_string(row.completionSerial)}, {"rendererSessionGeneration", std::to_string(context.rendererSessionGeneration)},
						{"settingsGeneration", std::to_string(context.settingsGeneration)}, {"mutationGeneration", std::to_string(context.mutationGeneration)},
						{"fixtureControlGeneration", std::to_string(row.fixtureControlGeneration)}, {"optionsSessionGeneration", std::to_string(row.effectiveOptions.optionsSessionGeneration)}}},
					{"eligibleCasterIdentityRows", eligible}, {"submittedCasterIdentityRows", submitted}, {"eligibleMembershipDigest", std::to_string(native.eligibleMembership.digest)}, {"membershipReusable", native.eligibleMembership.reusable}, {"membershipReason", native.eligibleMembership.reason},
					{"nativeCounters", {{"eligibleCasterSegments", Counter(row.eligibleCasterSegments)}, {"submittedCasterSegments", Counter(row.submittedCasterSegments)}, {"actualDrawSubmissions", Counter(row.actualDrawSubmissions)},
						{"groups", Counter(row.groups)}, {"singletons", Counter(row.singletons)}, {"staticRefreshes", Counter(row.staticRefreshes)}, {"dynamicRefreshes", Counter(row.dynamicRefreshes)}, {"shadowReuses", Counter(row.shadowReuses)},
						{"lightingLogicalWriteBytes", Counter(row.lightingLogicalWriteBytes)}, {"lightingWriteRecords", Counter(row.lightingWriteRecords)}, {"lightingWriteRanges", Counter(row.lightingWriteRanges)},
						{"lightingPhysicalFlushBytes", Counter(row.lightingPhysicalFlushBytes)}, {"lightingFlushRanges", Counter(row.lightingFlushRanges)}, {"staticReservedBytes", Counter(row.staticReservedBytes)}, {"staticActiveBytes", Counter(row.staticActiveBytes)}, {"staticRetiringBytes", Counter(row.staticRetiringBytes)}}},
					{"nativePreparation", {{"workerTasks", row.preparationWorkerTasks}, {"inlineTasks", row.preparationInlineTasks}, {"settled", row.preparationSettled}}},
					{"nativeShadowDiagnostics", {{"refreshReason", row.refreshReason}, {"fallbackReason", row.fallbackReason}, {"contentOwner", row.contentOwner}}},
					{"lightingDepthTimingNanoseconds", Timing(row.lightingDepthTiming)}, {"totalFrameTimingNanoseconds", Unavailable("No completion-qualified total-frame GPU timing producer.")}, {"effectiveOptions", Options(row.effectiveOptions.options)}};
				counters["lightingUploads"] = LightingRows(row,true);
				counters["shadowBatchExecution"] = ShadowBatchExecution(row.shadowBatchExecution);
				counters["staticShadowExecution"] = StaticShadowExecution(row.staticShadowExecution);
				const bool staticAvailable = row.staticShadowExecution.available;
				counters["nativeCounters"]["staticCacheBudgetRequestedBytes"] = staticAvailable ? Number(row.staticCacheBudgetRequestedBytes) : Unavailable("No native static execution producer.");
				counters["nativeCounters"]["staticCacheBudgetEffectiveBytes"] = staticAvailable ? Number(row.staticCacheBudgetEffectiveBytes) : Unavailable("No native static execution producer.");
				counters["nativeCounters"]["staticCacheDraining"] = staticAvailable ? Json(row.staticCacheDraining) : Json("Unavailable");
				if (!staticAvailable) for (const auto* field : {"staticReservedBytes", "staticActiveBytes", "staticRetiringBytes"})
					counters["nativeCounters"][field] = Unavailable("No native static execution producer.");
				const auto text = counters.dump(1, '\t'); artifacts.push_back(WriteArtifact(project, root, "Counters", ".counters.json", std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
				const auto unavailable = Unavailable("This stage has not published the required native counter.");
				Json lighting = LightingRows(row,false);
				Json frame = {{"fixtureStepId", context.fixtureStepId}, {"stepFrame", row.fixtureCorrelation.stepFrame}, {"measurementPhase", context.measurementPhase}, {"viewType", row.viewType == RenderViewType::Scene ? "Scene" : "Game"},
					{"fullSourceCandidateCount",Unavailable("Shadow readback does not own uncapped lighting candidate metadata.")},
					{"viewId", row.viewId}, {"lightId", Valid(ToHexString(row.lightId))}, {"faceIndex", Number(row.faceIndex)}, {"frameSerial", Number(row.frameSerial)}, {"settingsGeneration", Number(context.settingsGeneration)},
					{"mutationGeneration", Number(context.mutationGeneration)}, {"rendererSessionGeneration", Number(context.rendererSessionGeneration)}, {"commandSerial", Number(row.commandSerial)}, {"submitSerial", Number(row.submitSerial)}, {"completionSerial", Number(row.completionSerial)},
					{"publicationState", "Complete"}, {"effectiveOptions", Options(row.effectiveOptions.options)}, {"lighting", lighting},
					{"shadow", {{"eligibleCasterSegments", Counter(row.eligibleCasterSegments)}, {"submittedCasterSegments", Counter(row.submittedCasterSegments)}, {"actualDrawSubmissions", Counter(row.actualDrawSubmissions)}, {"groups", Counter(row.groups)},
						{"singletons", Counter(row.singletons)}, {"staticRefreshCasterSegments", Counter(row.staticRefreshes)}, {"dynamicRefreshCasterSegments", Counter(row.dynamicRefreshes)}, {"reuseRegionCount", Counter(row.shadowReuses)},
						{"staticReuseRegionCount", staticAvailable ? Number(row.staticShadowExecution.copyOverlay && row.staticShadowExecution.staticReused ? 1 : 0) : unavailable},
						{"contentOwner", Text(row.contentOwner)}, {"refreshReason", Text(row.refreshReason)}, {"fallbackReason", Text(row.fallbackReason)}}},
					{"memory", {{"staticCacheBudgetRequestedBytes", staticAvailable ? Number(row.staticCacheBudgetRequestedBytes) : unavailable}, {"staticCacheBudgetEffectiveBytes", staticAvailable ? Number(row.staticCacheBudgetEffectiveBytes) : unavailable}, {"reservedBytes", staticAvailable ? Counter(row.staticReservedBytes) : unavailable},
						{"activeBytes", staticAvailable ? Counter(row.staticActiveBytes) : unavailable}, {"retiringBytes", staticAvailable ? Counter(row.staticRetiringBytes) : unavailable},
						// Native static images borrow the existing color attachment and
						// allocate no auxiliary GPU storage. Availability qualifies zero.
						{"auxiliaryBytes", staticAvailable ? Number(0) : unavailable}, {"draining", staticAvailable ? Json(row.staticCacheDraining) : Json("Unavailable")},
						{"admissionReason", staticAvailable ? Text(row.staticShadowExecution.copyOverlay ? "StaticCopyOverlay" : row.staticShadowExecution.fallbackReason) : unavailable},
						{"fallbackReason", staticAvailable ? Text(row.staticShadowExecution.fallbackReason) : unavailable}}},
					{"depthStageTimingNanoseconds", Timing(row.lightingDepthTiming)}, {"totalFrameTimingNanoseconds", Unavailable("No completion-qualified total-frame GPU timing producer.")}, {"artifacts", artifacts}};
				result.frames.push_back(std::move(frame)); for(const auto& artifact : artifacts) result.artifacts.push_back(artifact);
			}
			output = std::move(result); error.clear(); return true;
		}
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}
}
