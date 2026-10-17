#include "TestHarness.h"
#include "VulkanSourceTestUtils.h"

#include "File Formats/TerraEngineMaterialFile.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryRasterBins.h"
#include "Resource/ResourceManager.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		// Source checks supplement runtime tests. Missing definitions must fail even
		// when the caller only searches for forbidden text; ordering stays in one file.
		std::string RequiredContractSource(const std::filesystem::path& relative)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relative, std::ios::binary);
			std::string text((std::istreambuf_iterator<char>(input)), {});
			if(!input || input.bad() || text.empty())
				throw std::runtime_error("Required source unavailable: " + relative.generic_string());
			return text;
		}
		std::string RequiredContractRegion(const std::string& text, const char* first, const char* next)
		{
			const auto begin = text.find(first);
			const auto end = begin == std::string::npos ? std::string::npos : text.find(next, begin + std::string(first).size());
			if(begin == std::string::npos || end == std::string::npos || end <= begin)
				throw std::runtime_error(std::string("Required definition boundaries unavailable: ") + first);
			return text.substr(begin, end - begin);
		}

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

				const std::string rasterShader = RequiredContractSource("Engine/Shaders/VirtualGeometry/VirtualGeometryComputeRaster.comp.hlsl");
				const std::string setupShader = RequiredContractSource("Engine/Shaders/VirtualGeometry/VirtualGeometryTriangleSetup.comp.hlsl");
				const auto route = RequiredContractRegion(RequiredContractSource("Engine/Renderer/Frame/RenderVirtualGeometrySelection.cpp"),
					"void RenderSystem::RouteFrameVirtualGeometryPackets(", "void RenderSystem::ValidateFrameVirtualSelectionPacket(");
				bool forbiddenRouteAbsent = true;
				std::size_t closureFiles = 0u;
				// A forbidden route must not hide after relocation into a stage/header.
				for(const auto& entry : std::filesystem::recursive_directory_iterator(sourceRoot / "Engine/Renderer")) {
					if(!entry.is_regular_file()) continue;
					const auto relative = entry.path().lexically_relative(sourceRoot);
					const auto extension = entry.path().extension().string();
					if(extension != ".cpp" && extension != ".h" && extension != ".hpp" && extension != ".inl") continue;
					if(!entry.path().filename().string().starts_with("RenderSystem")
						&& !relative.generic_string().starts_with("Engine/Renderer/Frame/")) continue;
					++closureFiles;
					forbiddenRouteAbsent &= RequiredContractSource(relative).find("singleSidedComputeSurface") == std::string::npos;
				}
				if(closureFiles == 0u) throw std::runtime_error("Required renderer closure is empty");
				context.Expect(
					rasterShader.find("TE_VIRTUAL_GEOMETRY_RASTER_FLAG_DOUBLE_SIDED")
						!= std::string::npos
						&& setupShader.find("packet.identity.w") != std::string::npos
						&& forbiddenRouteAbsent
						&& route.find("candidate.computeSupported =") != std::string::npos,
					"Default double-sided materials must carry explicit winding policy through GPU setup instead of falling off the compute route.");

				// Both ordering markers belong to this one native RenderFrame definition.
				const auto vulkan = RequiredContractRegion(
					RequiredContractSource("Engine/Renderer/RHI/Vulkan/VulkanFrameRendering.cpp"),
					"void VulkanContext::RenderFrame(", "\n}");
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
