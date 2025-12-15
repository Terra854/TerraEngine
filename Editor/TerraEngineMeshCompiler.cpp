#include <iostream>
#include <string>
#include <string_view>
#include <filesystem>
#include "Editor/AssetCompilers/MeshCompiler.h"

namespace
{
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
	};

	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string_view flag{argv[i]};
			if(flag == "-i" || flag == "--input")
			{
				if(i + 1 >= argc)
				{
					std::cerr << "Missing value after " << flag << "\n";
					return false;
				}
				args.input = argv[++i];
			}
			else if(flag == "-o" || flag == "--output")
			{
				if(i + 1 >= argc)
				{
					std::cerr << "Missing value after " << flag << "\n";
					return false;
				}
				args.output = argv[++i];
			}
			else
			{
				std::cerr << "Unrecognized argument: " << flag << "\n";
				return false;
			}
		}

		if(args.input.empty() || args.output.empty())
		{
			std::cerr << "Usage: TerraEngineMeshCompiler -i <input.obj> -o <output.temf>\n";
			return false;
		}

		return true;
	}
}

int main(int argc, char** argv)
{
	CommandLine args{};
	if(!ParseCommandLine(argc, argv, args))
	{
		return 1;
	}

	if(CompileMesh(args.input, args.output))
	{
		std::cout << "Wrote TEMF: " << args.output << "\n";
		return 0;
	}
	else
	{
		return 1;
	}
}