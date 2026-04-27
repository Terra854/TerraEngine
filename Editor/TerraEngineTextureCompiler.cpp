// TerraEngineTextureCompiler.cpp — Contains the Terra Engine Texture Compiler compiler pipeline implementation for module-specific declarations, data, and implementation details.

#include <iostream>
#include <string>
#include <string_view>
#include <filesystem>
#include "Editor/AssetCompilers/TextureCompiler.h"

namespace
{
	// struct CommandLine groups related state and operations so callers have one documented contract.
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		TextureFileSemantic semantic = TextureFileSemantic::Unknown;
		std::uint32_t quality = 1;
		std::string format = "UASTC";
	};

	// ParseCommandLine is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string_view flag{argv[i]};
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
			else if((flag == "-s" || flag == "--semantic") && i + 1 < argc)
			{
				std::string_view value = argv[++i];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if(value == "color")
				{
					args.semantic = TextureFileSemantic::Color;
				}
				// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
				else if(value == "data")
				{
					args.semantic = TextureFileSemantic::Data;
				}
				else
				{
					std::cerr << "Invalid semantic: " << value << " (expected 'color' or 'data')\n";
					return false;
				}
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if((flag == "-q" || flag == "--quality") && i + 1 < argc)
			{
				args.quality = static_cast<std::uint32_t>(std::stoul(argv[++i]));
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if((flag == "-f" || flag == "--format") && i + 1 < argc)
			{
				args.format = argv[++i];
			}
			else
			{
				std::cerr << "Unrecognized argument: " << flag << "\n";
				return false;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if(args.input.empty() || args.output.empty())
		{
			std::cerr << "Usage: TerraEngineTextureCompiler -i <input image> -o <output.tetf> [-s <color|data>] [-q <quality 0-2>] [-f <format>]\n";
			std::cerr << "Supported texture source formats: " << GetSupportedTextureSourceExtensionsText() << "\n";
			return false;
		}

		return true;
	}
}

// main is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
int main(int argc, char** argv)
{
	CommandLine args {};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if(!ParseCommandLine(argc, argv, args))
	{
		return 1;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if(CompileTexture(args.input, args.output, args.semantic, args.quality))
	{
		std::cout << "Wrote TETF: " << args.output << "\n";
		return 0;
	}
	else
	{
		return 1;
	}
}
