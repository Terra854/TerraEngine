#include "TestHarness.h"
#include "VulkanSourceTestUtils.h"

#include "File Formats/TerraEngineMaterialFile.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryRasterBins.h"
#include "Resource/ResourceManager.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		class TemporaryMaterialFile final
		{
			public:
			TemporaryMaterialFile()
			{
				const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
				path = std::filesystem::temp_directory_path() / ("terra-vg-material-" + std::to_string(stamp) + ".tmat");
			}

			~TemporaryMaterialFile()
			{
				std::error_code error;
				std::filesystem::remove(path, error);
			}

			std::filesystem::path path;
		};

		void WriteMaterialWithVirtualGeometryExtension(const std::filesystem::path &path)
		{
			MaterialHeader header{};
			std::memcpy(header.magic, "TMAT", sizeof(header.magic));
			header.version = kMaterialCurrentVersion;
			header.headerSize = sizeof(MaterialHeader);
			header.shadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
			header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
			header.textureArrayOffset = header.headerSize;
			header.floatParameterOffset = header.headerSize;

			VirtualGeometryMaterialExtension extension{};
			extension.programmableFeatureMask = 1u;
			extension.shadingBin = 7u;
			extension.fallbackPolicy = VirtualGeometryMaterialFallbackPolicy::Indexed;
			VirtualGeometryMaterialExtensionHeader extensionHeader{};
			extensionHeader.payloadSize = sizeof(extension);

			std::ofstream output(path, std::ios::binary);
			output.write(reinterpret_cast<const char *>(&header), sizeof(header));
			output.write(reinterpret_cast<const char *>(&extensionHeader), sizeof(extensionHeader));
			output.write(reinterpret_cast<const char *>(&extension), sizeof(extension));
		}
	} // namespace

	void RegisterVirtualGeometryMaterialIntegrationTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Material.OverflowUsesDiagnosedFallback",
			[](TestContext &context)
			{
				using namespace TerraEngine::Renderer;
				VirtualGeometryMaterialExtension extension{};
				extension.programmableFeatureMask = 1u;
				extension.fallbackPolicy = VirtualGeometryMaterialFallbackPolicy::Indexed;
				auto plan = BuildVirtualGeometryRasterBinPlan({ { 1, 2, true, true } }, 1);
				context.Expect(plan.failOpen && plan.overflowCount == 1, "Programmable material bin overflow must retain visible indexed fallback.");
			});

		registry.Add("VirtualGeometry.Material.LoadsTaggedVirtualGeometryPolicy",
			[](TestContext &context)
			{
				TemporaryMaterialFile file;
				WriteMaterialWithVirtualGeometryExtension(file.path);
				ResourceManager &manager = ResourceManager::Instance();
				const TerraEngine::UUID id = manager.LoadMaterial(file.path);
				Material *material = manager.GetMaterial(id);
				context.Expect(id != TerraEngine::EMPTY_UUID && material != nullptr, "A valid tagged TMVG extension should load with its v1 material payload.");
				context.Expect(material->virtualGeometryExtension.has_value() && material->virtualGeometryExtension->shadingBin == 7u,
					"The material loader should expose the validated virtual-geometry policy.");
			});

		registry.Add("VirtualGeometry.Material.RejectsUnknownFallbackPolicy",
			[](TestContext &context)
			{
				VirtualGeometryMaterialExtension extension{};
				extension.fallbackPolicy = static_cast<VirtualGeometryMaterialFallbackPolicy>(99u);
				context.Expect(
					!IsValidVirtualGeometryMaterialExtension(extension), "Corrupt material policy must not select an invented virtual-geometry route.");
			});

		registry.Add("VirtualGeometry.Material.DoubleSidedComputeAndHardwareMultipassRemainComplete",
			[](TestContext& context)
			{
				const std::filesystem::path sourceRoot = TERRAENGINE_SOURCE_DIR;
				const auto ReadSource = [](const std::filesystem::path& path)
				{
					std::ifstream stream(path, std::ios::binary);
					return std::string{
						std::istreambuf_iterator<char>(stream),
						std::istreambuf_iterator<char>() };
				};
				const std::string rasterShader = ReadSource(sourceRoot
					/ "Engine/Shaders/VirtualGeometry/VirtualGeometryComputeRaster.comp.hlsl");
				const std::string setupShader = ReadSource(sourceRoot
					/ "Engine/Shaders/VirtualGeometry/VirtualGeometryTriangleSetup.comp.hlsl");
				const std::string renderer = ReadSource(sourceRoot
					/ "Engine/Renderer/RenderSystem.cpp");
				context.Expect(
					rasterShader.find("TE_VIRTUAL_GEOMETRY_RASTER_FLAG_DOUBLE_SIDED")
						!= std::string::npos
						&& setupShader.find("packet.identity.w") != std::string::npos
						&& renderer.find("singleSidedComputeSurface") == std::string::npos,
					"Default double-sided materials must carry explicit winding policy through GPU setup instead of falling off the compute route.");

				const std::string vulkan = VulkanSourceContracts::ReadContextSources();
				const std::size_t shadowPreparation = vulkan.find("preparedShadowLists");
				const std::size_t computeOwnership = vulkan.find("hasComputeOwnedDraw");
				context.Expect(
					shadowPreparation != std::string::npos
						&& computeOwnership != std::string::npos
						&& shadowPreparation < computeOwnership,
					"Shadow passes must retain the complete hardware draw before main-view compute ownership omits duplicate color coverage.");
			});
	}
} // namespace TerraEngine::Tests
