// TerraEngineMeshCompiler.cpp — Contains the Terra Engine Mesh Compiler compiler pipeline implementation for module-specific declarations, data, and implementation details.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Editor/AssetCompilers/MeshCompiler.h"

namespace
{

	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		std::filesystem::path baseline;
		bool includeMeshletBounds = true;
		bool showHelp = false;
		float smoothingAngle = 60.0f;
		float weldThreshold = 1e-6f;
		std::optional<VirtualGeometryAdaptiveCompileSettings> adaptiveSettings;
	};


	std::string ToBoolText(bool value)
	{
		return value ? "true" : "false";
	}

	std::string ToVirtualGeometryCompileStatusText(VirtualGeometryCompileStatus status)
	{
		switch (status)
		{
		case VirtualGeometryCompileStatus::Cooked:
			return "cooked";
		case VirtualGeometryCompileStatus::Skipped:
			return "skipped";
		case VirtualGeometryCompileStatus::Failed:
			return "failed";
		case VirtualGeometryCompileStatus::NeedsRebuild:
			return "needsRebuild";
		default:
			return "unknown";
		}
	}


	std::filesystem::path BuildMetadataPath(const std::filesystem::path& artifactPath)
	{
		return std::filesystem::path(artifactPath.string() + ".meshreport");
	}


	bool TryParseUInt64(std::string_view text, std::uint64_t& outValue)
	{
		if (text.empty())
		{
			return false;
		}

		std::uint64_t value = 0;

		for (char c : text)
		{
			if (c < '0' || c > '9')
			{
				return false;
			}

			const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');

			if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
			{
				return false;
			}

			value = value * 10u + digit;
		}

		outValue = value;
		return true;
	}

	bool TryParseUInt32(std::string_view text, std::uint32_t& outValue)
	{
		std::uint64_t value = 0u;
		if (!TryParseUInt64(text, value) || value > std::numeric_limits<std::uint32_t>::max())
		{
			return false;
		}

		outValue = static_cast<std::uint32_t>(value);
		return true;
	}

	bool TryParseFloat(std::string_view text, float& outValue)
	{
		try
		{
			std::size_t consumed = 0u;
			const float parsed = std::stof(std::string(text), &consumed);
			if (consumed != text.size())
			{
				return false;
			}

			outValue = parsed;
			return true;
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	VirtualGeometryAdaptiveCompileSettings& EnsureAdaptiveSettings(CommandLine& args)
	{
		if (!args.adaptiveSettings.has_value())
		{
			// Start from project defaults so CLI callers can override one knob
			// without accidentally clearing the rest of the adaptive recipe.
			args.adaptiveSettings = MakeDefaultVirtualGeometryAdaptiveCompileSettings();
		}

		return *args.adaptiveSettings;
	}

	bool ParseAdaptiveTargetRatios(std::string_view text, std::vector<float>& outRatios)
	{
		outRatios.clear();
		std::size_t start = 0u;
		while (start <= text.size())
		{
			const std::size_t comma = text.find(',', start);
			const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
			const std::string_view token = text.substr(start, end - start);
			float ratio = 0.0f;
			if (token.empty() || !TryParseFloat(token, ratio))
			{
				return false;
			}
			outRatios.push_back(ratio);
			if (comma == std::string_view::npos)
			{
				break;
			}
			start = comma + 1u;
		}

		return !outRatios.empty();
	}


	void PrintUsage()
	{
		std::cout << "Usage: TerraEngineMeshCompiler -i <input.mesh> -o <output.temf> [options]\n";
		std::cout << "Supported mesh source formats: " << GetSupportedMeshSourceExtensionsText() << "\n";
		std::cout << "Options:\n";
		std::cout << "  -b, --baseline <path.temf>         Optional accepted baseline artifact for source-hash-aware regression checks.\n";
		std::cout << "      --include-bounds               Emit optional meshlet bounds payload (default).\n";
		std::cout << "      --omit-bounds                  Skip optional meshlet bounds payload for extra size savings.\n";
		std::cout << "      --smoothing-angle <deg>        Angle threshold for normal smoothing (default: 60). Edges sharper than this will be split.\n";
		std::cout << "      --weld-threshold <eps>         Distance threshold for vertex position welding (default: 1e-6).\n";
		std::cout << "      --adaptive-target-ratios <csv> Override virtual geometry target ratios, for example 0.5,0.25.\n";
		std::cout << "      --adaptive-min-source-triangles <n> Minimum source triangles before adaptive levels are generated.\n";
		std::cout << "      --adaptive-min-generated-triangles <n> Minimum triangles allowed in generated adaptive levels.\n";
		std::cout << "      --adaptive-min-meshlets-per-level <n> Minimum meshlets allowed per generated adaptive level.\n";
		std::cout << "      --adaptive-min-triangle-reduction <n> Minimum useful triangle reduction ratio.\n";
		std::cout << "      --adaptive-min-meshlet-reduction <n> Minimum useful meshlet reduction ratio.\n";
		std::cout << "      --adaptive-max-geometric-error <n> Maximum geometric error accepted for adaptive levels.\n";
		std::cout << "      --adaptive-settings-source <text> Label recorded in compile reports for diagnostics.\n";
		std::cout << "  -h, --help                         Show this help message.\n";
	}


	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string_view flag{ argv[i] };

			if (flag == "-h" || flag == "--help")
			{
				args.showHelp = true;
				return true;
			}

			else if (flag == "-i" || flag == "--input")
			{
				if (i + 1 >= argc)
				{
					std::cerr << "Missing value after " << flag << ".\n";
					return false;
				}

				args.input = argv[++i];
			}

			else if (flag == "-o" || flag == "--output")
			{
				if (i + 1 >= argc)
				{
					std::cerr << "Missing value after " << flag << ".\n";
					return false;
				}

				args.output = argv[++i];
			}

			else if (flag == "-b" || flag == "--baseline")
			{
				if (i + 1 >= argc)
				{
					std::cerr << "Missing value after " << flag << ".\n";
					return false;
				}

				args.baseline = argv[++i];
			}

			else if (flag == "--include-bounds" || flag == "--includeMeshletBounds")
			{
				args.includeMeshletBounds = true;
			}

			else if (flag == "--omit-bounds" || flag == "--excludeMeshletBounds")
			{
				args.includeMeshletBounds = false;
			}

			else if (flag == "--smoothing-angle" && i + 1 < argc)
			{
				args.smoothingAngle = std::stof(argv[++i]);
			}

			else if (flag == "--weld-threshold" && i + 1 < argc)
			{
				args.weldThreshold = std::stof(argv[++i]);
			}
			else if (flag == "--adaptive-target-ratios" && i + 1 < argc)
			{
				auto& adaptiveSettings = EnsureAdaptiveSettings(args);
				if (!ParseAdaptiveTargetRatios(argv[++i], adaptiveSettings.targetRatios))
				{
					std::cerr << "Invalid adaptive target ratio list.\n";
					return false;
				}
			}
			else if (flag == "--adaptive-min-source-triangles" && i + 1 < argc)
			{
				if (!TryParseUInt32(argv[++i], EnsureAdaptiveSettings(args).minimumSourceTriangles))
				{
					std::cerr << "Invalid adaptive minimum source triangle count.\n";
					return false;
				}
			}
			else if (flag == "--adaptive-min-generated-triangles" && i + 1 < argc)
			{
				if (!TryParseUInt32(argv[++i], EnsureAdaptiveSettings(args).minimumGeneratedTriangles))
				{
					std::cerr << "Invalid adaptive minimum generated triangle count.\n";
					return false;
				}
			}
			else if (flag == "--adaptive-min-meshlets-per-level" && i + 1 < argc)
			{
				if (!TryParseUInt32(argv[++i], EnsureAdaptiveSettings(args).minimumMeshletsPerGeneratedLevel))
				{
					std::cerr << "Invalid adaptive minimum meshlets per level.\n";
					return false;
				}
			}
			else if (flag == "--adaptive-min-triangle-reduction" && i + 1 < argc)
			{
				if (!TryParseFloat(argv[++i], EnsureAdaptiveSettings(args).minimumUsefulTriangleReduction))
				{
					std::cerr << "Invalid adaptive minimum triangle reduction.\n";
					return false;
				}
			}
			else if (flag == "--adaptive-min-meshlet-reduction" && i + 1 < argc)
			{
				if (!TryParseFloat(argv[++i], EnsureAdaptiveSettings(args).minimumUsefulMeshletReduction))
				{
					std::cerr << "Invalid adaptive minimum meshlet reduction.\n";
					return false;
				}
			}
			else if (flag == "--adaptive-max-geometric-error" && i + 1 < argc)
			{
				if (!TryParseFloat(argv[++i], EnsureAdaptiveSettings(args).maximumGeometricError))
				{
					std::cerr << "Invalid adaptive maximum geometric error.\n";
					return false;
				}
			}
			else if (flag == "--adaptive-settings-source" && i + 1 < argc)
			{
				EnsureAdaptiveSettings(args).settingsSource = argv[++i];
			}
			else
			{
				std::cerr << "Unrecognized argument: " << flag << "\n";
				return false;
			}
		}


		if (args.showHelp)
		{
			return true;
		}


		if (args.input.empty() || args.output.empty())
		{
			std::cerr << "Input/output arguments are required.\n";
			return false;
		}

		return true;
	}


	std::string EscapeJson(const std::string& value)
	{
		std::string escaped;
		escaped.reserve(value.size());

		for (const char c : value)
		{
			switch (c)
			{
			case '\\':
				escaped += "\\\\";
				break;
			case '"':
				escaped += "\\\"";
				break;
			case '\n':
				escaped += "\\n";
				break;
			case '\r':
				escaped += "\\r";
				break;
			case '\t':
				escaped += "\\t";
				break;
			default:
				escaped.push_back(c);
				break;
			}
		}

		return escaped;
	}

	void EmitProgress(float progress, std::string_view stage, std::string_view message)
	{
		// The editor watches stdout for one JSON object per line. Other human
		// diagnostics remain plain text and are logged without affecting the bar.
		std::cout
			<< "{\"type\":\"progress\",\"progress\":" << progress
			<< ",\"stage\":\"" << EscapeJson(std::string(stage)) << "\""
			<< ",\"message\":\"" << EscapeJson(std::string(message)) << "\"}"
			<< std::endl;
	}

	void AppendStringArrayJson(std::ostringstream& json, const std::vector<std::string>& values)
	{
		json << "[";
		for (std::size_t index = 0; index < values.size(); ++index)
		{
			if (index != 0u)
			{
				json << ",";
			}
			json << "\"" << EscapeJson(values[index]) << "\"";
		}
		json << "]";
	}

	void AppendFloatArrayJson(std::ostringstream& json, const std::vector<float>& values)
	{
		json << "[";
		for (std::size_t index = 0; index < values.size(); ++index)
		{
			if (index != 0u)
			{
				json << ",";
			}
			json << values[index];
		}
		json << "]";
	}

	void AppendAdaptiveSettingsJson(std::ostringstream& json, const VirtualGeometryAdaptiveCompileSettings& settings)
	{
		// Keep every cook input visible in the CLI report so automated import
		// tests can prove whether a mesh used project defaults or an override.
		json << "{";
		json << "\"settingsSource\":\"" << EscapeJson(settings.settingsSource) << "\",";
		json << "\"targetRatios\":";
		AppendFloatArrayJson(json, settings.targetRatios);
		json << ",\"minimumSourceTriangles\":" << settings.minimumSourceTriangles;
		json << ",\"minimumGeneratedTriangles\":" << settings.minimumGeneratedTriangles;
		json << ",\"minimumMeshletsPerGeneratedLevel\":" << settings.minimumMeshletsPerGeneratedLevel;
		json << ",\"minimumUsefulTriangleReduction\":" << settings.minimumUsefulTriangleReduction;
		json << ",\"minimumUsefulMeshletReduction\":" << settings.minimumUsefulMeshletReduction;
		json << ",\"maximumGeometricError\":" << settings.maximumGeometricError;
		json << "}";
	}

	void AppendRequestedTargetsJson(std::ostringstream& json, const std::vector<VirtualGeometryRequestedTargetCompileReport>& targets)
	{
		json << "[";
		for (std::size_t index = 0; index < targets.size(); ++index)
		{
			const VirtualGeometryRequestedTargetCompileReport& target = targets[index];
			if (index != 0u)
			{
				json << ",";
			}
			json << "{";
			json << "\"targetIndex\":" << target.targetIndex << ",";
			json << "\"requestedRatio\":" << target.requestedRatio << ",";
			json << "\"desiredTriangleCount\":" << target.desiredTriangleCount << ",";
			json << "\"desiredMeshletCount\":" << target.desiredMeshletCount << ",";
			json << "\"status\":\"" << EscapeJson(target.status) << "\"";
			json << "}";
		}
		json << "]";
	}

	void AppendGeneratedLevelsJson(std::ostringstream& json, const std::vector<VirtualGeometryGeneratedLevelCompileReport>& levels)
	{
		json << "[";
		for (std::size_t index = 0; index < levels.size(); ++index)
		{
			const VirtualGeometryGeneratedLevelCompileReport& level = levels[index];
			if (index != 0u)
			{
				json << ",";
			}
			json << "{";
			json << "\"generatedLevelIndex\":" << level.generatedLevelIndex << ",";
			json << "\"requestedTargetIndex\":" << level.requestedTargetIndex << ",";
			json << "\"lodIndex\":" << level.lodIndex << ",";
			json << "\"actualRatio\":" << level.actualRatio << ",";
			json << "\"triangleCount\":" << level.triangleCount << ",";
			json << "\"meshletCount\":" << level.meshletCount << ",";
			json << "\"geometricError\":" << level.geometricError;
			json << "}";
		}
		json << "]";
	}

	void AppendSkippedLevelsJson(std::ostringstream& json, const std::vector<VirtualGeometrySkippedLevelCompileReport>& levels)
	{
		json << "[";
		for (std::size_t index = 0; index < levels.size(); ++index)
		{
			const VirtualGeometrySkippedLevelCompileReport& level = levels[index];
			if (index != 0u)
			{
				json << ",";
			}
			json << "{";
			json << "\"targetIndex\":" << level.targetIndex << ",";
			json << "\"requestedRatio\":" << level.requestedRatio << ",";
			json << "\"status\":\"" << EscapeJson(level.status) << "\",";
			json << "\"reason\":\"" << EscapeJson(level.reason) << "\",";
			json << "\"candidateTriangleCount\":" << level.candidateTriangleCount << ",";
			json << "\"candidateMeshletCount\":" << level.candidateMeshletCount << ",";
			json << "\"actualCandidateRatio\":" << level.actualCandidateRatio << ",";
			json << "\"nearestGeneratedLevelIndex\":" << level.nearestGeneratedLevelIndex << ",";
			json << "\"message\":\"" << EscapeJson(level.message) << "\"";
			json << "}";
		}
		json << "]";
	}

	void AppendShadowCodecProfilesJson(
		std::ostringstream& json,
		const std::vector<VirtualGeometryShadowCodecCompileReport>& profiles)
	{
		json << "[";
		for(std::size_t index = 0u; index < profiles.size(); ++index)
		{
			if(index != 0u)
			{
				json << ",";
			}
			const auto& profile = profiles[index];
			json << "{";
			json << "\"codec\":\""
				<< EscapeJson(profile.codecProfile) << "\",";
			json << "\"pageSizeKiB\":" << profile.pageSizeKiB << ",";
			json << "\"sourceBytes\":" << profile.sourceBytes << ",";
			json << "\"encodedBytes\":" << profile.encodedBytes << ",";
			json << "\"deterministicHash\":"
				<< profile.deterministicHash << ",";
			json << "\"complete\":" << ToBoolText(profile.complete);
			json << "}";
		}
		json << "]";
	}


	std::string BuildReportJson(const MeshCompileReport& report)
	{
		std::ostringstream ratio;
		ratio << std::fixed << std::setprecision(6) << report.compressionRatio;

		std::ostringstream json;
		json << "{";
		json << "\"assetPath\":\"" << EscapeJson(report.assetPath.string()) << "\",";
		json << "\"formatVersion\":" << report.formatVersion << ",";
		json << "\"sourceHashHex\":\"" << EscapeJson(report.sourceHashHex) << "\",";
		json << "\"sourceBytes\":" << report.sourceBytes << ",";
		json << "\"outputBytes\":" << report.outputBytes << ",";
		json << "\"baselineBytes\":" << report.baselineBytes << ",";
		json << "\"baselinePass\":" << ToBoolText(report.baselinePass) << ",";
		json << "\"compressionRatio\":" << ratio.str() << ",";
		json << "\"deterministicReady\":" << ToBoolText(report.deterministicReady) << ",";
		json << "\"runtimeValidationPass\":" << ToBoolText(report.runtimeValidationPass) << ",";
		json << "\"usedU16RemapEncoding\":" << ToBoolText(report.usedU16RemapEncoding) << ",";
		json << "\"meshletGlobalVertexCount\":" << report.meshletGlobalVertexCount << ",";
		json << "\"virtualGeometry\":{";
		json << "\"status\":\"" << ToVirtualGeometryCompileStatusText(report.virtualGeometry.status) << "\",";
		json << "\"detailLevelCount\":" << report.virtualGeometry.detailLevelCount << ",";
		json << "\"cullableGroupCount\":" << report.virtualGeometry.cullableGroupCount << ",";
		json << "\"pageCount\":" << report.virtualGeometry.pageCount << ",";
		json << "\"sourceTriangleCount\":" << report.virtualGeometry.sourceTriangleCount << ",";
		json << "\"selectedFallbackAvailable\":" << ToBoolText(report.virtualGeometry.selectedFallbackAvailable != 0u) << ",";
		json << "\"maxGeometricError\":" << report.virtualGeometry.maxGeometricError << ",";
		json << "\"adaptiveSettings\":";
		AppendAdaptiveSettingsJson(json, report.virtualGeometry.adaptiveSettings);
		json << ",\"requestedTargets\":";
		AppendRequestedTargetsJson(json, report.virtualGeometry.requestedTargets);
		json << ",\"generatedLevels\":";
		AppendGeneratedLevelsJson(json, report.virtualGeometry.generatedLevels);
		json << ",\"skippedLevels\":";
		AppendSkippedLevelsJson(json, report.virtualGeometry.skippedLevels);
		json << ",\"shadowCodecProfiles\":";
		AppendShadowCodecProfilesJson(
			json, report.virtualGeometry.shadowCodecProfiles);
		json << ",";
		json << "\"warnings\":";
		AppendStringArrayJson(json, report.virtualGeometry.warnings);
		json << ",\"errors\":";
		AppendStringArrayJson(json, report.virtualGeometry.errors);
		json << "},";
		json << "\"status\":\"" << EscapeJson(report.status) << "\",";
		json << "\"failureReason\":\"" << EscapeJson(report.failureReason) << "\"";
		json << "}";
		return json.str();
	}


	void WriteMetadataFile(const std::filesystem::path& outputArtifactPath, const MeshCompileReport& report)
	{
		const std::filesystem::path metadataPath = BuildMetadataPath(outputArtifactPath);
		std::ofstream out(metadataPath, std::ios::trunc);

		if (!out)
		{
			std::cerr << "Warning: failed to write baseline metadata file '" << metadataPath << "'.\n";
			return;
		}

		// A tiny key-value file keeps baseline checks source-hash-aware without requiring extra tooling.
		out << "sourceHashHex=" << report.sourceHashHex << "\n";
		out << "outputBytes=" << report.outputBytes << "\n";
		out << "formatVersion=" << report.formatVersion << "\n";
		out << "status=" << report.status << "\n";
		out << "virtualGeometryStatus=" << ToVirtualGeometryCompileStatusText(report.virtualGeometry.status) << "\n";
		out << "virtualGeometryDetailLevelCount=" << report.virtualGeometry.detailLevelCount << "\n";
		out << "virtualGeometryMeshletCount=" << report.virtualGeometry.cullableGroupCount << "\n";
		out << "virtualGeometrySourceTriangleCount=" << report.virtualGeometry.sourceTriangleCount << "\n";
		out << "virtualGeometryRequestedTargetCount=" << report.virtualGeometry.requestedTargets.size() << "\n";
		out << "virtualGeometryGeneratedLevelCount=" << report.virtualGeometry.generatedLevels.size() << "\n";
		out << "virtualGeometrySkippedLevelCount=" << report.virtualGeometry.skippedLevels.size() << "\n";
		out << "virtualGeometryAdaptiveTargetCount=" << report.virtualGeometry.adaptiveSettings.targetRatios.size() << "\n";
		out << "virtualGeometryWarningCount=" << report.virtualGeometry.warnings.size() << "\n";
	}


	bool LoadMetadataFile(const std::filesystem::path& metadataPath, std::unordered_map<std::string, std::string>& outValues)
	{
		outValues.clear();
		std::ifstream in(metadataPath);

		if (!in)
		{
			return false;
		}

		std::string line;

		while (std::getline(in, line))
		{
			const std::size_t equalsIndex = line.find('=');

			if (equalsIndex == std::string::npos || equalsIndex == 0 || equalsIndex + 1 >= line.size())
			{
				continue;
			}

			const std::string key = line.substr(0, equalsIndex);
			const std::string value = line.substr(equalsIndex + 1);
			outValues[key] = value;
		}

		return true;
	}


	void ApplyBaselinePolicy(const CommandLine& args, MeshCompileReport& report)
	{
		report.baselineBytes = 0;
		report.baselinePass = true;


		if (args.baseline.empty())
		{
			return;
		}


		if (!std::filesystem::exists(args.baseline))
		{
			std::cerr << "Baseline file not found: " << args.baseline << "\n";
			return;
		}

		report.baselineBytes = static_cast<std::uint64_t>(std::filesystem::file_size(args.baseline));

		bool shouldEnforceRegressionCheck = true;
		const std::filesystem::path baselineMetadataPath = BuildMetadataPath(args.baseline);
		std::unordered_map<std::string, std::string> metadataValues;

		if (LoadMetadataFile(baselineMetadataPath, metadataValues))
		{
			const auto sourceHashIt = metadataValues.find("sourceHashHex");

			if (sourceHashIt != metadataValues.end()
				&& !report.sourceHashHex.empty()
				&& sourceHashIt->second != report.sourceHashHex)
			{
				// Source changed, so prior size no longer represents a fair regression baseline.
				shouldEnforceRegressionCheck = false;
			}
		}


		if (shouldEnforceRegressionCheck)
		{
			report.baselinePass = report.outputBytes <= report.baselineBytes;

			if (!report.baselinePass
				&& report.status != "compile_fail"
				&& report.status != "budget_fail")
			{
				report.status = "validation_fail";
				report.failureReason = "baseline-regression";
			}
		}
	}


	void EmitCompileReport(const MeshCompileReport& report)
	{
		std::cout << "CompileReport: " << BuildReportJson(report) << "\n";
		std::cout
			<< "Summary: directReplacement=true"
			<< " canonicalOnly=true"
			<< " version=" << report.formatVersion
			<< " deterministicReady=" << ToBoolText(report.deterministicReady)
			<< " gpuDirectPass=" << ToBoolText(report.runtimeValidationPass)
			<< " virtualGeometryStatus=" << ToVirtualGeometryCompileStatusText(report.virtualGeometry.status)
			<< " virtualGeometryDetailLevels=" << report.virtualGeometry.detailLevelCount
			<< " virtualGeometryMeshlets=" << report.virtualGeometry.cullableGroupCount
			<< " virtualGeometryCullableGroups=" << report.virtualGeometry.cullableGroupCount
			<< " virtualGeometrySourceTriangles=" << report.virtualGeometry.sourceTriangleCount
			<< " virtualGeometryFallbackAvailable=" << ToBoolText(report.virtualGeometry.selectedFallbackAvailable != 0u)
			<< " virtualGeometryRequestedTargets=" << report.virtualGeometry.requestedTargets.size()
			<< " virtualGeometryGeneratedLevels=" << report.virtualGeometry.generatedLevels.size()
			<< " virtualGeometrySkippedLevels=" << report.virtualGeometry.skippedLevels.size()
			<< " virtualGeometryAdaptiveTargets=" << report.virtualGeometry.adaptiveSettings.targetRatios.size()
			<< " virtualGeometryWarnings=" << report.virtualGeometry.warnings.size()
			<< " cpu-repack-used=false"
			<< " baselinePass=" << ToBoolText(report.baselinePass)
			<< " status=" << report.status
			<< "\n";
	}
}


int main(int argc, char** argv)
{
	CommandLine args{};

	if (!ParseCommandLine(argc, argv, args))
	{
		PrintUsage();
		return 1;
	}


	if (args.showHelp)
	{
		PrintUsage();
		return 0;
	}

	MeshCompileReport report{};
	const VirtualGeometryAdaptiveCompileSettings* adaptiveSettings = args.adaptiveSettings.has_value()
		? &(*args.adaptiveSettings)
		: nullptr;
	const MeshCompileProgressCallback progressCallback = [](float progress, std::string_view stage, std::string_view message)
	{
		EmitProgress(progress, stage, message);
	};
	const bool compileResult = CompileMesh(args.input, args.output, args.includeMeshletBounds, &report, args.smoothingAngle, args.weldThreshold, adaptiveSettings, progressCallback);


	if (compileResult || std::filesystem::exists(args.output))
	{
		ApplyBaselinePolicy(args, report);
		WriteMetadataFile(args.output, report);
	}


	if (!report.runtimeValidationPass
		&& report.status != "compile_fail"
		&& report.status != "budget_fail")
	{
		report.status = "validation_fail";
		report.failureReason = "runtime-validation-failed";
	}

	EmitCompileReport(report);

	const bool accepted = compileResult
		&& report.baselinePass
		&& report.runtimeValidationPass;


	if (!accepted)
	{
		std::cerr << "Compilation failed acceptance gates for '" << args.input << "'.\n";
		return 1;
	}

	return 0;
}
