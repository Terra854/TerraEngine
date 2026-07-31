#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadSource(const char* relativePath)
		{
			// CTest runs from the build tree, so all source contracts must be
			// resolved from the source directory supplied by CMake.
			const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream file(path);
			return { std::istreambuf_iterator<char>(file), {} };
		}
	} // namespace

	void RegisterVirtualGeometryHardwareVisibilityTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Visibility.LivePipelineAttachmentContract",
			[](TestContext &context)
			{
				const std::string meshShader = ReadSource("Engine/Shaders/Default/Default.mesh.hlsl");
				const std::string fragmentShader = ReadSource("Engine/Shaders/Default/Default.frag.hlsl");
				const std::string renderPassSource = ReadSource("Engine/Renderer/RHI/Vulkan/VulkanContextInit.cpp");
				const std::string offscreenSource = ReadSource("Engine/Renderer/RHI/Vulkan/VulkanContext.cpp");
				const std::string implementationSource = ReadSource("Engine/Renderer/RHI/Vulkan/VulkanContextImpl.h");
				const std::string pipelineSource = ReadSource("Engine/Renderer/RHI/Vulkan/VulkanContextResources.cpp");
				context.Expect(!meshShader.empty() && !fragmentShader.empty()
					&& !renderPassSource.empty() && !offscreenSource.empty()
					&& !implementationSource.empty() && !pipelineSource.empty(),
					"Hardware visibility shader and Vulkan pipeline sources must be present");
				context.Expect(meshShader.find("kStableVisibilityPrimitiveStride") != std::string::npos,
					"Mesh output must preserve an explicit stable primitive stride.");
				context.Expect(meshShader.find("out primitives StableVisibilityPrimitive") != std::string::npos
					&& fragmentShader.find("stablePrimitiveId : TEXCOORD6") != std::string::npos,
					"Hardware visibility must carry exact stable identity as a mesh primitive attribute.");
				context.Expect(fragmentShader.find("SV_PrimitiveID") == std::string::npos,
					"The fragment shader must not require Vulkan's optional geometryShader feature.");
				const std::size_t identityVariantGuard =
					fragmentShader.find("#if defined(TERRAENGINE_VULKAN_CONVENTIONAL_VISIBILITY_IDENTITY)");
				const std::size_t identityTarget = fragmentShader.find("SV_Target1");
				const std::size_t identityVariantEnd =
					identityTarget == std::string::npos
						? std::string::npos
						: fragmentShader.find("#endif", identityTarget);
				context.Expect(
					identityVariantGuard != std::string::npos
						&& identityTarget > identityVariantGuard
						&& identityVariantEnd > identityTarget,
					"The live material shader may write its second Vulkan color target only inside the explicitly guarded identity variant.");
				context.Expect(fragmentShader.find("SV_Target0") != std::string::npos,
					"The live material shader must write the primary Vulkan color target.");
				context.Expect(meshShader.find("meshletIndex * kStableVisibilityPrimitiveStride + primitiveIndex") != std::string::npos,
					"Debug identity must combine meshlet and local primitive identity deterministically.");
				context.Expect(meshShader.find("LoadPrimitiveIndex(primitiveOffset + 0u)") != std::string::npos
					&& meshShader.find("LoadPageLocalPrimitiveIndex(descriptor, primitiveOffset + 0u)") != std::string::npos,
					"Indexed and page-local Vulkan routes must both emit the stable primitive identity contract.");
				context.Expect(meshShader.find("pageId * 16777619u") != std::string::npos,
					"Page-local identities must include physical page ownership so separate resident pages cannot alias.");
				context.Expect(fragmentShader.find("main(\n\tPSIn input,\n\tnointerpolation uint stablePrimitiveId") != std::string::npos,
					"The fragment stage must consume the stable identity interpolator, rather than relying on debug color alone.");
				context.Expect(renderPassSource.find("VK_FORMAT_R32G32B32A32_UINT") != std::string::npos
					&& renderPassSource.find("colorAttachmentRefs.size()") != std::string::npos
					&& offscreenSource.find("VK_FORMAT_R32G32B32A32_UINT") != std::string::npos,
					"Runtime and offscreen conventional passes must declare the typed second identity attachment.");
				context.Expect(implementationSource.find("presentationVisibilityIdentities") != std::string::npos
					&& implementationSource.find("visibilityIdentity") != std::string::npos
					&& renderPassSource.find("DestroyConventionalVisibilityIdentityAttachment") != std::string::npos,
					"Swapchain and offscreen identity images must participate in create/destroy/recreation ownership.");
				context.Expect(pipelineSource.find("conventionalBlendAttachments") != std::string::npos
					&& pipelineSource.find("colorBlending.attachmentCount = conventionalVisibilityPass ? 2u : 1u") != std::string::npos,
					"Conventional pipelines must match two color attachments while target0-only passes retain one.");
				context.Expect(offscreenSource.find("identityBlendAttachment.colorWriteMask = 0u") != std::string::npos
					&& offscreenSource.find("shadowRenderPass") != std::string::npos,
					"Grid, sky, UI, and shadow paths must keep the identity target disabled or absent.");
			});

		registry.Add("VirtualGeometry.Visibility.ConventionalIdentityVariantInstallContract",
			[](TestContext &context)
			{
				const std::string fragmentShader = ReadSource("Engine/Shaders/Default/Default.frag.hlsl");
				const std::string buildScript = ReadSource("CMakeLists.txt");
				context.Expect(
					fragmentShader.find("TERRAENGINE_VULKAN_CONVENTIONAL_VISIBILITY_IDENTITY") != std::string::npos
						&& fragmentShader.find("uint4 identity : SV_Target1") != std::string::npos
						&& buildScript.find("Default.ConventionalVisibilityIdentity.frag.spv") != std::string::npos
						&& buildScript.find("-D TERRAENGINE_VULKAN_CONVENTIONAL_VISIBILITY_IDENTITY=1") != std::string::npos,
					"The installed Vulkan identity variant must have an explicit define, typed second output, and distinct SPIR-V artifact name.");
				context.Expect(
					buildScript.find("Default.ConventionalVisibilityIdentity.frag.dxil") == std::string::npos,
					"The conventional identity variant is Vulkan SPIR-V-only and must not add DXIL output.");
			});
	}
} // namespace TerraEngine::Tests
