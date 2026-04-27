// TerraEngineShaderCompiler.cpp — Contains the Terra Engine Shader Compiler compiler pipeline implementation for module-specific declarations, data, and implementation details.

#include <iostream>
#include <string>
#include <string_view>
#include <filesystem>
#include "Editor/AssetCompilers/ShaderCompiler.h"

namespace
{
	// struct CommandLine groups related state and operations so callers have one documented contract.
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		std::string entryPoint = "main";
		std::string stage;
		std::string targetProfile;
		std::filesystem::path dxcPathOverride;
	};

	// ParseCommandLine is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string flag{argv[i]};
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if((flag == "-i" || flag == "--input") && i + 1 < argc)
			{
				args.input = argv[++i];
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if((flag == "-o" || flag == "--output") && i + 1 < argc)
			{
				args.output = argv[++i];
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if((flag == "-e" || flag == "--entry") && i + 1 < argc)
			{
				args.entryPoint = argv[++i];
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if((flag == "-s" || flag == "--stage") && i + 1 < argc)
			{
				args.stage = argv[++i];
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if((flag == "-t" || flag == "--target") && i + 1 < argc)
			{
				args.targetProfile = argv[++i];
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if(args.input.empty() || args.output.empty() || args.stage.empty())
		{
			std::cerr << "Usage: TerraEngineShaderCompiler -i <input.hlsl> -o <output.tspf> -s <vs|ps|cs|gs|hs|ds|ms|ts>\n";
			return false;
		}

		return true;
	}

	// DefaultDxcPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path DefaultDxcPath(const std::filesystem::path& executablePath)
	{
#if defined(_WIN32)
		const std::filesystem::path localName = std::filesystem::path("dxc.exe");
#else
		const std::filesystem::path localName = std::filesystem::path("dxc");
#endif
		const std::filesystem::path sibling = executablePath.parent_path() / localName;
		return sibling;
	}
}

// main is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
int main(int argc, char** argv)
{
	CommandLine args{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if(!ParseCommandLine(argc, argv, args))
	{
		return 1;
	}

	const std::filesystem::path exePath = std::filesystem::absolute(argv[0]);
	const std::filesystem::path dxcPath = args.dxcPathOverride.empty() ? DefaultDxcPath(exePath) : args.dxcPathOverride;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if(CompileShader(args.input, args.output, args.stage, args.targetProfile, args.entryPoint, dxcPath))
	{
		std::cout << "Wrote TSPF: " << args.output << "\n";
		return 0;
	}
	else
	{
		return 1;
	}
}
