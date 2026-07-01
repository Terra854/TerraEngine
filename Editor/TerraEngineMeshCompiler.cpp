// TerraEngineMeshCompiler.cpp — Contains the Terra Engine Mesh Compiler compiler pipeline implementation for module-specific declarations, data, and implementation details.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Editor/AssetCompilers/MeshCompiler.h"

namespace
{
	// struct CommandLine groups related state and operations so callers have one documented contract.
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		std::filesystem::path baseline;
		bool includeMeshletBounds = true;
		bool showHelp = false;
		float smoothingAngle = 60.0f;
		float weldThreshold = 1e-6f;
	};

	// ToBoolText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// BuildMetadataPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path BuildMetadataPath(const std::filesystem::path& artifactPath)
	{
		return std::filesystem::path(artifactPath.string() + ".meshreport");
	}

	// TryParseUInt64 is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryParseUInt64(std::string_view text, std::uint64_t& outValue)
	{
		if (text.empty())
		{
			return false;
		}

		std::uint64_t value = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (char c : text)
		{
			if (c < '0' || c > '9')
			{
				return false;
			}

			const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
			{
				return false;
			}

			value = value * 10u + digit;
		}

		outValue = value;
		return true;
	}

	// PrintUsage is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
		std::cout << "  -h, --help                         Show this help message.\n";
	}

	// ParseCommandLine is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string_view flag{ argv[i] };
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (flag == "-h" || flag == "--help")
			{
				args.showHelp = true;
				return true;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (flag == "-i" || flag == "--input")
			{
				if (i + 1 >= argc)
				{
					std::cerr << "Missing value after " << flag << ".\n";
					return false;
				}

				args.input = argv[++i];
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (flag == "-o" || flag == "--output")
			{
				if (i + 1 >= argc)
				{
					std::cerr << "Missing value after " << flag << ".\n";
					return false;
				}

				args.output = argv[++i];
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (flag == "-b" || flag == "--baseline")
			{
				if (i + 1 >= argc)
				{
					std::cerr << "Missing value after " << flag << ".\n";
					return false;
				}

				args.baseline = argv[++i];
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (flag == "--include-bounds" || flag == "--includeMeshletBounds")
			{
				args.includeMeshletBounds = true;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (flag == "--omit-bounds" || flag == "--excludeMeshletBounds")
			{
				args.includeMeshletBounds = false;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (flag == "--smoothing-angle" && i + 1 < argc)
			{
				args.smoothingAngle = std::stof(argv[++i]);
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (flag == "--weld-threshold" && i + 1 < argc)
			{
				args.weldThreshold = std::stof(argv[++i]);
			}
			else
			{
				std::cerr << "Unrecognized argument: " << flag << "\n";
				return false;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (args.showHelp)
		{
			return true;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (args.input.empty() || args.output.empty())
		{
			std::cerr << "Input/output arguments are required.\n";
			return false;
		}

		return true;
	}

	// EscapeJson is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string EscapeJson(const std::string& value)
	{
		std::string escaped;
		escaped.reserve(value.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

	// BuildReportJson is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// WriteMetadataFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void WriteMetadataFile(const std::filesystem::path& outputArtifactPath, const MeshCompileReport& report)
	{
		const std::filesystem::path metadataPath = BuildMetadataPath(outputArtifactPath);
		std::ofstream out(metadataPath, std::ios::trunc);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// LoadMetadataFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool LoadMetadataFile(const std::filesystem::path& metadataPath, std::unordered_map<std::string, std::string>& outValues)
	{
		outValues.clear();
		std::ifstream in(metadataPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!in)
		{
			return false;
		}

		std::string line;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (std::getline(in, line))
		{
			const std::size_t equalsIndex = line.find('=');
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// ApplyBaselinePolicy is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ApplyBaselinePolicy(const CommandLine& args, MeshCompileReport& report)
	{
		report.baselineBytes = 0;
		report.baselinePass = true;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (args.baseline.empty())
		{
			return;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::filesystem::exists(args.baseline))
		{
			std::cerr << "Baseline file not found: " << args.baseline << "\n";
			return;
		}

		report.baselineBytes = static_cast<std::uint64_t>(std::filesystem::file_size(args.baseline));

		bool shouldEnforceRegressionCheck = true;
		const std::filesystem::path baselineMetadataPath = BuildMetadataPath(args.baseline);
		std::unordered_map<std::string, std::string> metadataValues;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (LoadMetadataFile(baselineMetadataPath, metadataValues))
		{
			const auto sourceHashIt = metadataValues.find("sourceHashHex");
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (sourceHashIt != metadataValues.end()
				&& !report.sourceHashHex.empty()
				&& sourceHashIt->second != report.sourceHashHex)
			{
				// Source changed, so prior size no longer represents a fair regression baseline.
				shouldEnforceRegressionCheck = false;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (shouldEnforceRegressionCheck)
		{
			report.baselinePass = report.outputBytes <= report.baselineBytes;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!report.baselinePass
				&& report.status != "compile_fail"
				&& report.status != "budget_fail")
			{
				report.status = "validation_fail";
				report.failureReason = "baseline-regression";
			}
		}
	}

	// EmitCompileReport is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void EmitCompileReport(const MeshCompileReport& report)
	{
		std::cout << "CompileReport: " << BuildReportJson(report) << "\n";
		std::cout
			<< "Summary: directReplacement=true"
			<< " backwardCompatibility=false"
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

// main is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
int main(int argc, char** argv)
{
	CommandLine args{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ParseCommandLine(argc, argv, args))
	{
		PrintUsage();
		return 1;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (args.showHelp)
	{
		PrintUsage();
		return 0;
	}

	MeshCompileReport report{};
	const bool compileResult = CompileMesh(args.input, args.output, args.includeMeshletBounds, &report, args.smoothingAngle, args.weldThreshold);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (compileResult || std::filesystem::exists(args.output))
	{
		ApplyBaselinePolicy(args, report);
		WriteMetadataFile(args.output, report);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!accepted)
	{
		std::cerr << "Compilation failed acceptance gates for '" << args.input << "'.\n";
		return 1;
	}

	return 0;
}
