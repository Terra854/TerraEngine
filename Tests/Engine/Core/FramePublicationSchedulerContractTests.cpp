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
		});
	}
}
