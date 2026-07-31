#include "TestHarness.h"

#include "File Formats/TerraEngineShaderFile.h"
#include "Resource/ResourceManager.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		class TemporaryShaderFile final
		{
			public:
			TemporaryShaderFile()
			{
				const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
				path = std::filesystem::temp_directory_path() / ("terra-vg-provenance-" + std::to_string(stamp) + ".tspf");
			}

			~TemporaryShaderFile()
			{
				std::error_code error;
				std::filesystem::remove(path, error);
			}

			std::filesystem::path path;
		};

		void WriteShaderWithProvenance(
			const std::filesystem::path &path,
			std::string_view sourcePath = "Engine/Shaders/ProvenanceTest.comp.hlsl",
			ShaderStage stage = ShaderStage::Compute)
		{
			std::string strings;
			const auto AddString = [&strings](std::string_view value)
			{
				const std::uint32_t offset = static_cast<std::uint32_t>(strings.size());
				strings.append(value);
				strings.push_back('\0');
				return offset;
			};
			const std::uint32_t sourceOffset = AddString(sourcePath);
			const std::uint32_t entryOffset = AddString("main");
			const std::uint32_t targetOffset = AddString("cs_6_8");
			const std::vector<std::uint8_t> bytecode{ 0x03u, 0x02u, 0x23u, 0x07u };
			ShaderHeader header{};
			std::memcpy(header.magic, "TSPF", sizeof(header.magic));
			header.version = kShaderCurrentVersion;
			header.headerSize = sizeof(ShaderHeader);
			header.reflectionOffset = header.headerSize;
			header.compiledBinaryOffset = header.reflectionOffset + sizeof(ShaderReflection);
			header.compiledBinarySize = static_cast<std::uint32_t>(bytecode.size());
			header.stringTableOffset = header.compiledBinaryOffset + sizeof(CompiledStageBinary) + static_cast<std::uint32_t>(bytecode.size());
			header.stringTableSize = static_cast<std::uint32_t>(strings.size());

			ShaderReflection reflection{};
			reflection.source.language = static_cast<std::uint32_t>(ShaderSourceLanguage::HLSL);
			reflection.source.stage = static_cast<std::uint32_t>(stage);
			reflection.source.pathOffset = sourceOffset;
			reflection.source.entryPointOffset = entryOffset;
			reflection.source.targetProfileOffset = targetOffset;

			CompiledStageBinary binary{};
			binary.format = static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV);
			binary.bytecodeOffset = header.compiledBinaryOffset + sizeof(CompiledStageBinary);
			binary.bytecodeSize = static_cast<std::uint32_t>(bytecode.size());

			ShaderProvenanceBackendVariantExtension provenance{};
			provenance.normalizedSourceHash = 11u;
			provenance.interfaceAbiHash = 22u;
			provenance.spirvAvailable = 1u;
			provenance.dxilAvailable = 0u;
			provenance.spirvHash = 33u;
			ShaderProvenanceExtensionHeader extensionHeader{};
			extensionHeader.payloadSize = sizeof(provenance);

			std::ofstream output(path, std::ios::binary);
			output.write(reinterpret_cast<const char *>(&header), sizeof(header));
			output.write(reinterpret_cast<const char *>(&reflection), sizeof(reflection));
			output.write(reinterpret_cast<const char *>(&binary), sizeof(binary));
			output.write(reinterpret_cast<const char *>(bytecode.data()), static_cast<std::streamsize>(bytecode.size()));
			output.write(strings.data(), static_cast<std::streamsize>(strings.size()));
			output.write(reinterpret_cast<const char *>(&extensionHeader), sizeof(extensionHeader));
			output.write(reinterpret_cast<const char *>(&provenance), sizeof(provenance));
		}
	} // namespace

	void RegisterVirtualGeometryShaderProvenanceTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.ShaderProvenance.LoadsTaggedBackendAvailability",
			[](TestContext &context)
			{
				TemporaryShaderFile file;
				WriteShaderWithProvenance(file.path);
				ResourceManager &manager = ResourceManager::Instance();
				const TerraEngine::UUID id = manager.LoadShader(file.path);
				Shader *shader = manager.GetShader(id);
				context.Expect(
					id != TerraEngine::EMPTY_UUID && shader != nullptr, "A valid tagged provenance extension should load with its v1 shader payload.");
				context.Expect(shader->provenance.has_value(), "The shader loader should expose the validated TSPV record.");
				context.Expect(shader->provenance->spirvAvailable == 1u && shader->provenance->dxilAvailable == 0u,
					"Provenance must explicitly retain unavailable DXIL rather than infer it.");
			});

		registry.Add("VirtualGeometry.ShaderProvenance.RejectsUnavailableBackendHash",
			[](TestContext &context)
			{
				ShaderProvenanceBackendVariantExtension provenance{};
				provenance.dxilAvailable = 0u;
				provenance.dxilHash = 1u;
				context.Expect(!IsValidShaderProvenanceExtension(provenance), "An unavailable backend must not carry a misleading compiled hash.");
			});

		registry.Add("VirtualGeometry.ShaderProvenance.AcceptsWindowsEngineGeometryPath",
			[](TestContext &context)
			{
				TemporaryShaderFile file;
				// Windows compilers preserve native separators in TSPF source
				// provenance. The runtime must still recognize the engine-owned
				// mesh stage after the artifact moves into an install tree.
				WriteShaderWithProvenance(
					file.path,
					"C:\\TerraEngine\\Engine\\Shaders\\Default\\Default.mesh.hlsl",
					ShaderStage::Mesh);
				ResourceManager &manager = ResourceManager::Instance();
				const TerraEngine::UUID id = manager.LoadShader(file.path);
				context.Expect(
					id != TerraEngine::EMPTY_UUID && manager.GetShader(id) != nullptr,
					"Native Windows source separators must not make an engine mesh shader look like unsupported authored geometry.");
			});
	}
} // namespace TerraEngine::Tests
