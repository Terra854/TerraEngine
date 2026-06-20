#include "TestHarness.h"

#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"

#include <filesystem>
#include <fstream>

namespace TerraEngine::Tests {

	namespace {

		std::string ReadRepositoryHeader(const std::filesystem::path& relativePath)
		{
			// CTest runs from the build tree while local executable runs usually
			// start in the repository root. Walking upward keeps the header leak
			// tests stable in both entry points.
			std::filesystem::path probe = std::filesystem::current_path();
			for (int depth = 0; depth < 8; ++depth)
			{
				const std::filesystem::path candidate = probe / relativePath;
				std::ifstream stream(candidate);
				if (stream)
				{
					return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
				}
				if (!probe.has_parent_path())
				{
					break;
				}
				probe = probe.parent_path();
			}
			return {};
		}

	}

	void RegisterRenderGraphContractTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Graph.RequiresOrderedReadWriteDependencies", [](TestContext& context) {
			TerraEngine::Renderer::RenderPassNode writer;
			writer.passId = "writer";
			writer.outputs.push_back({ 42u, TerraEngine::Renderer::RenderAccessMode::Write, TerraEngine::Renderer::RenderStage::Geometry, "writer" });
			TerraEngine::Renderer::RenderPassNode reader;
			reader.passId = "reader";
			reader.inputs.push_back({ 42u, TerraEngine::Renderer::RenderAccessMode::Read, TerraEngine::Renderer::RenderStage::Lighting, "reader" });
			const auto invalid = TerraEngine::Renderer::ValidateRenderGraph({ writer, reader });
			context.Expect(!invalid.valid, "Unordered read/write conflicts should fail graph validation.");
			reader.dependencies.push_back("writer");
			const auto valid = TerraEngine::Renderer::ValidateRenderGraph({ writer, reader });
			context.Expect(valid.valid, "Explicit pass dependencies should order read/write conflicts.");
		});

		registry.Add("Renderer.Graph.RejectsStaleSubmissionFrame", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 10u;
			plan.orderedPasses.push_back({ "draw" });
			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 11u);
			context.Expect(!plan.valid, "Stale frame ids must block final submission.");
		});

		registry.Add("Renderer.Graph.HeadersAvoidBackendNames", [](TestContext& context) {
			const std::string contents = ReadRepositoryHeader("Engine/Renderer/Graph/RenderGraph.h");
			context.Expect(!contents.empty(), "Renderer graph header should be readable from the test working directory.");
			context.Expect(contents.find("Vk") == std::string::npos, "Renderer graph public headers should not expose Vulkan symbols.");
			context.Expect(contents.find("D3D") == std::string::npos, "Renderer graph public headers should not expose future backend symbols either.");
		});

		registry.Add("Renderer.Graph.RhiHeadersExposePortableQueueNames", [](TestContext& context) {
			const std::string contents = ReadRepositoryHeader("Engine/Renderer/RHI/RHIDefinitions.h");
			context.Expect(!contents.empty(), "RHI definitions header should be readable from the test working directory.");
			context.Expect(contents.find("RenderBackendQueueMode") != std::string::npos, "RHI queue readiness should use backend-neutral names.");
		});
	}

}
