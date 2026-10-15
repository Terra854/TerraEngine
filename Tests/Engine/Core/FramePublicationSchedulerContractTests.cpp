#include "TestHarness.h"
#include "Animation/AnimationSystem.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Renderer/RenderSystem.h"
#include "Core/Scheduling/SystemScheduler.h"
#include <algorithm>
#include <string_view>

namespace TerraEngine::Tests
{
	void RegisterFramePublicationSchedulerContractTests(TestRegistry& registry)
	{
		registry.Add("Engine.FramePublicationScheduler.DeclarationsExposeCanonicalResources", [](TestContext& context)
		{
			const auto animation = TerraEngine::Animation::AnimationSystem::Instance().SchedulerDeclaration();
			const auto presentation = TerraEngine::Animation::AnimationPresentationSystem::Instance().SchedulerDeclaration();
			const auto renderer = ::RenderSystem::Instance().SchedulerDeclaration();
			const auto rendererWorkloads =
				::RenderSystem::Instance().DeclareFrameWorkloads(0.0f);
			const auto hasResource = [](const auto& values, std::string_view name) {
				return std::find(values.begin(), values.end(), name) != values.end();
			};
			context.Expect(!animation.name.empty() && !presentation.name.empty() && !renderer.name.empty(), "Publication systems must declare stable names.");
			context.Expect(hasResource(animation.writes, TerraEngine::Scheduling::kResourceAnimationPresentationFrame)
				|| hasResource(presentation.writes, TerraEngine::Scheduling::kResourceAnimationPresentationFrame), "Animation publication must declare its presentation resource.");
			const bool rendererReadsPresentation = std::any_of(
				rendererWorkloads.begin(), rendererWorkloads.end(),
				[&hasResource](const auto& workload) {
					return hasResource(workload.reads,
						TerraEngine::Scheduling::kResourceAnimationPresentationFrame);
				});
			context.Expect(rendererReadsPresentation,
				"Renderer workloads must read the animation presentation resource.");
			// Scratch ownership stays inside the same publication chain: capture
			// alone writes storage, and every later stage reads captured values.
			context.Expect(rendererWorkloads.size() == 5U,
				"Scratch migration must preserve the existing five renderer stages.");
			for(std::size_t index = 0U; index < rendererWorkloads.size(); ++index) {
				const auto& workload = rendererWorkloads[index];
				context.Expect(workload.phase == "Render", "Scratch must use the existing Render phase.");
				context.Expect(hasResource(index == 0U ? workload.writes : workload.reads,
					TerraEngine::Scheduling::kResourceRendererFrameScratch),
					"Capture writes scratch and subsequent publication stages declare reads.");
				if(index != 0U) context.Expect(!hasResource(workload.writes,
					TerraEngine::Scheduling::kResourceRendererFrameScratch),
					"Read-only worker/publication stages cannot allocate or reset capture storage.");
			}
		});
	}
}
