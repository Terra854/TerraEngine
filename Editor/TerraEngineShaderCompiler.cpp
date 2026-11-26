// TerraEngineShaderCompiler.cpp — Contains the Terra Engine Shader Compiler compiler pipeline implementation for module-specific declarations, data, and implementation details.

#include <iostream>
#include <string>
#include <string_view>
#include <filesystem>
#include <climits>
#include <iomanip>
#include "Editor/AssetCompilers/ShaderCompiler.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace
{
#if defined(_WIN32)
	using CommandCharacter = wchar_t;
#else
	using CommandCharacter = char;
#endif

	// Keep paths in the operating system's native character type. Only option
	// names and shader text identifiers need UTF-8; narrowing a Windows path
	// through char** would replace characters outside the active ANSI code page.
	bool ReadCommandText(const CommandCharacter* value, std::string& text)
	{
#if defined(_WIN32)
		const std::wstring_view wide(value);
		if(wide.empty()) { text.clear(); return true; }
		if(wide.size() > static_cast<std::size_t>(INT_MAX)) return false;
		const int length = static_cast<int>(wide.size());
		const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			wide.data(), length, nullptr, 0, nullptr, nullptr);
		if(bytes <= 0) return false;
		text.resize(static_cast<std::size_t>(bytes));
		return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			wide.data(), length, text.data(), bytes, nullptr, nullptr) == bytes;
#else
		text = value;
		return true;
#endif
	}

	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		std::string entryPoint = "main";
		std::string stage;
		std::string targetProfile;
		std::filesystem::path dxcPathOverride;
		ShaderUse shaderUse = ShaderUse::Surface;
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
		// The editor parses these JSON lines to update the per-file import bar.
		// Plain text diagnostics are still allowed and are logged separately.
		std::cout
			<< "{\"type\":\"progress\",\"progress\":" << progress
			<< ",\"stage\":\"" << EscapeJson(std::string(stage)) << "\""
			<< ",\"message\":\"" << EscapeJson(std::string(message)) << "\"}"
			<< std::endl;
	}


	bool ParseCommandLine(int argc, CommandCharacter** argv, CommandLine& args)
	{
		for(int i = 1; i < argc; ++i)
		{
			std::string flag;
			if(!ReadCommandText(argv[i], flag))
			{
				std::cerr << "Invalid Unicode option text.\n";
				return false;
			}

			if((flag == "-i" || flag == "--input") && i + 1 < argc)
			{
				args.input = std::filesystem::path(argv[++i]);
			}

			else if((flag == "-o" || flag == "--output") && i + 1 < argc)
			{
				args.output = std::filesystem::path(argv[++i]);
			}

			else if((flag == "-e" || flag == "--entry") && i + 1 < argc)
			{
				if(!ReadCommandText(argv[++i], args.entryPoint)) return false;
			}

			else if((flag == "-s" || flag == "--stage") && i + 1 < argc)
			{
				if(!ReadCommandText(argv[++i], args.stage)) return false;
			}

			else if((flag == "-t" || flag == "--target") && i + 1 < argc)
			{
				if(!ReadCommandText(argv[++i], args.targetProfile)) return false;
			}

			else if(flag == "--shader-use" && i + 1 < argc)
			{
				std::string value;
				if(!ReadCommandText(argv[++i], value)) return false;
				if(value == "surface") args.shaderUse = ShaderUse::Surface;
				else if(value == "post-process") args.shaderUse = ShaderUse::PostProcess;
				else if(value == "source-set") args.shaderUse = ShaderUse::SourceSet;
				else if(value == "compatible-surface") args.shaderUse = ShaderUse::CompatibleSurface;
				else { std::cerr << "Unsupported --shader-use; expected surface, post-process, source-set, or compatible-surface.\n"; return false; }
			}
			else if(flag == "--dxc" && i + 1 < argc)
			{
				args.dxcPathOverride = std::filesystem::path(argv[++i]);
			}
			else
			{
				std::cerr << "Unrecognized or incomplete argument: " << flag << "\n";
				return false;
			}
		}


		if(args.input.empty() || args.output.empty() || args.stage.empty())
		{
			std::cerr << "Usage: TerraEngineShaderCompiler -i <input.hlsl> -o <output.tspf> -s <vs|ps|cs|gs|hs|ds|ms|ts> [--shader-use surface|post-process|source-set|compatible-surface]\n";
			return false;
		}

		return true;
	}


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


// One parser and compile body serve both platforms. Windows receives UTF-16
// directly from its CRT; POSIX retains the existing byte-preserving argv path.
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
	CommandLine args{};

	if(!ParseCommandLine(argc, argv, args))
	{
		return 1;
	}

	const std::filesystem::path exePath = std::filesystem::absolute(argv[0]);
	const std::filesystem::path dxcPath = args.dxcPathOverride.empty() ? DefaultDxcPath(exePath) : args.dxcPathOverride;
	// Prepare the UTF-8 diagnostic before publishing an output file. Streaming a
	// native Windows path directly would narrow it through the ANSI code page
	// and could fail even after a Unicode-path compilation had succeeded.
	std::string outputText;
	if(!ReadCommandText(args.output.c_str(), outputText))
	{
		std::cerr << "Invalid Unicode output path.\n";
		return 1;
	}


	EmitProgress(0.05f, "Shader Compile", "Compiling shader source with DXC.");
	if(CompileShader(args.input, args.output, args.stage, args.targetProfile, args.entryPoint, dxcPath, args.shaderUse))
	{
		EmitProgress(0.98f, "Shader Done", "Shader compile output is ready.");
		std::cout << (args.shaderUse == ShaderUse::SourceSet ? "Wrote SPIR-V: " : "Wrote TSPF: ")
			<< std::quoted(outputText) << "\n";
		return 0;
	}
	else
	{
		return 1;
	}
}
