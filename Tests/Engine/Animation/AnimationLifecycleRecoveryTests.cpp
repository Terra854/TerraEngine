// Supplemental lifecycle delegation checks; production cancellation/reader tests prove behavior.
#include "TestHarness.h"
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		std::string ReadLifecycleSource(TestContext& context, const char* relative) {
			std::ifstream input(std::string(TERRAENGINE_SOURCE_DIR) + relative, std::ios::binary);
			context.Expect(input.is_open(), "Lifecycle source contract must open its actual production file.");
			std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
			context.Expect(!input.bad() && !source.empty(), "Lifecycle source contract requires nonempty readable bytes.");
			return source;
		}

		std::string LifecycleRegion(TestContext& context, const std::string& source,
			const char* declaration, const char* nextDeclaration) {
			const auto begin = source.find(declaration);
			context.Expect(begin != std::string::npos, "The actual lifecycle definition must exist.");
			const auto end = source.find(nextDeclaration, begin + std::char_traits<char>::length(declaration));
			context.Expect(end != std::string::npos && end > begin, "The lifecycle region requires explicit nonempty bounds.");
			return source.substr(begin, end - begin);
		}
	}

	void RegisterAnimationLifecycleRecoveryTests(TestRegistry& registry) {
		registry.Add("Animation.LifecycleRecovery.InvalidatesRetainedGenerations", [](TestContext& context) {
			const auto text = ReadLifecycleSource(context, "/Engine/Animation/AnimationSystemLifecycle.cpp");
			const auto notify = LifecycleRegion(context, text, "AnimationSystem::NotifyLifecycleInvalidation(", "AnimationSystem::IsAnimatorInstanceAlive(");
			const auto retire = LifecycleRegion(context, text, "AnimationSystem::Impl::RetireLifecyclePublicationStateLocked(", "AnimationSystem::Impl::ActivateRuntimeOwnershipLocked(");
			const auto invalidation = LifecycleRegion(context, text, "AnimationSystem::Impl::InvalidateLifecycleLocked(", "AnimationSystem::Impl::RetireLifecyclePublicationStateLocked(");
			context.Expect(notify.find("AnimationLifecycleBoundary::Renderer") != std::string::npos
				&& notify.find("AnimationLifecycleBoundary::Device") != std::string::npos,
				"Lifecycle handling must distinguish renderer/device recovery from invalidating CPU history.");
			context.Expect(notify.find("impl_->persistentReuse.evaluationCache.InvalidateAll()") != std::string::npos
				&& invalidation.find("lifecycleLatch->Invalidate(boundary)") != std::string::npos,
				"Reload/resource lifecycle changes must invalidate both the boundary generation and retained evaluation cache.");
			context.Expect(notify.find("impl_->RetireLifecyclePublicationStateLocked();") != std::string::npos,
				"The public lifecycle API must delegate retirement to the compiled retirement helper.");
			const auto clear = retire.find("authorityPublisher.Clear(");
			const auto reset = retire.find("ResetFrameIntegrationStateLocked();");
			const auto candidate = retire.find("authorityState.sealedAuthorityCandidate = {};");
			context.Expect(clear != std::string::npos && reset != std::string::npos && candidate != std::string::npos
				&& clear < reset && reset < candidate,
				"Actual retirement must clear public authority before resetting and releasing its sealed candidate.");

			const auto presentationFile = ReadLifecycleSource(context, "/Engine/Animation/AnimationPresentationSystem.cpp");
			const auto presentation = LifecycleRegion(context, presentationFile,
				"AnimationPresentationSystem::NotifyLifecycleInvalidation(", "AnimationPresentationSystem::LoadPublishedGpuAnimationPresentationPlan(");
			context.Expect(presentation.find("&impl_->publishedPlan") != std::string::npos
				&& presentation.find("&impl_->publishedFrame") != std::string::npos
				&& presentation.find("impl_->submittedFrame.reset()") != std::string::npos,
				"The matching presentation definition must release both public envelopes and its submitted frame.");
			const auto editor = ReadLifecycleSource(context, "/Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp");
			context.Expect(editor.find("AnimationLifecycleBoundary::Scene") != std::string::npos
				&& editor.find("AnimationLifecycleBoundary::PlayMode") != std::string::npos
				&& editor.find("NotifyLifecycleInvalidation") != std::string::npos,
				"Editor scene and Play Mode boundaries must retain their animation lifecycle notifications.");
		});
	}
}
