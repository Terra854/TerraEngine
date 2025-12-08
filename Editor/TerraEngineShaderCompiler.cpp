#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>

// Offline TSPF compiler: wraps DXC to compile HLSL into TerraEngine's shader container
// (SPIR-V by default) so editor users do not need to install toolchains separately.

#include "Engine/File Formats/TerraEngineShaderFile.h"

namespace
{
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		std::string entryPoint = "main";
		std::string stage;
		std::string targetProfile;
		std::filesystem::path dxcPathOverride;
	};

	std::string ToLower(std::string s)
	{
		for(char& c : s)
		{
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string flag{argv[i]};
			if((flag == "-i" || flag == "--input") && i + 1 < argc)
			{
				args.input = argv[++i];
			}
			else if((flag == "-o" || flag == "--output") && i + 1 < argc)
			{
				args.output = argv[++i];
			}
			else if((flag == "-e" || flag == "--entry") && i + 1 < argc)
			{
				args.entryPoint = argv[++i];
			}
			else if((flag == "-s" || flag == "--stage") && i + 1 < argc)
			{
				args.stage = argv[++i];
			}
			else if((flag == "-t" || flag == "--target") && i + 1 < argc)
			{
				args.targetProfile = argv[++i];
			}
			else if(flag == "--dxc" && i + 1 < argc)
			{
				args.dxcPathOverride = argv[++i];
			}
			else
			{
				std::cerr << "Unrecognized or incomplete argument: " << flag << "\n";
				return false;
			}
		}

		if(args.input.empty() || args.output.empty() || args.stage.empty())
		{
			std::cerr << "Usage: TerraEngineShaderCompiler -i <input.hlsl> -o <output.tspf> -s <vs|ps|cs|gs|hs|ds|ms>\n";
			return false;
		}

		return true;
	}

	std::string DefaultProfileForStage(const std::string& stage)
	{
		const std::string s = ToLower(stage);
		if(s == "vs") return "vs_6_8";
		if(s == "ps" || s == "fs") return "ps_6_8";
		if(s == "cs") return "cs_6_8";
		if(s == "gs") return "gs_6_8";
		if(s == "hs") return "hs_6_8";
		if(s == "ds") return "ds_6_8";
		if(s == "ms") return "ms_6_8";
		if(s == "ts" || s == "as") return "as_6_8";
		return "vs_6_8";
	}

	std::optional<ShaderStage> StageEnum(const std::string& stage)
	{
		const std::string s = ToLower(stage);
		if(s == "vs") return ShaderStage::Vertex;
		if(s == "ps" || s == "fs") return ShaderStage::Fragment;
		if(s == "cs") return ShaderStage::Compute;
		if(s == "gs") return ShaderStage::Geometry;
		if(s == "hs") return ShaderStage::TessellationControl;
		if(s == "ds") return ShaderStage::TessellationEvaluation;
		if(s == "ms") return ShaderStage::Mesh;
		if(s == "ts" || s == "as") return ShaderStage::Task;
		return std::nullopt;
	}

	std::filesystem::path DefaultDxcPath()
	{
#if defined(_WIN32)
		return std::filesystem::path("External/dxc/windows/bin/x64/dxc.exe");
#else
		return std::filesystem::path("External/dxc/linux/bin/dxc");
#endif
	}

	std::string Quote(const std::filesystem::path& p)
	{
		std::ostringstream oss;
		oss << "\"" << p.string() << "\"";
		return oss.str();
	}

	bool RunDxc(const CommandLine& args, const std::filesystem::path& dxcPath, const std::filesystem::path& tempSpv)
	{
		// Rely on the vendored DXC binaries shipped with the repo so the tool stays self-contained.
		const std::string target = args.targetProfile.empty() ? DefaultProfileForStage(args.stage) : args.targetProfile;
		std::ostringstream cmd;
		cmd << Quote(dxcPath) << " -spirv -fspv-target-env=vulkan1.4"
			<< " -T " << target
			<< " -E " << args.entryPoint
			<< " -Fo " << Quote(tempSpv)
			<< " " << Quote(args.input);

		std::cout << "Running: " << cmd.str() << "\n";
		const int code = std::system(cmd.str().c_str());
		if(code != 0)
		{
			std::cerr << "dxc failed with exit code " << code << "\n";
			return false;
		}
		return true;
	}

	struct StringTableBuilder
	{
		std::vector<char> data;

		std::uint32_t Add(const std::string& value)
		{
			// Return the offset before appending so other structures can reference the string table.
			const std::uint32_t offset = static_cast<std::uint32_t>(data.size());
			data.insert(data.end(), value.begin(), value.end());
			data.push_back('\0');
			return offset;
		}
	};
}

int main(int argc, char** argv)
{
	CommandLine args{};
	if(!ParseCommandLine(argc, argv, args))
	{
		return 1;
	}

	const auto stageOpt = StageEnum(args.stage);
	if(!stageOpt)
	{
		std::cerr << "Unsupported stage: " << args.stage << "\n";
		return 1;
	}

	const std::filesystem::path dxcPath = args.dxcPathOverride.empty() ? DefaultDxcPath() : args.dxcPathOverride;
	if(!std::filesystem::exists(dxcPath))
	{
		std::cerr << "Cannot find dxc at: " << dxcPath << "\n";
		return 1;
	}

	const std::filesystem::path tempSpv = std::filesystem::path(args.output).replace_extension(".spv");
	if(!RunDxc(args, dxcPath, tempSpv))
	{
		return 1;
	}

	std::ifstream spvFile(tempSpv, std::ios::binary | std::ios::ate);
	if(!spvFile)
	{
		std::cerr << "Failed to read compiled bytecode: " << tempSpv << "\n";
		return 1;
	}
	const std::streamsize spvSize = spvFile.tellg();
	spvFile.seekg(0);
	std::vector<std::uint8_t> bytecode(static_cast<std::size_t>(spvSize));
	spvFile.read(reinterpret_cast<char*>(bytecode.data()), spvSize);
	std::filesystem::remove(tempSpv);

	StringTableBuilder strings;
	const std::uint32_t pathOffset = strings.Add(args.input.string());
	const std::uint32_t entryOffset = strings.Add(args.entryPoint);
	const std::string targetProfile = args.targetProfile.empty() ? DefaultProfileForStage(args.stage) : args.targetProfile;
	const std::uint32_t targetOffset = strings.Add(targetProfile);

	ShaderReflection reflection{};
	reflection.source.language = static_cast<std::uint32_t>(ShaderSourceLanguage::HLSL);
	reflection.source.stage = static_cast<std::uint32_t>(*stageOpt);
	reflection.source.pathOffset = pathOffset;
	reflection.source.entryPointOffset = entryOffset;
	reflection.source.targetProfileOffset = targetOffset;
	reflection.descriptorSetOffset = 0;
	reflection.descriptorSetCount = 0;
	reflection.pushConstantOffset = 0;
	reflection.pushConstantCount = 0;

	CompiledStageBinary binary{};
	binary.format = static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV);
	binary.entryPointOffset = entryOffset;
	binary.targetProfileOffset = targetOffset;

	const std::uint32_t headerSize = static_cast<std::uint32_t>(sizeof(ShaderHeader));
	const std::uint32_t reflectionOffset = headerSize;
	const std::uint32_t compiledOffset = reflectionOffset + static_cast<std::uint32_t>(sizeof(ShaderReflection));
	const std::uint32_t bytecodeOffset = compiledOffset + static_cast<std::uint32_t>(sizeof(CompiledStageBinary));
	binary.bytecodeOffset = bytecodeOffset;
	binary.bytecodeSize = static_cast<std::uint32_t>(bytecode.size());

	const std::uint32_t stringOffset = bytecodeOffset + binary.bytecodeSize;
	const std::uint32_t stringSize = static_cast<std::uint32_t>(strings.data.size());

	ShaderHeader header{};
	header.magic[0] = 'T';
	header.magic[1] = 'S';
	header.magic[2] = 'P';
	header.magic[3] = 'F';
	header.version = kShaderCurrentVersion;
	header.headerSize = static_cast<std::uint16_t>(sizeof(ShaderHeader));
	header.descriptorSetCount = 0;
	header.descriptorBindingCount = 0;
	header.pushConstantCount = 0;
	header.stringTableOffset = stringOffset;
	header.stringTableSize = stringSize;
	header.reflectionOffset = reflectionOffset;
	header.descriptorSetsOffset = 0;
	header.descriptorBindingsOffset = 0;
	header.pushConstantsOffset = 0;
	header.compiledBinaryOffset = compiledOffset;
	header.compiledBinarySize = binary.bytecodeSize;

	std::vector<std::uint8_t> storage;
	storage.resize(stringOffset + stringSize);
	std::memcpy(storage.data() + reflectionOffset, &reflection, sizeof(reflection));
	std::memcpy(storage.data() + compiledOffset, &binary, sizeof(binary));
	std::memcpy(storage.data() + bytecodeOffset, bytecode.data(), bytecode.size());
	std::memcpy(storage.data() + stringOffset, strings.data.data(), strings.data.size());

	std::ofstream out(args.output, std::ios::binary);
	if(!out)
	{
		std::cerr << "Failed to open output file: " << args.output << "\n";
		return 1;
	}
	out.write(reinterpret_cast<const char*>(&header), sizeof(header));
	out.write(reinterpret_cast<const char*>(storage.data()), static_cast<std::streamsize>(storage.size()));
	std::cout << "Wrote TSPF: " << args.output << " (" << bytecode.size() << " bytes SPIR-V)\n";
	return 0;
}
