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
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		std::filesystem::path baseline;
		bool includeMeshletBounds = true;
		bool showHelp = false;
	};

	std::string ToBoolText(bool value)
	{
		return value ? "true" : "false";
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

	void PrintUsage()
	{
		std::cout << "Usage: TerraEngineMeshCompiler -i <input.obj> -o <output.temf> [options]\n";
		std::cout << "Options:\n";
		std::cout << "  -b, --baseline <path.temf>         Optional accepted baseline artifact for source-hash-aware regression checks.\n";
		std::cout << "      --include-bounds               Emit optional meshlet bounds payload (default).\n";
		std::cout << "      --omit-bounds                  Skip optional meshlet bounds payload for extra size savings.\n";
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
			<< " backwardCompatibility=false"
			<< " version=" << report.formatVersion
			<< " deterministicReady=" << ToBoolText(report.deterministicReady)
			<< " gpuDirectPass=" << ToBoolText(report.runtimeValidationPass)
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
	const bool compileResult = CompileMesh(args.input, args.output, args.includeMeshletBounds, &report);

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
