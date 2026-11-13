// ShaderCompilerArgumentTests.cpp - real DXC must receive complete path arguments.
#include "TestHarness.h"
#include "ShaderCompiler.h"
#include "File Formats/TerraEngineShaderFile.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace TerraEngine::EditorTests {
namespace {
	struct ShaderArgumentFixture {
		std::filesystem::path root;
		ShaderArgumentFixture() {
			static std::atomic<unsigned> next{0};
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			for(unsigned attempt = 0; attempt != 512; ++attempt) {
				// The Unicode component is built from wide characters rather than the
				// host's narrow source/execution encoding.
				root = std::filesystem::temp_directory_path() / (L"Terra Shader \u03bb " + std::to_wstring(stamp) + L" " + std::to_wstring(next.fetch_add(1)));
				std::error_code error;
				if(std::filesystem::create_directory(root, error)) return;
				if(error && error != std::errc::file_exists) throw std::runtime_error("Cannot reserve shader argument fixture directory.");
			}
			throw std::runtime_error("Shader argument fixture reservation exhausted.");
		}
		~ShaderArgumentFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
	};
}

void RegisterShaderCompilerArgumentTests(TestRegistry& registry) {
	registry.Add("Editor.AssetPipeline.ShaderCompiler.WindowsSpacedUnicodeArguments", [](TestContext& context) {
#if defined(_WIN32)
		ShaderArgumentFixture fixture;
		const auto source = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
		const auto originalDxc = source / "External/dxc/windows/bin/x64";
		context.Expect(std::filesystem::exists(originalDxc / "dxc.exe"), "The pinned repository DXC is a required compile prerequisite.");
		const auto copiedDxc = fixture.root / L"DXC \u03bb tool";
		// Copy the exact bundled DLL closure; never select a globally installed
		// DXC or write to its source dependency directory.
		std::filesystem::copy(originalDxc, copiedDxc, std::filesystem::copy_options::recursive);
		const auto shaders = fixture.root / "Engine/Shaders";
		std::filesystem::create_directories(shaders / "PostProcess");
		std::filesystem::copy(source / "Engine/Shaders/Geometry", shaders / "Geometry", std::filesystem::copy_options::recursive);
		const auto input = shaders / "PostProcess/HiZGenerate.comp.hlsl";
		std::filesystem::copy_file(source / "Engine/Shaders/PostProcess/HiZGenerate.comp.hlsl", input);
		const auto outputDirectory = fixture.root / L"Output \u03bb shaders";
		std::filesystem::create_directory(outputDirectory);
		const auto output = outputDirectory / "HiZ compiled.tspf";
		// This production shader has real relative Geometry includes. Executable,
		// source, discovered include roots and output all contain spaces/Unicode.
		context.Expect(CompileShader(input, output, "cs", "cs_6_0", "main", copiedDxc / "dxc.exe"),
			"The production shader compiler must preserve complete Windows argument values.");
		std::ifstream file(output, std::ios::binary);
		context.Expect(file.good(), "Successful compilation must publish its TSPF output.");
		const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		context.Expect(bytes.size() >= 4 && bytes[0] == 'T' && bytes[1] == 'S' && bytes[2] == 'P' && bytes[3] == 'F',
			"Argument success must produce a shader artifact rather than an empty file.");
#else
		context.Skip("This regression establishes Windows CRT argument preservation; Windows DXC execution is required.");
#endif
	});
}
}
