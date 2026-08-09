#include "TestHarness.h"

#include <fstream>
#include <sstream>

namespace TerraEngine::Tests
{
	void RegisterAnimationLifecycleRecoveryTests(TestRegistry& registry)
	{
		registry.Add("Animation.LifecycleRecovery.InvalidatesRetainedGenerations", [](TestContext& context)
		{
			std::ifstream input(std::string(TERRAENGINE_SOURCE_DIR) + "/Engine/Animation/AnimationSystem.cpp", std::ios::binary);
			std::ostringstream source; source << input.rdbuf();
			const std::string text = source.str();
			context.Expect(text.find("recovery") != std::string::npos || text.find("rendererGeneration") != std::string::npos, "Animation lifecycle code must account for renderer recovery generations.");
			context.Expect(text.find("reload") != std::string::npos || text.find("resource") != std::string::npos, "Animation lifecycle code must account for reload/resource invalidation.");
			const std::size_t publicInvalidation = text.find(
				"AnimationSystem::NotifyLifecycleInvalidation(");
			const std::size_t retirement = text.find(
				"RetireLifecyclePublicationStateLocked();", publicInvalidation);
			context.Expect(publicInvalidation != std::string::npos
				&& retirement != std::string::npos
				&& text.find("authorityPublisher.Clear(") != std::string::npos
				&& text.find("ResetFrameIntegrationStateLocked();") != std::string::npos
				&& text.find("sealedAuthorityCandidate = {};") != std::string::npos,
				"A public lifecycle boundary must retire the authority root and sealed candidate state, not merely cancel workers.");

			std::ifstream presentationInput(
				std::string(TERRAENGINE_SOURCE_DIR) + "/Engine/Animation/AnimationPresentationSystem.cpp",
				std::ios::binary);
			std::ostringstream presentationSource; presentationSource << presentationInput.rdbuf();
			const std::string presentation = presentationSource.str();
			context.Expect(presentation.find("AnimationPresentationSystem::NotifyLifecycleInvalidation(")
				!= std::string::npos
				&& presentation.find("publishedPlan") != std::string::npos
				&& presentation.find("publishedFrame") != std::string::npos,
				"The matching presentation lifecycle boundary must release both public presentation envelopes.");

			std::ifstream editorInput(std::string(TERRAENGINE_SOURCE_DIR)
				+ "/Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp", std::ios::binary);
			std::ostringstream editorSource; editorSource << editorInput.rdbuf();
			const std::string editor = editorSource.str();
			context.Expect(editor.find("AnimationLifecycleBoundary::Scene") != std::string::npos
				&& editor.find("AnimationLifecycleBoundary::PlayMode") != std::string::npos
				&& editor.find("NotifyLifecycleInvalidation") != std::string::npos,
				"Project open/close and Play Mode boundaries must notify the animation lifecycle before they reuse scene state.");
		});
	}
}
