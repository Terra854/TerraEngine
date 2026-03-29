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
			else
			{
				std::cerr << "Unrecognized argument: " << flag << "\n";
				return false;
			}
		}

		if(args.input.empty() || args.output.empty())
		{
			std::cerr << "Usage: TerraEngineTextureCompiler -i <input image> -o <output" << kCompiledTextureExtension << "> [-s <color|data>]\n";
			std::cerr << "Supported texture source formats: " << GetSupportedTextureSourceExtensionsText() << "\n";
			std::cerr << "Compiled output: " << GetCompiledTextureOutputDescription() << "\n";
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

	if(CompileTexture(args.input, args.output, args.semantic))
	{
		std::cout << "Wrote current TETF with embedded KTX2 payload: " << args.output << "\n";
		return 0;
	}
	else
	{
		return 1;
	}
}
