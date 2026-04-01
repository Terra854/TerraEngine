#include <iostream>
#include <string>
#include <string_view>
#include <filesystem>
#include "Editor/AssetCompilers/TextureCompiler.h"

namespace
{
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		TextureFileSemantic semantic = TextureFileSemantic::Unknown;
		std::uint32_t quality = 1;
		std::string format = "UASTC";
	};

	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string_view flag{argv[i]};
			if((flag == "-i" || flag == "--input") && i + 1 < argc)
			{
				args.input = argv[++i];
			}
			else if((flag == "-o" || flag == "--output") && i + 1 < argc)
			{
				args.output = argv[++i];
			}
			else if((flag == "-s" || flag == "--semantic") && i + 1 < argc)
			{
				std::string_view value = argv[++i];
				if(value == "color")
				{
					args.semantic = TextureFileSemantic::Color;
				}
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
			else if((flag == "-q" || flag == "--quality") && i + 1 < argc)
			{
				args.quality = static_cast<std::uint32_t>(std::stoul(argv[++i]));
			}
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

		if(args.input.empty() || args.output.empty())
		{
			std::cerr << "Usage: TerraEngineTextureCompiler -i <input image> -o <output.tetf> [-s <color|data>] [-q <quality 0-2>] [-f <format>]\n";
			std::cerr << "Supported texture source formats: " << GetSupportedTextureSourceExtensionsText() << "\n";
			return false;
		}

		return true;
	}
}

int main(int argc, char** argv)
{
	CommandLine args {};
	if(!ParseCommandLine(argc, argv, args))
	{
		return 1;
	}

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
