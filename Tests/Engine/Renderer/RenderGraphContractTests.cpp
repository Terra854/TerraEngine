#include "TestHarness.h"

#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"

#include <fstream>

namespace TerraEngine::Tests {

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
			std::ifstream graphHeader("Engine/Renderer/Graph/RenderGraph.h");
			std::string contents((std::istreambuf_iterator<char>(graphHeader)), std::istreambuf_iterator<char>());
			context.Expect(contents.find("Vk") == std::string::npos, "Renderer graph public headers should not expose Vulkan symbols.");
			context.Expect(contents.find("D3D") == std::string::npos, "Renderer graph public headers should not expose future backend symbols either.");
		});
	}

}
