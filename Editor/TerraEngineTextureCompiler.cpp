// TerraEngineTextureCompiler.cpp — Contains the Terra Engine Texture Compiler compiler pipeline implementation for module-specific declarations, data, and implementation details.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
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
		bool generateMipmaps = true;
		TextureFilterType filter = TextureFilterType::Lanczos;
		bool showHelp = false;
	};

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
		// Keep the child compiler's progress machine-readable so the editor can
		// drive per-file bars while still logging ordinary stdout diagnostics.
		std::cout
			<< "{\"type\":\"progress\",\"progress\":" << progress
			<< ",\"stage\":\"" << EscapeJson(std::string(stage)) << "\""
			<< ",\"message\":\"" << EscapeJson(std::string(message)) << "\"}"
			<< std::endl;
	}

	void PrintUsage()
	{
		std::cerr << "Usage: TerraEngineTextureCompiler -i <input image> -o <output.tetf> [-s <color|data>] [-q <quality 0-2>] [-f <format>] [--generate-mipmaps|--no-mipmaps] [--filter <Box|Triangle|Lanczos|Kaiser>]\n";
		std::cerr << "Supported texture source formats: " << GetSupportedTextureSourceExtensionsText() << "\n";
	}

	bool TryParseFilter(std::string_view value, TextureFilterType& outFilter)
	{
		if (value == "Box")
		{
			outFilter = TextureFilterType::Box;
			return true;
		}
		if (value == "Triangle")
		{
			outFilter = TextureFilterType::Triangle;
			return true;
		}
		if (value == "Lanczos")
		{
			outFilter = TextureFilterType::Lanczos;
			return true;
		}
		if (value == "Kaiser")
		{
			outFilter = TextureFilterType::Kaiser;
			return true;
		}

		return false;
	}

	// ParseCommandLine is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string_view flag{argv[i]};
			if(flag == "-h" || flag == "--help")
			{
				args.showHelp = true;
				return true;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			else if((flag == "-i" || flag == "--input") && i + 1 < argc)
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
			else if(flag == "--generate-mipmaps")
			{
				args.generateMipmaps = true;
			}
			else if(flag == "--no-mipmaps")
			{
				args.generateMipmaps = false;
			}
			else if(flag == "--filter" && i + 1 < argc)
			{
				const std::string_view value = argv[++i];
				if(!TryParseFilter(value, args.filter))
				{
					std::cerr << "Invalid filter: " << value << " (expected Box, Triangle, Lanczos, or Kaiser)\n";
					return false;
				}
			}
			else
			{
				std::cerr << "Unrecognized argument: " << flag << "\n";
				return false;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if(args.showHelp)
		{
			return true;
		}

		if(args.input.empty() || args.output.empty())
		{
			PrintUsage();
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

	if(args.showHelp)
	{
		PrintUsage();
		return 0;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	const TextureCompileProgressCallback progressCallback = [](float progress, std::string_view stage, std::string_view message)
	{
		EmitProgress(progress, stage, message);
	};
	if(CompileTexture(args.input, args.output, args.semantic, args.quality, args.generateMipmaps, args.filter, progressCallback))
	{
		std::cout << "Wrote TETF: " << args.output << "\n";
		return 0;
	}
	else
	{
		return 1;
	}
}
