// AnimationPackValidator.cpp - deep archive validation and structured evidence.

#include "validation/AnimationPackValidator.h"

#include "File Formats/TerraEngineAnimationFile.h"
#include "TEPAK/PackReader.h"
#include "TEPAK/PackTypes.h"
#include "TEPAK/PackValidator.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace TerraEngine::Export
{
	namespace
	{
		constexpr std::uint64_t kMaximumManifestBytes = 64ULL * 1024ULL * 1024ULL;
		constexpr std::uint64_t kMaximumAnimationAssetBytes = 1024ULL * 1024ULL * 1024ULL;
		constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

		struct ManifestDependency final
		{
			std::string guid;
			std::uint32_t expectedAssetType = kUnknownAnimationExportAssetType;
			std::uint32_t expectedGeneration = 0U;
			std::uint64_t expectedFingerprint = 0U;
			bool required = true;
			std::string path;
			std::string assetClass;
		};

		struct ManifestAsset final
		{
			std::string guid;
			std::string path;
			std::string assetClass;
			std::uint32_t assetType = kUnknownAnimationExportAssetType;
			std::uint32_t generation = 0U;
			std::uint64_t size = 0U;
			std::string fileHash;
			std::string envelopeContentHash;
			std::uint16_t formatMajor = 0U;
			std::uint16_t formatMinor = 0U;
			std::uint64_t fingerprint = 0U;
			bool cookedEnvelope = false;
			std::vector<ManifestDependency> dependencies;
		};

		std::string LowercaseAscii(std::string value)
		{
			for(char& character : value)
			{
				if(character >= 'A' && character <= 'Z')
				{
					character = static_cast<char>(character + ('a' - 'A'));
				}
			}
			return value;
		}

		std::string HashBytes(std::span<const std::uint8_t> bytes)
		{
			std::uint64_t hash = kFnvOffset;
			for(const std::uint8_t byte : bytes)
			{
				hash ^= byte;
				hash *= kFnvPrime;
			}
			std::ostringstream output;
			output << std::hex << std::setfill('0') << std::setw(16) << hash;
			return output.str();
		}

		std::string ContentHashToHex(const TerraEngine::Animation::AnimationContentHash& hash)
		{
			std::ostringstream output;
			output << std::hex << std::setfill('0');
			for(const std::uint8_t byte : hash)
			{
				output << std::setw(2) << static_cast<unsigned int>(byte);
			}
			return output.str();
		}

		std::string GuidToString(const TerraEngine::UUID& guid)
		{
			return LowercaseAscii(TerraEngine::ToHexString(guid));
		}

		bool IsGuidText(std::string_view value)
		{
			if(value.size() != 32U) return false;
			bool nonZero = false;
			for(const char character : value)
			{
				if(!std::isxdigit(static_cast<unsigned char>(character))) return false;
				if(character != '0') nonZero = true;
			}
			return nonZero;
		}

		bool IsAnimationClass(std::string_view assetClass)
		{
			return AnimationAssetTypeCode(assetClass).has_value();
		}

		void AddEvidence(
			AnimationPackValidationReport& report,
			std::string rule,
			std::string path,
			bool passed,
			std::string message,
			std::string expected = {},
			std::string actual = {})
		{
			report.evidence.push_back({std::move(rule), std::move(path), passed,
				std::move(message), std::move(expected), std::move(actual)});
		}

		bool ReadBoundedEntry(
			const tepak::PackReader& reader,
			const tepak::PackEntry& entry,
			std::uint64_t limit,
			std::vector<std::uint8_t>& outBytes,
			AnimationPackValidationReport& report,
			std::string_view rule)
		{
			if(entry.logicalSize > limit)
			{
				AddEvidence(report, std::string(rule), entry.normalizedPath, false,
					"The archive entry exceeds its bounded validation read limit.",
					std::to_string(limit), std::to_string(entry.logicalSize));
				return false;
			}
			try
			{
				outBytes = reader.ReadAll(entry.normalizedPath);
				const bool complete = outBytes.size() == entry.logicalSize;
				AddEvidence(report, std::string(rule), entry.normalizedPath, complete,
					complete ? "The bounded archive read reconstructed the complete entry."
						: "The bounded archive read returned an incomplete entry.",
					std::to_string(entry.logicalSize), std::to_string(outBytes.size()));
				return complete;
			}
			catch(const std::exception& exception)
			{
				AddEvidence(report, std::string(rule), entry.normalizedPath, false,
					std::string("The archive entry could not be reconstructed: ") + exception.what());
				return false;
			}
		}

		bool ParseManifestAsset(const nlohmann::json& value, ManifestAsset& output)
		{
			if(!value.is_object()) return false;
			try
			{
				output.guid = LowercaseAscii(value.at("guid").get<std::string>());
				output.path = LowercaseAscii(value.at("path").get<std::string>());
				output.assetClass = value.at("assetClass").get<std::string>();
				output.assetType = value.at("assetType").get<std::uint32_t>();
				output.generation = value.at("generation").get<std::uint32_t>();
				output.size = value.at("size").get<std::uint64_t>();
				output.fileHash = LowercaseAscii(value.at("fileHash").get<std::string>());
				output.envelopeContentHash = LowercaseAscii(value.value("envelopeContentHash", std::string{}));
				output.formatMajor = value.value("formatMajor", static_cast<std::uint16_t>(0U));
				output.formatMinor = value.value("formatMinor", static_cast<std::uint16_t>(0U));
				output.fingerprint = value.value("compatibilityFingerprint", static_cast<std::uint64_t>(0U));
				output.cookedEnvelope = value.value("cookedAnimationEnvelope", false);
				const nlohmann::json& dependencies = value.at("dependencies");
				if(!dependencies.is_array()) return false;
				for(const nlohmann::json& dependencyValue : dependencies)
				{
					ManifestDependency dependency;
					dependency.guid = LowercaseAscii(dependencyValue.at("guid").get<std::string>());
					dependency.expectedAssetType = dependencyValue.at("expectedAssetType").get<std::uint32_t>();
					dependency.expectedGeneration = dependencyValue.at("expectedGeneration").get<std::uint32_t>();
					dependency.expectedFingerprint = dependencyValue.at("expectedCompatibilityFingerprint").get<std::uint64_t>();
					dependency.required = dependencyValue.at("required").get<bool>();
					dependency.path = LowercaseAscii(dependencyValue.value("path", std::string{}));
					dependency.assetClass = dependencyValue.value("assetClass", std::string{});
					output.dependencies.push_back(std::move(dependency));
				}
				return true;
			}
			catch(const std::exception&)
			{
				return false;
			}
		}

		bool JsonContainsForbiddenReference(const nlohmann::json& value)
		{
			if(value.is_string())
			{
				const std::string text = value.get<std::string>();
				// Optional manifest fields legitimately serialize as empty strings.
				// They carry no path, so do not feed them into the fail-closed path
				// validator, where an empty lookup path is correctly invalid.
				if(text.empty()) return false;
				return IsForbiddenAnimationRuntimePath(text);
			}
			if(value.is_array())
			{
				return std::any_of(value.begin(), value.end(), [](const nlohmann::json& child) {
					return JsonContainsForbiddenReference(child);
				});
			}
			if(value.is_object())
			{
				for(auto iterator = value.begin(); iterator != value.end(); ++iterator)
				{
					const std::string key = LowercaseAscii(iterator.key());
					if(key == "source" || key == "sourcepath" || key == "editorpath" || key == "buildpath")
					{
						return true;
					}
					if(JsonContainsForbiddenReference(iterator.value())) return true;
				}
			}
			return false;
		}

		void ValidateEnvelope(
			const ManifestAsset& asset,
			const std::vector<std::uint8_t>& bytes,
			AnimationPackValidationReport& report)
		{
			TerraEngine::Animation::AnimationAssetReader reader;
			const TerraEngine::Animation::AnimationStatus status = reader.Parse(bytes);
			const bool parsed = status == TerraEngine::Animation::AnimationStatus::Ok;
			AddEvidence(report, "animation-envelope", asset.path, parsed,
				parsed ? "The cooked animation envelope passed format, range, chunk, and content-hash checks."
					: "The cooked animation envelope failed checked parsing.",
				"0", std::to_string(static_cast<std::uint16_t>(status)));
			if(!parsed) return;

			const auto& envelope = reader.Envelope();
			const bool identityMatches = GuidToString(envelope.assetId.value) == asset.guid;
			AddEvidence(report, "animation-guid", asset.path, identityMatches,
				"The runtime manifest GUID must match the cooked envelope GUID.",
				asset.guid, GuidToString(envelope.assetId.value));
			const bool versionMatches = envelope.majorVersion == TerraEngine::Animation::kAnimationAssetMajorVersion
				&& envelope.minorVersion <= TerraEngine::Animation::kAnimationAssetMinorVersion
				&& envelope.majorVersion == asset.formatMajor && envelope.minorVersion == asset.formatMinor;
			AddEvidence(report, "animation-format-version", asset.path, versionMatches,
				"Cooked assets must use a supported common-envelope version recorded by the runtime manifest.",
				std::to_string(TerraEngine::Animation::kAnimationAssetMajorVersion) + "."
					+ std::to_string(TerraEngine::Animation::kAnimationAssetMinorVersion),
				std::to_string(envelope.majorVersion) + "." + std::to_string(envelope.minorVersion));
			const bool contentHashMatches = ContentHashToHex(envelope.contentHash) == asset.envelopeContentHash;
			AddEvidence(report, "animation-content-hash", asset.path, contentHashMatches,
				"The runtime manifest must bind to the checked envelope content hash.",
				asset.envelopeContentHash, ContentHashToHex(envelope.contentHash));
			const bool fingerprintMatches = envelope.compatibilityFingerprint == asset.fingerprint;
			AddEvidence(report, "animation-capability-fingerprint", asset.path, fingerprintMatches,
				"The runtime manifest compatibility fingerprint must match the cooked envelope.",
				std::to_string(asset.fingerprint), std::to_string(envelope.compatibilityFingerprint));

			std::set<std::tuple<std::string, std::uint32_t, std::uint32_t>> envelopeEdges;
			for(const auto& dependency : reader.Dependencies())
			{
				envelopeEdges.emplace(GuidToString(dependency.assetId.value),
					dependency.expectedAssetType, dependency.expectedGeneration);
			}
			for(const ManifestDependency& dependency : asset.dependencies)
			{
				if(dependency.expectedAssetType == kUnknownAnimationExportAssetType) continue;
				const bool present = envelopeEdges.contains({dependency.guid,
					dependency.expectedAssetType, dependency.expectedGeneration});
				AddEvidence(report, "animation-envelope-dependency", asset.path, present,
					"Every typed runtime dependency edge must originate in the cooked envelope.",
					dependency.guid, present ? dependency.guid : std::string{});
			}
		}
	}

	std::string AnimationPackValidationReport::SerializeJson() const
	{
		nlohmann::json evidenceJson = nlohmann::json::array();
		for(const AnimationPackEvidence& item : evidence)
		{
			evidenceJson.push_back({
				{"rule", item.rule}, {"path", item.path}, {"passed", item.passed},
				{"message", item.message}, {"expected", item.expected}, {"actual", item.actual}
			});
		}
		return nlohmann::json({
			{"schemaVersion", 1U},
			{"accepted", accepted},
			{"archivePath", archivePath.generic_string()},
			{"checkedArchiveEntries", checkedArchiveEntries},
			{"checkedAnimationAssets", checkedAnimationAssets},
			{"checkedDependencyEdges", checkedDependencyEdges},
			{"evidence", evidenceJson}
		}).dump(2) + "\n";
	}

	AnimationPackValidationReport AnimationPackValidator::ValidateArchive(
		const std::filesystem::path& archivePath)
	{
		AnimationPackValidationReport report;
		report.archivePath = archivePath;

		const tepak::ValidationReport base = tepak::PackValidator::Validate(archivePath, true);
		report.checkedArchiveEntries = base.checkedEntryCount;
		AddEvidence(report, "tepak-structure-integrity", archivePath.generic_string(), base.validationStatus,
			"TEPAK metadata bounds, reconstructed extent hashes, and entry hashes must pass deep readback.",
			"pass", base.validationStatus ? "pass" : "fail");
		for(const tepak::ValidationFinding& finding : base.findings)
		{
			const bool passed = finding.severity != tepak::ValidationSeverity::Error;
			AddEvidence(report, "tepak." + finding.ruleIdentifier, finding.entryPath, passed,
				finding.message, {}, finding.location);
		}
		if(!base.validationStatus)
		{
			report.accepted = false;
			return report;
		}

		tepak::PackReader reader;
		try
		{
			reader = tepak::PackReader::Open(archivePath);
		}
		catch(const std::exception& exception)
		{
			AddEvidence(report, "animation-archive-open", archivePath.generic_string(), false, exception.what());
			report.accepted = false;
			return report;
		}

		std::unordered_map<std::string, const tepak::PackEntry*> entries;
		std::set<std::string> actualAnimationPaths;
		for(const tepak::PackEntry& entry : reader.GetEntries())
		{
			entries.emplace(entry.normalizedPath, &entry);
			const bool sourceFree = !IsForbiddenAnimationRuntimePath(entry.normalizedPath);
			AddEvidence(report, "runtime-source-absence", entry.normalizedPath, sourceFree,
				"Packaged runtime entries must not contain source, editor, build-only, or import-sidecar paths.");

			const std::string expectedClass = AnimationAssetClassForPath(entry.normalizedPath);
			if(!expectedClass.empty())
			{
				actualAnimationPaths.insert(entry.normalizedPath);
				const bool stableClass = entry.assetClass == expectedClass;
				AddEvidence(report, "tepak-animation-class", entry.normalizedPath, stableClass,
					"Each animation extension must decode to its append-only stable TEPAK asset class.",
					expectedClass, entry.assetClass);
			}
			else if(IsAnimationClass(entry.assetClass))
			{
				AddEvidence(report, "tepak-animation-class", entry.normalizedPath, false,
					"An animation TEPAK class may not be attached to an unknown or source extension.",
					"non-animation/other", entry.assetClass);
			}
		}

		if(actualAnimationPaths.empty())
		{
			report.accepted = std::all_of(report.evidence.begin(), report.evidence.end(), [](const auto& item) {
				return item.passed;
			});
			return report;
		}

		const std::string runtimeManifestPath(kAnimationRuntimeManifestPath);
		const auto manifestEntryIt = entries.find(runtimeManifestPath);
		const bool manifestPresent = manifestEntryIt != entries.end();
		AddEvidence(report, "animation-runtime-manifest", runtimeManifestPath, manifestPresent,
			"Archives containing animation assets require the generated runtime manifest.");
		if(!manifestPresent)
		{
			report.accepted = false;
			return report;
		}

		std::vector<std::uint8_t> manifestBytes;
		if(!ReadBoundedEntry(reader, *manifestEntryIt->second, kMaximumManifestBytes,
			manifestBytes, report, "animation-manifest-read"))
		{
			report.accepted = false;
			return report;
		}

		nlohmann::json manifest;
		try
		{
			manifest = nlohmann::json::parse(manifestBytes.begin(), manifestBytes.end());
		}
		catch(const std::exception& exception)
		{
			AddEvidence(report, "animation-manifest-json", runtimeManifestPath, false, exception.what());
			report.accepted = false;
			return report;
		}
		AddEvidence(report, "animation-manifest-source-absence", runtimeManifestPath,
			!JsonContainsForbiddenReference(manifest),
			"Generated runtime manifests must contain archive-relative cooked references only.");

		try
		{
			const bool schemaMatches = manifest.at("schemaVersion").get<std::uint32_t>()
				== kAnimationExportManifestSchemaVersion;
			AddEvidence(report, "animation-manifest-schema", runtimeManifestPath, schemaMatches,
				"The animation runtime-manifest schema must be supported.",
				std::to_string(kAnimationExportManifestSchemaVersion),
				std::to_string(manifest.at("schemaVersion").get<std::uint32_t>()));
			const bool formatMatches = manifest.at("formatMajor").get<std::uint16_t>()
				== TerraEngine::Animation::kAnimationAssetMajorVersion
				&& manifest.at("formatMinor").get<std::uint16_t>()
				<= TerraEngine::Animation::kAnimationAssetMinorVersion;
			AddEvidence(report, "animation-manifest-format", runtimeManifestPath, formatMatches,
				"The archive animation format capability must be supported by this runtime.");
			const nlohmann::json& capabilities = manifest.at("capabilities");
			const bool capabilitiesMatch = capabilities.at("version").get<std::uint32_t>()
				== kAnimationExportCapabilityVersion
				&& capabilities.at("shaderVersion").get<std::uint32_t>()
				== kAnimationExportCapabilityVersion
				&& capabilities.at("cpuFallback").get<bool>()
				&& capabilities.at("headless").get<bool>();
			AddEvidence(report, "animation-runtime-capabilities", runtimeManifestPath, capabilitiesMatch,
				"Packaged animation requires compatible capabilities plus CPU and headless fallbacks.");
		}
		catch(const std::exception& exception)
		{
			AddEvidence(report, "animation-manifest-header", runtimeManifestPath, false,
				std::string("The animation manifest header is missing or malformed: ") + exception.what());
		}

		std::unordered_map<std::string, ManifestAsset> assetsByGuid;
		std::set<std::string> manifestedAnimationPaths;
		try
		{
			const nlohmann::json& assetValues = manifest.at("assets");
			if(!assetValues.is_array()) throw std::runtime_error("assets is not an array");
			for(const nlohmann::json& assetValue : assetValues)
			{
				ManifestAsset asset;
				if(!ParseManifestAsset(assetValue, asset))
				{
					AddEvidence(report, "animation-manifest-asset", runtimeManifestPath, false,
						"An animation runtime-manifest asset record is malformed.");
					continue;
				}
				const bool identityValid = IsGuidText(asset.guid)
					&& !assetsByGuid.contains(asset.guid) && !asset.path.empty()
					&& !IsForbiddenAnimationRuntimePath(asset.path);
				AddEvidence(report, "animation-manifest-identity", asset.path, identityValid,
					"Manifest asset GUIDs and source-free paths must be present and unique.");
				if(!identityValid) continue;

				const auto entryIt = entries.find(asset.path);
				const bool entryPresent = entryIt != entries.end();
				AddEvidence(report, "animation-manifest-entry", asset.path, entryPresent,
					"Every manifest asset must resolve inside the same archive.");
				if(entryPresent)
				{
					std::vector<std::uint8_t> bytes;
					const std::uint64_t limit = asset.cookedEnvelope
						? kMaximumAnimationAssetBytes : kMaximumManifestBytes;
					if(ReadBoundedEntry(reader, *entryIt->second, limit, bytes, report,
						"animation-asset-read"))
					{
						const bool evidenceMatches = bytes.size() == asset.size
							&& HashBytes(bytes) == asset.fileHash
							&& entryIt->second->assetClass == asset.assetClass;
						AddEvidence(report, "animation-asset-evidence", asset.path, evidenceMatches,
							"Manifest size, file hash, and TEPAK class must match reconstructed bytes and metadata.",
							asset.fileHash, HashBytes(bytes));
						if(asset.cookedEnvelope)
						{
							++report.checkedAnimationAssets;
							manifestedAnimationPaths.insert(asset.path);
							ValidateEnvelope(asset, bytes, report);
						}
					}
				}
				assetsByGuid.emplace(asset.guid, std::move(asset));
			}
		}
		catch(const std::exception& exception)
		{
			AddEvidence(report, "animation-manifest-assets", runtimeManifestPath, false,
				std::string("The animation manifest asset table is missing or malformed: ") + exception.what());
		}

		const bool coverageMatches = manifestedAnimationPaths == actualAnimationPaths;
		AddEvidence(report, "animation-archive-coverage", runtimeManifestPath, coverageMatches,
			"Every cooked animation archive entry must appear exactly once in the runtime manifest.",
			std::to_string(actualAnimationPaths.size()), std::to_string(manifestedAnimationPaths.size()));

		for(const auto& [ownerGuid, owner] : assetsByGuid)
		{
			(void)ownerGuid;
			for(const ManifestDependency& dependency : owner.dependencies)
			{
				++report.checkedDependencyEdges;
				const auto targetIt = assetsByGuid.find(dependency.guid);
				const bool present = targetIt != assetsByGuid.end();
				AddEvidence(report, "animation-dependency-closure", owner.path,
					present || !dependency.required,
					"Every required cooked or generated-manifest dependency must resolve in the archive.",
					dependency.guid, present ? dependency.guid : std::string{});
				if(!present) continue;
				const ManifestAsset& target = targetIt->second;
				const bool pathMatches = dependency.path == target.path
					&& dependency.assetClass == target.assetClass;
				AddEvidence(report, "animation-dependency-path", owner.path, pathMatches,
					"A dependency's resolved path and class must match its GUID target.",
					target.path, dependency.path);
				const bool typeMatches = dependency.expectedAssetType == kUnknownAnimationExportAssetType
					|| dependency.expectedAssetType == target.assetType;
				const bool generationMatches = dependency.expectedGeneration == 0U
					|| dependency.expectedGeneration == target.generation;
				const bool fingerprintMatches = dependency.expectedFingerprint == 0U
					|| dependency.expectedFingerprint == target.fingerprint;
				AddEvidence(report, "animation-dependency-compatibility", owner.path,
					typeMatches && generationMatches && fingerprintMatches,
					"Dependency type, generation, and compatibility fingerprint constraints must match the target.");
			}
		}

		try
		{
			std::set<std::string> requiredEntries;
			for(const nlohmann::json& pathValue : manifest.at("requiredRuntimeEntries"))
			{
				requiredEntries.insert(LowercaseAscii(pathValue.get<std::string>()));
			}
			for(const std::string& shaderPath : RequiredAnimationShaderPaths())
			{
				requiredEntries.insert(shaderPath);
			}
			for(const std::string& path : requiredEntries)
			{
				const bool present = !IsForbiddenAnimationRuntimePath(path) && entries.contains(path);
				AddEvidence(report, "animation-required-runtime-entry", path, present,
					"Every declared animation runtime shader or sanitized package manifest must be archived.");
			}

			for(const nlohmann::json& packageValue : manifest.at("packageManifests"))
			{
				const std::string path = LowercaseAscii(packageValue.at("path").get<std::string>());
				const auto entryIt = entries.find(path);
				bool valid = entryIt != entries.end() && !IsForbiddenAnimationRuntimePath(path);
				std::vector<std::uint8_t> bytes;
				if(valid)
				{
					valid = ReadBoundedEntry(reader, *entryIt->second, kMaximumManifestBytes,
						bytes, report, "animation-package-manifest-read")
						&& bytes.size() == packageValue.at("size").get<std::uint64_t>()
						&& HashBytes(bytes) == LowercaseAscii(packageValue.at("fileHash").get<std::string>());
					if(valid)
					{
						const nlohmann::json packageDocument = nlohmann::json::parse(bytes.begin(), bytes.end());
						valid = !JsonContainsForbiddenReference(packageDocument)
							&& packageDocument.at("schemaVersion").get<std::uint32_t>()
								== kAnimationExportManifestSchemaVersion;
					}
				}
				AddEvidence(report, "animation-package-manifest", path, valid,
					"Sanitized package manifests must exist, hash-match, use a supported schema, and omit source references.");
			}
		}
		catch(const std::exception& exception)
		{
			AddEvidence(report, "animation-manifest-runtime-tables", runtimeManifestPath, false,
				std::string("Required runtime or package-manifest tables are malformed: ") + exception.what());
		}

		report.accepted = std::all_of(report.evidence.begin(), report.evidence.end(), [](const auto& item) {
			return item.passed;
		});
		return report;
	}

	AnimationPackValidationReport AnimationPackValidator::Validate(
		const AnimationExportClosure& closure,
		std::uint32_t formatMajor,
		bool cpuFallbackAvailable)
	{
		AnimationPackValidationReport report;
		AddEvidence(report, "format-major", {}, formatMajor == 1U,
			"Animation export requires the supported cooked format major.");
		AddEvidence(report, "dependency-closure", {}, closure.Complete(),
			"Every transitive animation dependency must be cooked into the archive.");
		AddEvidence(report, "cpu-fallback", {}, cpuFallbackAvailable,
			"Headless and renderer-unavailable startup require a CPU fallback.");
		for(const AnimationExportIssue& issue : closure.issues)
		{
			AddEvidence(report, issue.code, issue.path, false, issue.message,
				issue.dependencyGuid, {});
		}
		report.accepted = std::all_of(report.evidence.begin(), report.evidence.end(), [](const auto& item) {
			return item.passed;
		});
		return report;
	}

	bool AnimationPackValidator::WriteEvidence(
		const AnimationPackValidationReport& report,
		const std::filesystem::path& evidencePath,
		std::string& outError)
	{
		outError.clear();
		std::error_code error;
		std::filesystem::create_directories(evidencePath.parent_path(), error);
		if(error)
		{
			outError = "Could not create animation validation evidence directory: " + error.message();
			return false;
		}
		std::ofstream output(evidencePath, std::ios::binary | std::ios::trunc);
		if(!output)
		{
			outError = "Could not create animation validation evidence: " + evidencePath.generic_string();
			return false;
		}
		output << report.SerializeJson();
		if(!output.good())
		{
			outError = "Could not finish animation validation evidence: " + evidencePath.generic_string();
			return false;
		}
		return true;
	}
}
